/// @file group_config.cpp
/// @brief NVS persistence + crypto primitives for the group coordination
/// feature. The deterministic string/format logic lives in group_proto.cpp
/// (host-tested); this file adds the hardware RNG, mbedTLS HMAC, and NVS I/O.

#include "group_config.h"

#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "group_proto.h"

namespace hvac_group {

namespace {
const char* TAG = "hvgroup";
constexpr char kNvsNs[] = "hvgroup";

SemaphoreHandle_t s_mtx = nullptr;
GroupConfig       s_cfg;
GroupReplica      s_replica;  // authoritative for label + membership (Phase 6)
std::string       s_self_uid;
uint64_t          s_op_id = 0;  // monotonic resolution op counter (Phase 3)

// Owner-side pairing window. Lives only in RAM (never persisted) and is guarded
// by s_mtx like the config cache.
struct PairSession {
    bool        active = false;
    std::string code;
    int64_t     expires_us = 0;
    int         attempts_left = 0;
};
PairSession s_pair;

// Keep a group label printable and bounded before it hits NVS.
std::string sanitize_label(const std::string& in) {
    std::string out;
    for (char ch : in) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f) continue;
        out.push_back(ch);
        if (out.size() >= 48) break;
    }
    return out;
}

struct Lock {
    Lock()  { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
    ~Lock() { if (s_mtx) xSemaphoreGive(s_mtx); }
};

bool nvs_get_str_std(nvs_handle_t h, const char* key, std::string& out) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return false;
    std::string buf(len, '\0');
    if (nvs_get_str(h, key, buf.data(), &len) != ESP_OK) return false;
    if (!buf.empty() && buf.back() == '\0') buf.pop_back();  // drop NUL terminator
    out = buf;
    return true;
}

// Uniform 0..9 digit via rejection sampling (avoids modulo bias).
char random_digit() {
    for (;;) {
        uint8_t b;
        esp_fill_random(&b, 1);
        if (b < 250) return static_cast<char>('0' + (b % 10));
    }
}

std::string random_hex(size_t nbytes) {
    std::vector<uint8_t> buf(nbytes);
    esp_fill_random(buf.data(), buf.size());
    return to_hex(buf.data(), buf.size());
}

// Validate a config before persisting: non-empty id/secret must be well-formed,
// and no peer may be malformed or equal to this device's own uid.
bool config_ok(const GroupConfig& c) {
    if (!c.group_id.empty() && !is_valid_group_id(c.group_id)) return false;
    if (!c.group_secret.empty() && !is_valid_group_secret(c.group_secret)) return false;
    for (const auto& p : c.peers) {
        if (!is_valid_uid(p)) return false;
        if (p == s_self_uid) return false;
    }
    return true;
}

// ── Replicated-state (CRDT) JSON + projection (device side) ─────────────────
// Compact keys keep the snapshot small enough for the poller's body cap.

std::string serialize_replica_locked(const GroupReplica& r) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "l", static_cast<double>(r.lamport));
    cJSON* lbl = cJSON_AddObjectToObject(root, "lbl");
    cJSON_AddStringToObject(lbl, "v", r.label.value.c_str());
    cJSON_AddNumberToObject(lbl, "ver", static_cast<double>(r.label.version));
    cJSON_AddStringToObject(lbl, "o", r.label.origin.c_str());
    cJSON* ms = cJSON_AddArrayToObject(root, "m");
    for (const auto& m : r.members) {
        cJSON* mo = cJSON_CreateObject();
        cJSON_AddStringToObject(mo, "u", m.uid.c_str());
        cJSON_AddStringToObject(mo, "nv", m.name.value.c_str());
        cJSON_AddNumberToObject(mo, "nver", static_cast<double>(m.name.version));
        cJSON_AddStringToObject(mo, "no", m.name.origin.c_str());
        cJSON_AddNumberToObject(mo, "a", static_cast<double>(m.add_version));
        cJSON_AddItemToArray(ms, mo);
    }
    cJSON* ts = cJSON_AddArrayToObject(root, "t");
    for (const auto& t : r.tombstones) {
        cJSON* to = cJSON_CreateObject();
        cJSON_AddStringToObject(to, "u", t.first.c_str());
        cJSON_AddNumberToObject(to, "v", static_cast<double>(t.second));
        cJSON_AddItemToArray(ts, to);
    }
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    std::string out = s ? s : "";
    if (s) cJSON_free(s);
    return out;
}

// Read a uint64 Lamport clock from a cJSON number (bounded, never negative).
uint64_t json_u64(const cJSON* n) {
    if (!n || !cJSON_IsNumber(n) || n->valuedouble < 0) return 0;
    return static_cast<uint64_t>(n->valuedouble);
}

// Parse untrusted replica JSON, applying the same caps as merge_replica so a
// hostile peer can't blow up memory. Returns false on unparseable input.
bool parse_replica(const std::string& json, GroupReplica& out) {
    out = GroupReplica{};
    if (json.empty()) return false;
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;
    out.lamport = json_u64(cJSON_GetObjectItem(root, "l"));
    if (const cJSON* lbl = cJSON_GetObjectItem(root, "lbl")) {
        const cJSON* v = cJSON_GetObjectItem(lbl, "v");
        const cJSON* o = cJSON_GetObjectItem(lbl, "o");
        out.label.value   = (v && cJSON_IsString(v)) ? sanitize_label(v->valuestring) : "";
        out.label.version = json_u64(cJSON_GetObjectItem(lbl, "ver"));
        out.label.origin  = (o && cJSON_IsString(o) && is_valid_uid(o->valuestring))
                                ? o->valuestring : "";
    }
    if (const cJSON* ms = cJSON_GetObjectItem(root, "m"); ms && cJSON_IsArray(ms)) {
        const cJSON* mo = nullptr;
        cJSON_ArrayForEach(mo, ms) {
            if (out.members.size() >= kMaxReplicaMembers) break;
            const cJSON* u = cJSON_GetObjectItem(mo, "u");
            if (!u || !cJSON_IsString(u) || !is_valid_uid(u->valuestring)) continue;
            ReplicaMember m;
            m.uid = u->valuestring;
            const cJSON* nv = cJSON_GetObjectItem(mo, "nv");
            const cJSON* no = cJSON_GetObjectItem(mo, "no");
            m.name.value   = (nv && cJSON_IsString(nv)) ? sanitize_label(nv->valuestring) : "";
            m.name.version = json_u64(cJSON_GetObjectItem(mo, "nver"));
            m.name.origin  = (no && cJSON_IsString(no) && is_valid_uid(no->valuestring))
                                 ? no->valuestring : "";
            m.add_version  = json_u64(cJSON_GetObjectItem(mo, "a"));
            out.members.push_back(m);
        }
    }
    if (const cJSON* ts = cJSON_GetObjectItem(root, "t"); ts && cJSON_IsArray(ts)) {
        const cJSON* to = nullptr;
        cJSON_ArrayForEach(to, ts) {
            if (out.tombstones.size() >= kMaxReplicaTombstones) break;
            const cJSON* u = cJSON_GetObjectItem(to, "u");
            if (!u || !cJSON_IsString(u) || !is_valid_uid(u->valuestring)) continue;
            out.tombstones.emplace_back(u->valuestring, json_u64(cJSON_GetObjectItem(to, "v")));
        }
    }
    cJSON_Delete(root);
    return true;
}

// Project the authoritative replica onto the legacy GroupConfig fields that the
// rest of the firmware reads (group panel label, poll loop, is_peer).
void reproject_locked() {
    s_cfg.group_label = sanitize_label(s_replica.label.value);
    s_cfg.peers.clear();
    for (const auto& uid : replica_peer_uids(s_replica, s_self_uid))
        if (is_valid_uid(uid)) s_cfg.peers.push_back(uid);
}

// Seed a fresh replica from legacy (pre-CRDT) config or a plain member list —
// used on upgrade and by join fallback. Self is always a member.
void seed_replica_locked(const std::string& label,
                         const std::vector<std::string>& peers) {
    s_replica = GroupReplica{};
    replica_add_member(s_replica, s_self_uid);
    for (const auto& p : peers)
        if (is_valid_uid(p) && p != s_self_uid) replica_add_member(s_replica, p);
    if (!label.empty()) replica_set_label(s_replica, sanitize_label(label), s_self_uid);
}

// True if this head is still a present member of its own replica.
bool self_is_member_locked() {
    for (const auto& m : s_replica.members)
        if (m.uid == s_self_uid) return true;
    return false;
}

esp_err_t persist_locked(const GroupConfig& c) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "gid", c.group_id.c_str());
    nvs_set_str(h, "label", c.group_label.c_str());
    nvs_set_str(h, "secret", c.group_secret.c_str());
    nvs_set_str(h, "peers", serialize_peers(c.peers).c_str());
    nvs_set_str(h, "replica", serialize_replica_locked(s_replica).c_str());
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Persist just the monotonic op_id (kept separate from the group blob so it
// survives a leave_group() and never rolls backward).
esp_err_t persist_op_id_locked(uint64_t v) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u64(h, "op_id", v);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
}  // namespace

esp_err_t init(const std::string& uid) {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    Lock lk;
    s_self_uid = uid;
    GroupConfig c;
    nvs_handle_t h;
    std::string replica_blob;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str_std(h, "gid", c.group_id);
        nvs_get_str_std(h, "label", c.group_label);
        nvs_get_str_std(h, "secret", c.group_secret);
        std::string peers_blob;
        if (nvs_get_str_std(h, "peers", peers_blob))
            c.peers = deserialize_peers(peers_blob);
        nvs_get_str_std(h, "replica", replica_blob);
        nvs_get_u64(h, "op_id", &s_op_id);  // absent → stays 0
        nvs_close(h);
    }
    // Defend against corrupt NVS: a malformed id/secret means "not grouped".
    if (!c.group_id.empty() && !is_valid_group_id(c.group_id)) {
        ESP_LOGW(TAG, "ignoring malformed stored group_id");
        c = GroupConfig{};
        replica_blob.clear();
    }
    // Drop any self/invalid peer that somehow got stored.
    std::vector<std::string> clean;
    for (const auto& p : c.peers)
        if (is_valid_uid(p) && p != s_self_uid) clean.push_back(p);
    c.peers = clean;

    // Establish the authoritative replica. Prefer the persisted CRDT blob; on a
    // firmware upgrade (no blob yet) synthesize one from the legacy fields so
    // membership/label carry forward, then persist it for next boot.
    bool need_persist = false;
    if (c.group_id.empty()) {
        s_replica = GroupReplica{};
    } else if (parse_replica(replica_blob, s_replica) && self_is_member_locked()) {
        // adopted persisted replica
    } else {
        seed_replica_locked(c.group_label, c.peers);
        need_persist = true;
    }
    s_cfg = c;
    if (!c.group_id.empty()) reproject_locked();  // keep peers/label consistent with replica
    if (need_persist) persist_locked(s_cfg);
    ESP_LOGI(TAG, "loaded: %s (%u peer(s))",
             s_cfg.group_id.empty() ? "standalone" : s_cfg.group_id.c_str(),
             static_cast<unsigned>(s_cfg.peers.size()));
    return ESP_OK;
}

std::string self_uid() { Lock lk; return s_self_uid; }

GroupConfig get() { Lock lk; return s_cfg; }

bool in_group() {
    Lock lk;
    return is_valid_group_id(s_cfg.group_id) && is_valid_group_secret(s_cfg.group_secret);
}

esp_err_t save(const GroupConfig& cfg) {
    Lock lk;
    if (!config_ok(cfg)) return ESP_ERR_INVALID_ARG;
    // Rebuild the authoritative replica from the supplied config so the CRDT
    // stays the single source of truth for label + membership.
    if (cfg.group_id.empty()) s_replica = GroupReplica{};
    else                      seed_replica_locked(cfg.group_label, cfg.peers);
    GroupConfig c = cfg;
    if (!cfg.group_id.empty()) {
        c.group_label = sanitize_label(cfg.group_label);
        c.peers       = replica_peer_uids(s_replica, s_self_uid);
    }
    esp_err_t err = persist_locked(c);
    if (err == ESP_OK) s_cfg = c;
    return err;
}

esp_err_t set_label(const std::string& label) {
    Lock lk;
    if (s_cfg.group_id.empty()) return ESP_ERR_INVALID_STATE;
    replica_set_label(s_replica, sanitize_label(label), s_self_uid);
    reproject_locked();
    return persist_locked(s_cfg);
}

esp_err_t leave_group() {
    Lock lk;
    s_replica = GroupReplica{};
    GroupConfig empty;
    esp_err_t err = persist_locked(empty);
    if (err == ESP_OK) s_cfg = empty;
    return err;
}

std::string replica_json() {
    Lock lk;
    if (s_cfg.group_id.empty()) return "";
    return serialize_replica_locked(s_replica);
}

bool merge_remote_json(const std::string& json) {
    Lock lk;
    if (s_cfg.group_id.empty()) return false;
    GroupReplica remote;
    if (!parse_replica(json, remote)) return false;
    if (!merge_replica(s_replica, remote)) return false;
    // If an admin on another head evicted us, we're no longer a member — drop
    // the group entirely (mirrors leave_group) so we stop coordinating.
    if (!self_is_member_locked()) {
        s_replica = GroupReplica{};
        GroupConfig empty;
        if (persist_locked(empty) == ESP_OK) s_cfg = empty;
        ESP_LOGW(TAG, "evicted from group by peer; left");
        return true;
    }
    reproject_locked();
    persist_locked(s_cfg);
    return true;
}

std::string member_display_name(const std::string& uid) {
    Lock lk;
    return replica_member_name(s_replica, uid);
}

void note_self_name(const std::string& name, bool seed_only) {
    Lock lk;
    if (s_cfg.group_id.empty()) return;
    if (seed_only && !replica_member_name(s_replica, s_self_uid).empty())
        return;  // don't overwrite an existing (possibly admin-set) name on boot.
    if (replica_set_name(s_replica, s_self_uid, sanitize_label(name), s_self_uid)) {
        reproject_locked();
        persist_locked(s_cfg);
    }
}

esp_err_t set_member_name(const std::string& uid, const std::string& name) {
    Lock lk;
    if (s_cfg.group_id.empty()) return ESP_ERR_INVALID_STATE;
    if (replica_set_name(s_replica, uid, sanitize_label(name), s_self_uid)) {
        reproject_locked();
        return persist_locked(s_cfg);
    }
    return ESP_OK;  // absent / unchanged is not an error
}

esp_err_t remove_member(const std::string& uid) {
    Lock lk;
    if (s_cfg.group_id.empty()) return ESP_ERR_INVALID_STATE;
    if (uid == s_self_uid) return ESP_ERR_INVALID_ARG;  // use leave_group() instead
    if (replica_remove_member(s_replica, uid)) {
        reproject_locked();
        return persist_locked(s_cfg);
    }
    return ESP_OK;  // not a member is a no-op
}

std::string generate_group_id()     { return random_hex(kGroupIdHexLen / 2); }
std::string generate_group_secret() { return random_hex(kGroupSecretHexLen / 2); }

std::string generate_pairing_code() {
    std::string code;
    code.reserve(kPairingCodeLen);
    for (size_t i = 0; i < kPairingCodeLen; ++i) code.push_back(random_digit());
    return code;
}

std::string generate_nonce() { return random_hex(8); }  // 64-bit → 16 hex chars

bool is_peer(const std::string& uid) {
    Lock lk;
    for (const auto& p : s_cfg.peers)
        if (p == uid) return true;
    return false;
}

uint64_t last_op_id() { Lock lk; return s_op_id; }

uint64_t issue_op_id() {
    Lock lk;
    ++s_op_id;
    persist_op_id_locked(s_op_id);  // best-effort; cache is authoritative
    return s_op_id;
}

bool accept_op_id(uint64_t incoming) {
    Lock lk;
    if (incoming <= s_op_id) return false;
    s_op_id = incoming;
    persist_op_id_locked(s_op_id);
    return true;
}

std::string hmac_hex(const std::string& message) {
    std::string secret_hex;
    { Lock lk; secret_hex = s_cfg.group_secret; }
    std::vector<uint8_t> key;
    if (!is_valid_group_secret(secret_hex) || !from_hex(secret_hex, key)) return "";

    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return "";
    uint8_t out[32];
    if (mbedtls_md_hmac(info, key.data(), key.size(),
                        reinterpret_cast<const uint8_t*>(message.data()),
                        message.size(), out) != 0) {
        return "";
    }
    return to_hex(out, sizeof(out));
}

bool hmac_verify(const std::string& message, const std::string& provided_hex) {
    const std::string expected = hmac_hex(message);
    if (expected.empty() || expected.size() != provided_hex.size()) return false;
    // Constant-time compare so a timing side-channel can't leak the signature.
    uint8_t diff = 0;
    for (size_t i = 0; i < expected.size(); ++i)
        diff |= static_cast<uint8_t>(expected[i] ^ provided_hex[i]);
    return diff == 0;
}

esp_err_t pairing_start(const std::string& label, std::string& out_code) {
    Lock lk;
    // Form a group on first pairing: mint a random id + secret so the owner has
    // something to hand out. An already-grouped head keeps its existing id.
    if (!is_valid_group_id(s_cfg.group_id) || !is_valid_group_secret(s_cfg.group_secret)) {
        GroupReplica prev_rep = s_replica;
        GroupConfig  prev_cfg = s_cfg;
        GroupConfig c;
        c.group_id     = generate_group_id();
        c.group_secret = generate_group_secret();
        // Seed the authoritative replica with this owner as the sole member plus
        // the initial label, so subsequent joiners inherit a consistent state.
        seed_replica_locked(sanitize_label(label), {});
        s_cfg = c;
        reproject_locked();
        esp_err_t err = persist_locked(s_cfg);
        if (err != ESP_OK) { s_replica = prev_rep; s_cfg = prev_cfg; return err; }
    }
    s_pair.active        = true;
    s_pair.code          = generate_pairing_code();
    s_pair.expires_us    = esp_timer_get_time() +
                           static_cast<int64_t>(kPairingTtlSeconds) * 1000000;
    s_pair.attempts_left = kPairingMaxAttempts;
    out_code = s_pair.code;
    ESP_LOGI(TAG, "pairing window open (%ds, %d attempts)",
             kPairingTtlSeconds, kPairingMaxAttempts);
    return ESP_OK;
}

void pairing_stop() {
    Lock lk;
    s_pair = PairSession{};
}

PairingStatus pairing_status() {
    Lock lk;
    PairingStatus st;
    if (!s_pair.active) return st;
    const int64_t now = esp_timer_get_time();
    if (now >= s_pair.expires_us) {  // lazily reap an expired window
        s_pair = PairSession{};
        return st;
    }
    st.active        = true;
    st.seconds_left  = static_cast<int>((s_pair.expires_us - now) / 1000000);
    st.attempts_left = s_pair.attempts_left;
    return st;
}

ClaimOutcome pairing_claim(const std::string& code, const std::string& joiner_uid) {
    Lock lk;
    ClaimOutcome out;

    const int64_t now     = esp_timer_get_time();
    const bool    active  = s_pair.active;
    const bool    expired = active && now >= s_pair.expires_us;
    const bool    matches = active && ct_equal(code, s_pair.code);

    out.decision = evaluate_claim(active, expired, matches, s_pair.attempts_left);

    switch (out.decision) {
        case ClaimDecision::Ok: {
            // A malformed joiner uid can't be enrolled; treat as a bad code so we
            // don't leak the group secret to a garbage identity.
            if (!is_valid_uid(joiner_uid) || joiner_uid == s_self_uid) {
                out.decision = ClaimDecision::BadCode;
                if (--s_pair.attempts_left <= 0) s_pair = PairSession{};
                break;
            }
            // The joiner needs everyone already in the group: this owner plus any
            // existing peers (computed BEFORE we add the joiner).
            out.group_id     = s_cfg.group_id;
            out.group_label  = s_cfg.group_label;
            out.group_secret = s_cfg.group_secret;
            out.members.push_back(s_self_uid);
            for (const auto& p : s_cfg.peers) out.members.push_back(p);

            // Enroll the joiner into the authoritative replica (OR-Set add), then
            // reproject + persist. Snapshot so a persist failure fully rolls back.
            GroupReplica prev = s_replica;
            replica_add_member(s_replica, joiner_uid);
            reproject_locked();
            if (persist_locked(s_cfg) != ESP_OK) {
                // Persist failed: report as if no code was active and leave the
                // window open so a retry can succeed. Don't hand out the secret.
                s_replica = prev;
                reproject_locked();
                out = ClaimOutcome{};
                out.decision = ClaimDecision::NoActiveCode;
                break;
            }
            // Hand the joiner our exact replicated state so membership/labels are
            // immediately consistent (includes the joiner we just added).
            out.replica_json = serialize_replica_locked(s_replica);
            s_pair = PairSession{};  // single-use: burn on success
            ESP_LOGI(TAG, "enrolled peer %s", joiner_uid.c_str());
            break;
        }
        case ClaimDecision::BadCode:
            if (--s_pair.attempts_left <= 0) s_pair = PairSession{};
            break;
        case ClaimDecision::Expired:
        case ClaimDecision::LockedOut:
            s_pair = PairSession{};  // window is dead; clear it
            break;
        case ClaimDecision::NoActiveCode:
            break;
    }
    return out;
}

esp_err_t join_group(const std::string& group_id, const std::string& label,
                     const std::string& secret,
                     const std::vector<std::string>& members,
                     const std::string& replica_json) {
    if (!is_valid_group_id(group_id) || !is_valid_group_secret(secret))
        return ESP_ERR_INVALID_ARG;

    Lock lk;
    const std::string self = s_self_uid;

    // Build the replica we'll adopt into a local first, so a validation failure
    // leaves existing state untouched.
    GroupReplica newrep;
    if (!replica_json.empty() && parse_replica(replica_json, newrep)) {
        // Prefer the owner's exact CRDT state; make sure we're in it.
        bool self_present = false;
        for (const auto& m : newrep.members)
            if (m.uid == self) { self_present = true; break; }
        if (!self_present) replica_add_member(newrep, self);
    } else {
        // Fallback: synthesize from the plain member list + label.
        for (const auto& m : members) {
            if (m == self) continue;
            if (!is_valid_uid(m)) return ESP_ERR_INVALID_ARG;
        }
        replica_add_member(newrep, self);
        for (const auto& m : members)
            if (is_valid_uid(m) && m != self) replica_add_member(newrep, m);
        if (!label.empty()) replica_set_label(newrep, sanitize_label(label), self);
    }

    s_replica       = newrep;
    s_cfg           = GroupConfig{};
    s_cfg.group_id  = group_id;
    s_cfg.group_secret = secret;
    reproject_locked();  // fills label + peers from the adopted replica
    return persist_locked(s_cfg);
}

}  // namespace hvac_group

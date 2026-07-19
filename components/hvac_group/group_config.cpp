/// @file group_config.cpp
/// @brief NVS persistence + crypto primitives for the group coordination
/// feature. The deterministic string/format logic lives in group_proto.cpp
/// (host-tested); this file adds the hardware RNG, mbedTLS HMAC, and NVS I/O.

#include "group_config.h"

#include <cstring>

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
std::string       s_self_uid;

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

esp_err_t persist_locked(const GroupConfig& c) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "gid", c.group_id.c_str());
    nvs_set_str(h, "label", c.group_label.c_str());
    nvs_set_str(h, "secret", c.group_secret.c_str());
    nvs_set_str(h, "peers", serialize_peers(c.peers).c_str());
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
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str_std(h, "gid", c.group_id);
        nvs_get_str_std(h, "label", c.group_label);
        nvs_get_str_std(h, "secret", c.group_secret);
        std::string peers_blob;
        if (nvs_get_str_std(h, "peers", peers_blob))
            c.peers = deserialize_peers(peers_blob);
        nvs_close(h);
    }
    // Defend against corrupt NVS: a malformed id/secret means "not grouped".
    if (!c.group_id.empty() && !is_valid_group_id(c.group_id)) {
        ESP_LOGW(TAG, "ignoring malformed stored group_id");
        c = GroupConfig{};
    }
    // Drop any self/invalid peer that somehow got stored.
    std::vector<std::string> clean;
    for (const auto& p : c.peers)
        if (is_valid_uid(p) && p != s_self_uid) clean.push_back(p);
    c.peers = clean;
    s_cfg = c;
    ESP_LOGI(TAG, "loaded: %s (%u peer(s))",
             c.group_id.empty() ? "standalone" : c.group_id.c_str(),
             static_cast<unsigned>(c.peers.size()));
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
    esp_err_t err = persist_locked(cfg);
    if (err == ESP_OK) s_cfg = cfg;
    return err;
}

esp_err_t set_label(const std::string& label) {
    Lock lk;
    if (s_cfg.group_id.empty()) return ESP_ERR_INVALID_STATE;
    GroupConfig c = s_cfg;
    c.group_label = sanitize_label(label);
    esp_err_t err = persist_locked(c);
    if (err == ESP_OK) s_cfg = c;
    return err;
}

esp_err_t leave_group() {
    Lock lk;
    GroupConfig empty;
    esp_err_t err = persist_locked(empty);
    if (err == ESP_OK) s_cfg = empty;
    return err;
}

std::string generate_group_id()     { return random_hex(kGroupIdHexLen / 2); }
std::string generate_group_secret() { return random_hex(kGroupSecretHexLen / 2); }

std::string generate_pairing_code() {
    std::string code;
    code.reserve(kPairingCodeLen);
    for (size_t i = 0; i < kPairingCodeLen; ++i) code.push_back(random_digit());
    return code;
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
        GroupConfig c;
        c.group_id     = generate_group_id();
        c.group_secret = generate_group_secret();
        c.group_label  = sanitize_label(label);
        esp_err_t err = persist_locked(c);
        if (err != ESP_OK) return err;
        s_cfg = c;
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

            GroupConfig c = s_cfg;
            add_peer(c.peers, joiner_uid, s_self_uid);
            if (persist_locked(c) != ESP_OK) {
                // Persist failed: report as if no code was active and leave the
                // window open so a retry can succeed. Don't hand out the secret.
                out = ClaimOutcome{};
                out.decision = ClaimDecision::NoActiveCode;
                break;
            }
            s_cfg  = c;
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
                     const std::vector<std::string>& members) {
    if (!is_valid_group_id(group_id) || !is_valid_group_secret(secret))
        return ESP_ERR_INVALID_ARG;

    const std::string self = self_uid();  // takes the lock itself; call before Lock

    GroupConfig c;
    c.group_id     = group_id;
    c.group_label  = sanitize_label(label);
    c.group_secret = secret;
    for (const auto& m : members) {
        if (m == self) continue;
        if (!is_valid_uid(m)) return ESP_ERR_INVALID_ARG;
        add_peer(c.peers, m, self);
    }
    return save(c);  // save() validates + persists + updates cache under the lock
}

}  // namespace hvac_group

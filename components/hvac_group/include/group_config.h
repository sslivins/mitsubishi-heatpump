/// @file group_config.h
/// @brief Persistent identity + membership for the shared-compressor group
/// coordination feature (Phase 1). Stores the group_id, human label, the shared
/// group_secret used to HMAC-sign peer requests, and the enrolled-peer uid list
/// in NVS, and exposes the crypto primitives (secure random id/code/secret
/// generation and HMAC-SHA256) that the pure helpers in group_proto.h cannot
/// provide on their own. A head with an empty group_id is "standalone" and pays
/// no coordination cost.
#pragma once

#include <string>
#include <vector>

#include "esp_err.h"

#include "group_proto.h"

namespace hvac_group {

/// Everything that defines this head's participation in a compressor group.
struct GroupConfig {
    std::string group_id;      ///< 32 lowercase hex chars, or "" when standalone.
    std::string group_label;   ///< display-only, e.g. "Upstairs system".
    std::string group_secret;  ///< 64 lowercase hex chars (256-bit), or "".
    std::vector<std::string> peers;  ///< enrolled sibling device_uids (not self).
};

/// Load persisted group config from NVS and cache it. @p self_uid is this
/// device's immutable id (from wifi::device_uid()); it is never added to the
/// peer list and is used to bind signatures. Safe to call once at boot.
esp_err_t init(const std::string& self_uid);

/// This device's immutable uid (as passed to init()).
std::string self_uid();

/// Cached copy of the current config (thread-safe).
GroupConfig get();

/// True when this head is enrolled in a group (valid group_id AND secret).
bool in_group();

/// Persist @p cfg to NVS and update the cache. Rejects (ESP_ERR_INVALID_ARG) a
/// config whose non-empty group_id/secret are malformed, or whose peer list
/// contains an invalid uid or this device's own uid.
esp_err_t save(const GroupConfig& cfg);

/// Update only the display label (must already be in a group). Routed through
/// the replicated LWW register so the new label propagates to every peer.
esp_err_t set_label(const std::string& label);

/// Leave the group: clears group_id/secret/label/peers and persists. Idempotent.
esp_err_t leave_group();

// ── Replicated group state (Phase 6: eventual consistency via gossip) ──────
// The label, per-head display names, and membership are a small set of CRDTs
// (group_proto.h) reconciled over the signed peer-poll. These accessors bridge
// that pure core to NVS (JSON blob) and to the gossip transport in web_ui.

/// Serialize this head's replicated group state (label + member names +
/// membership + tombstones) to a compact JSON string for the signed snapshot.
/// Returns "" when standalone.
std::string replica_json();

/// Merge a peer's replicated-state JSON (from its polled snapshot) into ours.
/// Reprojects peers/label and persists on any change. If the merge reveals this
/// head has been evicted (self removed by an admin elsewhere), it leaves the
/// group. Returns true if our state changed.
bool merge_remote_json(const std::string& json);

/// Display name currently recorded for @p uid in the replica ("" if none).
std::string member_display_name(const std::string& uid);

/// Record this head's own current display name into the replica so peers learn
/// it. No-op if unchanged or standalone. Call at boot/join and on every device
/// rename. This is the sole writer of a head's name: a head is named only by
/// its own device display name, so there is no separate admin-rename authority.
void note_self_name(const std::string& name);

/// Admin: remove a member from the group (OR-Set remove → tombstone). The
/// evicted head learns of it on its next poll and drops the group. Rejects an
/// attempt to remove this head itself (use leave_group()).
esp_err_t remove_member(const std::string& uid);

// ── Crypto primitives (hardware RNG + mbedTLS) ─────────────────────────────

/// 128-bit cryptographically-random group_id as 32 lowercase hex chars.
std::string generate_group_id();

/// 256-bit cryptographically-random group_secret as 64 lowercase hex chars.
std::string generate_group_secret();

/// A fresh kPairingCodeLen-digit pairing code (uniform, hardware RNG).
std::string generate_pairing_code();

/// HMAC-SHA256 of @p message under the current group_secret, lowercase hex.
/// Returns "" if the head has no valid secret.
std::string hmac_hex(const std::string& message);

/// Constant-time comparison of an expected HMAC hex against a provided one.
bool hmac_verify(const std::string& message, const std::string& provided_hex);

/// Generate a fresh 16-hex-char (64-bit) nonce for signing a peer request.
std::string generate_nonce();

/// True if @p uid is currently an enrolled peer of this head.
bool is_peer(const std::string& uid);

// ── Resolution ordering (Phase 3) ──────────────────────────────────────────
// A monotonic op_id, persisted in NVS, that totally orders resolution ops so a
// rebooting/late peer can reject a stale or replayed op. The counter is
// per-head and survives leave/rejoin (never decreases) so an old op_id can
// never be replayed after a group is torn down and re-formed.

/// The highest op_id this head has issued or accepted (0 if none yet).
uint64_t last_op_id();

/// Coordinator side: allocate the next op_id (bumps + persists) and return it.
/// The returned value is strictly greater than every previously issued/accepted
/// op_id on this head.
uint64_t issue_op_id();

/// Receiver side: accept @p incoming only if it is strictly greater than the
/// last seen op_id. On acceptance the counter advances and is persisted and the
/// function returns true; a stale/replayed (≤ last) op_id returns false and
/// changes nothing.
bool accept_op_id(uint64_t incoming);

// ── Pairing (Phase 1b) ─────────────────────────────────────────────────────

/// Seconds a pairing code stays valid, and how many wrong attempts burn it.
constexpr int kPairingTtlSeconds = 120;
constexpr int kPairingMaxAttempts = 5;

/// Owner side: ensure this head has a group (generating a random group_id +
/// group_secret if it was standalone), then open a single-use pairing window
/// with a fresh 6-digit code. @p label sets the group label only when a new
/// group is formed. The code is returned to display to the user; it lives in
/// RAM (never persisted) and expires after kPairingTtlSeconds.
esp_err_t pairing_start(const std::string& label, std::string& out_code);

/// Cancel any open pairing window.
void pairing_stop();

struct PairingStatus {
    bool active = false;
    int  seconds_left = 0;
    int  attempts_left = 0;
};

/// Current pairing-window state (an expired window reports inactive).
PairingStatus pairing_status();

/// Result of an inbound claim; on decision==Ok the out-params carry exactly
/// what the joining head needs to adopt the group.
struct ClaimOutcome {
    ClaimDecision decision = ClaimDecision::NoActiveCode;
    std::string group_id;
    std::string group_label;
    std::string group_secret;
    std::vector<std::string> members;  ///< owner uid + existing peers (not joiner)
    std::string replica_json;          ///< owner's replicated state for the joiner to adopt
};

/// Owner side: validate an inbound pairing claim from @p joiner_uid. On success
/// enrolls the joiner into the local peer table (persisted), burns the code,
/// and fills the outcome with the group_id/label/secret/members. A wrong code
/// decrements the attempt counter (and burns the code at zero).
ClaimOutcome pairing_claim(const std::string& code, const std::string& joiner_uid);

/// Joiner side: adopt a group from a successful claim response. Validates all
/// fields, stores group_id/label/secret, and sets the peer list to @p members
/// minus this device's own uid. When @p replica_json is non-empty and parses,
/// the joiner adopts the owner's exact replicated state (versions, names,
/// tombstones) so membership/labels are immediately consistent; otherwise it
/// synthesizes a fresh replica from @p members and @p label. Returns
/// ESP_ERR_INVALID_ARG on malformed input.
esp_err_t join_group(const std::string& group_id, const std::string& label,
                     const std::string& secret,
                     const std::vector<std::string>& members,
                     const std::string& replica_json = "");

}  // namespace hvac_group

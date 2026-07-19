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

/// Update only the display label (must already be in a group).
esp_err_t set_label(const std::string& label);

/// Leave the group: clears group_id/secret/label/peers and persists. Idempotent.
esp_err_t leave_group();

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
};

/// Owner side: validate an inbound pairing claim from @p joiner_uid. On success
/// enrolls the joiner into the local peer table (persisted), burns the code,
/// and fills the outcome with the group_id/label/secret/members. A wrong code
/// decrements the attempt counter (and burns the code at zero).
ClaimOutcome pairing_claim(const std::string& code, const std::string& joiner_uid);

/// Joiner side: adopt a group from a successful claim response. Validates all
/// fields, stores group_id/label/secret, and sets the peer list to @p members
/// minus this device's own uid. Returns ESP_ERR_INVALID_ARG on malformed input.
esp_err_t join_group(const std::string& group_id, const std::string& label,
                     const std::string& secret,
                     const std::vector<std::string>& members);

}  // namespace hvac_group

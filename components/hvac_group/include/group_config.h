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

}  // namespace hvac_group

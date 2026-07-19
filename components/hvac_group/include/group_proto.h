/// @file group_proto.h
/// @brief Pure (ESP-IDF-free) helpers for the shared-compressor group protocol:
/// identity validation, canonical HMAC signing-string construction, hex
/// encoding, and enrolled-peer (de)serialization. Kept dependency-free so the
/// security-relevant string/format logic is unit-testable on the host
/// (see test/host/test_group_proto.cpp). The randomness (group-id / pairing
/// code generation) and the HMAC primitive itself live in the ESP-only
/// group_config.cpp; everything here is deterministic.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hvac_group {

/// Length, in hex characters, of a group_id (128-bit) and of a group_secret
/// (256-bit) when rendered as lowercase hex.
constexpr size_t kGroupIdHexLen  = 32;  // 16 bytes
constexpr size_t kGroupSecretHexLen = 64;  // 32 bytes

/// Number of decimal digits in a pairing code shown/entered during join.
constexpr size_t kPairingCodeLen = 6;

/// Wire protocol version, advertised in mDNS TXT + peer messages. Heads with an
/// incompatible value are shown but excluded from automated resolution.
constexpr int kProtocolVersion = 1;

/// Lowercase-hex encode a byte buffer.
std::string to_hex(const uint8_t* data, size_t len);

/// Parse a lowercase/uppercase hex string into bytes. Returns false (and leaves
/// @p out empty) if the input has an odd length or any non-hex character.
bool from_hex(const std::string& hex, std::vector<uint8_t>& out);

/// A group_id is exactly kGroupIdHexLen lowercase hex characters. Anything else
/// (empty, wrong length, uppercase, non-hex) is invalid — used to reject
/// corrupt NVS and malformed peer messages.
bool is_valid_group_id(const std::string& id);

/// A group_secret is exactly kGroupSecretHexLen lowercase hex characters.
bool is_valid_group_secret(const std::string& secret);

/// A pairing code is exactly kPairingCodeLen ASCII decimal digits.
bool is_valid_pairing_code(const std::string& code);

/// A device_uid is 1..32 characters from [a-z0-9] (the firmware derives it from
/// the MAC as lowercase hex, but we accept any short alnum id defensively).
/// Notably it can never contain the '\n' field separator used below.
bool is_valid_uid(const std::string& uid);

/// Build the canonical string that a peer-to-peer request is HMAC-signed over.
/// Binds the message to sender, *receiver* (so a signature can't be replayed at
/// a different head), group, and a monotonic op_id/nonce (so it can't be
/// replayed in time). Fields are '\n'-joined; sender/receiver/group_id/nonce are
/// restricted charsets that cannot contain '\n', and the free-form @p body is
/// placed last so its contents can never shift an earlier field boundary.
std::string signing_string(const std::string& sender_uid,
                           const std::string& receiver_uid,
                           const std::string& group_id,
                           const std::string& nonce,
                           const std::string& body);

/// Serialize an enrolled-peer uid list to a compact NVS blob string. UIDs are
/// alnum (comma-free), so a comma-separated join is unambiguous. Empty/blank
/// entries are dropped and duplicates collapsed, preserving first-seen order.
std::string serialize_peers(const std::vector<std::string>& uids);

/// Inverse of serialize_peers. Ignores empty tokens and duplicates.
std::vector<std::string> deserialize_peers(const std::string& blob);

/// Add @p uid to @p uids if valid and not already present. Returns true if the
/// list changed. Never adds @p self_uid (a head is not its own peer).
bool add_peer(std::vector<std::string>& uids, const std::string& uid,
              const std::string& self_uid);

/// Remove @p uid from @p uids if present. Returns true if the list changed.
bool remove_peer(std::vector<std::string>& uids, const std::string& uid);

}  // namespace hvac_group

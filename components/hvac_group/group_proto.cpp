/// @file group_proto.cpp
/// @brief Implementation of the pure group-protocol helpers declared in
/// group_proto.h. No ESP-IDF dependencies — host-testable.

#include "group_proto.h"

namespace hvac_group {

namespace {
bool is_lower_hex(const std::string& s, size_t expect_len) {
    if (s.size() != expect_len) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}
}  // namespace

std::string to_hex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

bool from_hex(const std::string& hex, std::vector<uint8_t>& out) {
    out.clear();
    if (hex.size() % 2 != 0) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]);
        const int lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) { out.clear(); return false; }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

bool is_valid_group_id(const std::string& id) {
    return is_lower_hex(id, kGroupIdHexLen);
}

bool is_valid_group_secret(const std::string& secret) {
    return is_lower_hex(secret, kGroupSecretHexLen);
}

bool is_valid_pairing_code(const std::string& code) {
    if (code.size() != kPairingCodeLen) return false;
    for (char c : code)
        if (c < '0' || c > '9') return false;
    return true;
}

bool is_valid_uid(const std::string& uid) {
    if (uid.empty() || uid.size() > 32) return false;
    for (char c : uid) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!ok) return false;
    }
    return true;
}

std::string signing_string(const std::string& sender_uid,
                           const std::string& receiver_uid,
                           const std::string& group_id,
                           const std::string& nonce,
                           const std::string& body) {
    std::string s;
    s.reserve(sender_uid.size() + receiver_uid.size() + group_id.size() +
              nonce.size() + body.size() + 4);
    s += sender_uid;
    s += '\n';
    s += receiver_uid;
    s += '\n';
    s += group_id;
    s += '\n';
    s += nonce;
    s += '\n';
    s += body;
    return s;
}

std::string serialize_peers(const std::vector<std::string>& uids) {
    std::string out;
    std::vector<std::string> seen;
    for (const auto& u : uids) {
        if (u.empty()) continue;
        bool dup = false;
        for (const auto& s : seen)
            if (s == u) { dup = true; break; }
        if (dup) continue;
        seen.push_back(u);
        if (!out.empty()) out.push_back(',');
        out += u;
    }
    return out;
}

std::vector<std::string> deserialize_peers(const std::string& blob) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty()) return;
        for (const auto& s : out)
            if (s == cur) { cur.clear(); return; }
        out.push_back(cur);
        cur.clear();
    };
    for (char c : blob) {
        if (c == ',') flush();
        else cur.push_back(c);
    }
    flush();
    return out;
}

bool add_peer(std::vector<std::string>& uids, const std::string& uid,
              const std::string& self_uid) {
    if (!is_valid_uid(uid) || uid == self_uid) return false;
    for (const auto& u : uids)
        if (u == uid) return false;
    uids.push_back(uid);
    return true;
}

bool remove_peer(std::vector<std::string>& uids, const std::string& uid) {
    for (size_t i = 0; i < uids.size(); ++i) {
        if (uids[i] == uid) {
            uids.erase(uids.begin() + i);
            return true;
        }
    }
    return false;
}

bool ct_equal(const std::string& a, const std::string& b) {
    // Fold the length difference into the accumulator and always scan all of
    // `a`, so neither the running result nor the loop trip-count reveals where
    // (or whether) the strings first diverge.
    uint8_t diff = static_cast<uint8_t>(a.size() ^ b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        const char bc = (i < b.size()) ? b[i] : 0;
        diff |= static_cast<uint8_t>(a[i] ^ bc);
    }
    return diff == 0 && a.size() == b.size();
}

ClaimDecision evaluate_claim(bool active, bool expired, bool code_matches,
                             int attempts_left) {
    if (!active)             return ClaimDecision::NoActiveCode;
    if (expired)             return ClaimDecision::Expired;
    if (attempts_left <= 0)  return ClaimDecision::LockedOut;
    if (code_matches)        return ClaimDecision::Ok;
    return ClaimDecision::BadCode;
}

}  // namespace hvac_group

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

bool is_valid_nonce(const std::string& nonce) {
    if (nonce.empty() || nonce.size() > 32) return false;
    for (char c : nonce) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
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

namespace {
std::string upper(const std::string& s) {
    std::string o = s;
    for (char& c : o)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return o;
}
}  // namespace

Demand classify_demand(const std::string& power, const std::string& mode) {
    if (upper(power) != "ON") return Demand::Neutral;
    const std::string m = upper(mode);
    if (m == "HEAT") return Demand::Heat;
    if (m == "COOL" || m == "DRY") return Demand::Cool;
    if (m == "AUTO") return Demand::Auto;
    return Demand::Neutral;  // FAN, OFF, or anything unrecognized
}

MemberObs observe(const std::string& uid, const std::string& name,
                  const std::string& power, const std::string& mode,
                  bool operating, const std::string& sub_mode,
                  const std::string& stage) {
    MemberObs m;
    m.uid        = uid;
    m.name       = name;
    m.state      = MemberState::Known;
    m.demand     = classify_demand(power, mode);
    m.power_on   = (upper(power) == "ON");
    m.active_now = operating;
    // The losing head on a shared compressor parks in STANDBY/IDLE with the
    // compressor not running for it — the authoritative "my mode is blocked".
    m.standby    = (upper(sub_mode) == "STANDBY") && (upper(stage) == "IDLE") &&
                   !operating;
    return m;
}

Demand opposite(Demand d) {
    if (d == Demand::Heat) return Demand::Cool;
    if (d == Demand::Cool) return Demand::Heat;
    return Demand::Neutral;
}

const char* demand_str(Demand d) {
    switch (d) {
        case Demand::Heat: return "HEAT";
        case Demand::Cool: return "COOL";
        case Demand::Auto: return "AUTO";
        case Demand::Neutral: default: return "OFF";
    }
}

const char* status_str(GroupStatus s) {
    switch (s) {
        case GroupStatus::Ok:              return "ok";
        case GroupStatus::PendingConflict: return "pending_conflict";
        case GroupStatus::Conflict:        return "conflict";
        case GroupStatus::Indeterminate:   return "indeterminate";
        case GroupStatus::Standalone: default: return "standalone";
    }
}

GroupView evaluate_group(const std::vector<MemberObs>& members) {
    GroupView v;

    // No enrolled peers ⇒ coordination is disabled. (members[0] is self.)
    if (members.size() <= 1) {
        v.status = GroupStatus::Standalone;
        // Still surface a self lock display if this lone head is running.
        if (!members.empty() && members[0].active_now &&
            members[0].demand != Demand::Neutral && members[0].demand != Demand::Auto) {
            v.locked_mode   = members[0].demand;
            v.locked_by     = members[0].name;
            v.locked_by_uid = members[0].uid;
        }
        return v;
    }

    bool has_heat = false, has_cool = false, has_auto = false;
    bool any_unknown = false, any_incompatible = false;
    const MemberObs* active = nullptr;   // physically holding the compressor now
    const MemberObs* blocked = nullptr;  // a head in STANDBY (authoritative signal)

    for (const auto& m : members) {
        if (m.state == MemberState::Unknown) {
            any_unknown = true;
            v.unknown_members.push_back(m.name.empty() ? m.uid : m.name);
            continue;
        }
        if (m.state == MemberState::Incompatible) {
            any_incompatible = true;
            v.warnings.push_back((m.name.empty() ? m.uid : m.name) +
                                 " runs an incompatible protocol version");
            continue;
        }
        // Known / SelfKnown from here on.
        if (m.power_on) {
            switch (m.demand) {
                case Demand::Heat: has_heat = true; break;
                case Demand::Cool: has_cool = true; break;
                case Demand::Auto:
                    has_auto = true;
                    v.warnings.push_back((m.name.empty() ? m.uid : m.name) +
                                         " is in AUTO — its direction can't be read");
                    break;
                case Demand::Neutral: break;
            }
        }
        if (m.active_now && m.demand != Demand::Neutral && m.demand != Demand::Auto)
            active = &m;
        if (m.standby && m.power_on &&
            (m.demand == Demand::Heat || m.demand == Demand::Cool))
            blocked = &m;
    }

    // Establish what the compressor is serving.
    if (active) {
        v.locked_mode   = active->demand;
        v.locked_by     = active->name;
        v.locked_by_uid = active->uid;
    } else if (blocked) {
        // A blocked head proves the compressor is running the opposite direction
        // for someone we may not directly see.
        v.locked_mode = opposite(blocked->demand);
        // owner unknown → leave locked_by empty ("another zone").
    }

    const bool opposing = has_heat && has_cool;

    if (opposing) {
        // A real, retained conflict among known members.
        v.status = (active || blocked) ? GroupStatus::Conflict
                                       : GroupStatus::PendingConflict;
        for (const auto& m : members) {
            if (m.state == MemberState::Unknown || m.state == MemberState::Incompatible)
                continue;
            if (!m.power_on) continue;
            if (m.demand != Demand::Heat && m.demand != Demand::Cool) continue;
            // If a direction is locked, the blocked members are the opposing ones;
            // if still pending, list both sides so the user can choose.
            if (v.locked_mode != Demand::Neutral && m.demand == v.locked_mode) continue;
            v.conflicts.push_back({m.uid, m.name, m.demand});
        }
    } else if (blocked) {
        // No opposing *known* claim visible, but the hardware STANDBY signal is
        // authoritative: this head's mode is being blocked by an owner we can't
        // see (likely an Unknown peer). Fail toward warning the user.
        v.status = GroupStatus::Conflict;
        v.conflicts.push_back({blocked->uid, blocked->name, blocked->demand});
    } else if (any_unknown || any_incompatible || has_auto) {
        // Can't positively confirm "no conflict".
        v.status = GroupStatus::Indeterminate;
    } else {
        v.status = GroupStatus::Ok;
    }

    return v;
}

Demand parse_target_mode(const std::string& mode) {
    const std::string m = upper(mode);
    if (m == "HEAT") return Demand::Heat;
    if (m == "COOL") return Demand::Cool;
    return Demand::Neutral;
}

ResolveStrategy parse_strategy(const std::string& s) {
    return upper(s) == "OFF_CONFLICTING" ? ResolveStrategy::OffConflicting
                                         : ResolveStrategy::FlipMode;
}

ResolveOp plan_resolution(bool power_on, Demand member_demand,
                          Demand target, ResolveStrategy strat) {
    ResolveOp op;
    // Only Heat/Cool are valid resolution targets.
    if (target != Demand::Heat && target != Demand::Cool) return op;
    // A zero-draw zone (OFF/FAN, or simply not powered) is never disturbed, so a
    // resolution can never silently turn an OFF zone on.
    if (!power_on || member_demand == Demand::Neutral) return op;
    // Already asking the compressor for the target direction → nothing to do.
    if (member_demand == target) return op;
    // Otherwise this zone conflicts (opposing Heat/Cool, or unreadable Auto).
    op.change = true;
    if (strat == ResolveStrategy::OffConflicting) {
        op.turn_off = true;
    } else {
        op.set_mode = target;
    }
    return op;
}

}  // namespace hvac_group

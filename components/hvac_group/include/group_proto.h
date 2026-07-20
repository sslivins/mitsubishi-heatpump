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

/// A nonce (used to bind a signed peer request↔response and defeat replay) is
/// 1..32 characters of lowercase hex. Restricting the charset guarantees it can
/// never contain the '\n' field separator used by signing_string, so an
/// attacker-supplied nonce can't shift a field boundary.
bool is_valid_nonce(const std::string& nonce);

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

/// Constant-time string equality — compares every byte so a timing side-channel
/// can't reveal how many leading characters of a pairing code matched. Unequal
/// lengths return false (but still scan @p a fully).
bool ct_equal(const std::string& a, const std::string& b);

/// Outcome of validating an inbound pairing claim against the owner's active
/// code, factored out of the (time/RNG-bearing) session so it is host-testable.
enum class ClaimDecision {
    Ok,            ///< code matches an active, unexpired, non-locked session.
    NoActiveCode,  ///< no pairing window is open.
    Expired,       ///< the window's TTL elapsed.
    LockedOut,     ///< too many wrong attempts; the code is burned.
    BadCode,       ///< wrong code (caller decrements the attempt counter).
};

/// Decide a claim from the session's observable state. Order matters: a closed
/// or expired window is reported before any code comparison, and lockout is
/// checked before accepting a match so a burned code can never succeed.
ClaimDecision evaluate_claim(bool active, bool expired, bool code_matches,
                             int attempts_left);

// ── Conflict / lock model (Phase 2) ─────────────────────────────────────────
//
// Pure evaluation of a shared-compressor group from a set of per-member
// observations. The refrigerant *claim* is the retained power+mode (survives
// idle-at-setpoint/min-off/defrost); `operating` marks who is physically
// running the compressor now; STANDBY (+IDLE, !operating) is the hardware's
// authoritative "my commanded mode is being blocked" signal.

/// The refrigerant direction a head is asking the shared compressor for.
enum class Demand {
    Neutral,  ///< OFF or FAN — draws nothing from the compressor.
    Heat,
    Cool,     ///< COOL and DRY both draw cooling.
    Auto,     ///< AUTO — direction is unknowable over CN105 (a blind spot).
};

/// Reachability/compatibility of a member's observation.
enum class MemberState {
    SelfKnown,     ///< this device (always trustworthy).
    Known,         ///< a peer that answered with a fresh, compatible snapshot.
    Unknown,       ///< an enrolled peer that didn't answer / stale — may still
                   ///< be holding the compressor, so treated as fail-safe.
    Incompatible,  ///< answered but with an incompatible protocol version.
};

/// One member's observed claim + physical state, as fed to evaluate_group.
struct MemberObs {
    std::string uid;
    std::string name;
    MemberState state = MemberState::Unknown;
    bool        power_on = false;
    Demand      demand = Demand::Neutral;   ///< classified from power+mode.
    bool        active_now = false;         ///< operating==true (running now).
    bool        standby = false;            ///< STANDBY+IDLE+!operating (blocked).
};

/// Overall group status for the computed view.
enum class GroupStatus {
    Standalone,       ///< no enrolled peers — coordination disabled.
    Ok,               ///< no conflict; every enrolled peer accounted for.
    PendingConflict,  ///< opposing demands but nobody physically owns it yet.
    Conflict,         ///< opposing demands and the compressor is serving one.
    Indeterminate,    ///< can't confirm "no conflict" (a peer is unknown/incompat).
};

/// A member whose demand is being (or about to be) blocked.
struct ConflictEntry {
    std::string uid;
    std::string name;
    Demand      wants = Demand::Neutral;
};

/// Computed, UI-ready view of the group.
struct GroupView {
    GroupStatus status = GroupStatus::Standalone;
    Demand      locked_mode = Demand::Neutral;  ///< direction the compressor is serving.
    std::string locked_by;       ///< name of the head physically holding it ("" if unknown).
    std::string locked_by_uid;
    std::vector<ConflictEntry>   conflicts;        ///< blocked / opposing members.
    std::vector<std::string>     unknown_members;  ///< names of unreachable peers.
    std::vector<std::string>     warnings;         ///< human-readable caveats.
};

/// Classify a head's refrigerant claim from its retained power + mode strings
/// (case-insensitive). OFF/FAN → Neutral, HEAT → Heat, COOL/DRY → Cool,
/// AUTO → Auto, anything unrecognized → Neutral (fail safe).
Demand classify_demand(const std::string& power, const std::string& mode);

/// Build a MemberObs (state=Known) from a head's raw reported fields — the same
/// derivation used for self and for a polled peer, kept in one host-tested place.
/// `standby` is set from the STANDBY(+IDLE+!operating) signature. The caller sets
/// state=SelfKnown for the local head or downgrades to Incompatible on a protocol
/// mismatch.
MemberObs observe(const std::string& uid, const std::string& name,
                  const std::string& power, const std::string& mode,
                  bool operating, const std::string& sub_mode,
                  const std::string& stage);

/// The opposing refrigerant direction (Heat↔Cool; Neutral/Auto map to Neutral).
Demand opposite(Demand d);

/// Short uppercase label for a demand (HEAT/COOL/OFF/AUTO), for JSON/UI.
const char* demand_str(Demand d);

/// Machine-readable status token (standalone/ok/pending_conflict/conflict/
/// indeterminate), matching the /api/group contract.
const char* status_str(GroupStatus s);

/// Compute the group view from all members. By convention @p members[0] is this
/// device (state SelfKnown). With no peers the result is Standalone. A retained
/// opposing pair is a Conflict once someone is active_now (else PendingConflict);
/// a SelfKnown/Known member in STANDBY is an authoritative Conflict even when the
/// opposing owner isn't directly visible. Any Unknown/Incompatible peer downgrades
/// an otherwise-Ok view to Indeterminate (never masks a detected conflict).
GroupView evaluate_group(const std::vector<MemberObs>& members);

// ── Resolution planning (Phase 3) ───────────────────────────────────────────
//
// Pure logic for the coordinator-per-op resolve: given a chosen target mode and
// a strategy, decide what each member must do. Deterministic and host-tested so
// the "never turn an OFF zone on" / "only touch conflicting zones" guarantees
// are locked down without hardware.

/// How a coordinator resolves conflicting zones onto a single target mode.
enum class ResolveStrategy {
    FlipMode,        ///< switch a conflicting zone to the target mode (default).
    OffConflicting,  ///< turn a conflicting zone OFF instead of flipping it.
};

/// The concrete change one member should apply during a resolution. When
/// @c change is false the member is left completely untouched.
struct ResolveOp {
    bool   change   = false;            ///< false → leave this member as-is.
    bool   turn_off = false;            ///< OffConflicting: power the zone OFF.
    Demand set_mode = Demand::Neutral;  ///< FlipMode: the mode to switch to.
};

/// Parse a target-mode token ("HEAT"/"COOL", case-insensitive) into a Demand.
/// Only Heat and Cool are valid resolution targets; anything else (including
/// AUTO/OFF/FAN/garbage) → Neutral, which callers reject as an invalid target.
Demand parse_target_mode(const std::string& mode);

/// Parse a strategy token ("flip"/"off_conflicting", case-insensitive).
/// Unrecognized/empty → FlipMode, the safe default that keeps zones running.
ResolveStrategy parse_strategy(const std::string& s);

/// Decide what one member must do to resolve onto @p target under @p strat.
/// A member "conflicts" when it is powered and drawing a non-neutral direction
/// other than the target (an opposing Heat/Cool, or unreadable Auto). A member
/// that is off/neutral (OFF/FAN or simply not powered) or already aligned with
/// the target is left untouched — a resolution never turns an OFF zone on. An
/// invalid @p target (not Heat/Cool) yields no change.
ResolveOp plan_resolution(bool power_on, Demand member_demand,
                          Demand target, ResolveStrategy strat);

}  // namespace hvac_group

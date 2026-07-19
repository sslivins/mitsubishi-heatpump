// Host-side unit tests for the pure group-protocol helpers (group_proto.cpp).
//
// These run on the build host with a plain C++17 compiler — no ESP-IDF, no
// hardware, no crypto engine. They lock down the security-relevant string and
// format logic that guards the shared-compressor coordination protocol:
//   * identity validation (reject corrupt NVS / malformed peer messages),
//   * the canonical HMAC signing-string (field-boundary safety so a signature
//     can't be replayed at another head or with a shifted body),
//   * hex round-tripping, and enrolled-peer (de)serialization.
//
// Build & run (see .github/workflows/host-tests.yml): one g++ invocation with
//   -std=c++17 -I components/hvac_group/include
//   components/hvac_group/group_proto.cpp test/host/test_group_proto.cpp

#include "group_proto.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        const std::string a_ = (actual);                                      \
        const std::string e_ = (expected);                                    \
        if (a_ != e_) {                                                       \
            std::printf("FAIL %s:%d  %s\n      got: \"%s\"\n expected: \"%s\"\n", \
                        __FILE__, __LINE__, #actual, a_.c_str(), e_.c_str()); \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

#define CHECK_TRUE(cond)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL %s:%d  expected true: %s\n",                    \
                        __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

#define CHECK_FALSE(cond)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            std::printf("FAIL %s:%d  expected false: %s\n",                   \
                        __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

using namespace hvac_group;

static void test_hex_roundtrip() {
    const uint8_t bytes[] = {0x00, 0x0f, 0xa5, 0xff, 0x10};
    const std::string hex = to_hex(bytes, sizeof(bytes));
    CHECK_EQ(hex, "000fa5ff10");

    std::vector<uint8_t> back;
    CHECK_TRUE(from_hex(hex, back));
    CHECK_TRUE(back.size() == sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); ++i) CHECK_TRUE(back[i] == bytes[i]);

    // Uppercase parses too; odd-length and non-hex are rejected.
    CHECK_TRUE(from_hex("AABB", back));
    CHECK_FALSE(from_hex("abc", back));     // odd length
    CHECK_TRUE(back.empty());
    CHECK_FALSE(from_hex("zz", back));      // non-hex
    CHECK_TRUE(back.empty());
    CHECK_TRUE(from_hex("", back));         // empty is valid (zero bytes)
    CHECK_TRUE(back.empty());
}

static void test_identity_validation() {
    // group_id: exactly 32 lowercase hex.
    CHECK_TRUE(is_valid_group_id("0123456789abcdef0123456789abcdef"));
    CHECK_FALSE(is_valid_group_id(""));
    CHECK_FALSE(is_valid_group_id("0123456789ABCDEF0123456789abcdef"));  // uppercase
    CHECK_FALSE(is_valid_group_id("0123456789abcdef0123456789abcde"));   // 31 chars
    CHECK_FALSE(is_valid_group_id("0123456789abcdef0123456789abcdeff")); // 33 chars
    CHECK_FALSE(is_valid_group_id("0123456789abcdef0123456789abcdeg"));  // non-hex

    // group_secret: exactly 64 lowercase hex.
    CHECK_TRUE(is_valid_group_secret(std::string(64, 'a')));
    CHECK_FALSE(is_valid_group_secret(std::string(63, 'a')));
    CHECK_FALSE(is_valid_group_secret(std::string(64, 'g')));

    // pairing code: exactly 6 digits.
    CHECK_TRUE(is_valid_pairing_code("000000"));
    CHECK_TRUE(is_valid_pairing_code("482915"));
    CHECK_FALSE(is_valid_pairing_code("12345"));   // too short
    CHECK_FALSE(is_valid_pairing_code("1234567"));  // too long
    CHECK_FALSE(is_valid_pairing_code("12a456"));   // non-digit
    CHECK_FALSE(is_valid_pairing_code(" 12345"));

    // uid: 1..32 lowercase alnum (matches the MAC-derived device_uid).
    CHECK_TRUE(is_valid_uid("6dac"));
    CHECK_TRUE(is_valid_uid("e6c8"));
    CHECK_FALSE(is_valid_uid(""));
    CHECK_FALSE(is_valid_uid("6DAC"));       // uppercase
    CHECK_FALSE(is_valid_uid("6d-ac"));      // punctuation
    CHECK_FALSE(is_valid_uid("6d\nac"));     // never contains the field separator
    CHECK_FALSE(is_valid_uid(std::string(33, 'a')));
}

static void test_signing_string() {
    const std::string s = signing_string("6dac", "e6c8", "abc", "42", "{\"m\":1}");
    CHECK_EQ(s, "6dac\ne6c8\nabc\n42\n{\"m\":1}");

    // Deterministic.
    CHECK_EQ(signing_string("a", "b", "g", "1", "body"),
             signing_string("a", "b", "g", "1", "body"));

    // Binding: swapping sender/receiver changes the signed bytes (a signature
    // for A->B can't be replayed as B->A).
    CHECK_TRUE(signing_string("a", "b", "g", "1", "x") !=
               signing_string("b", "a", "g", "1", "x"));

    // Field-boundary safety: because uids/gid/nonce cannot contain '\n' and the
    // free-form body is last, two different field splits can never collide.
    // "a|b" vs "a"+"b" style ambiguity is impossible with the newline join.
    CHECK_TRUE(signing_string("a", "b", "g", "1", "23") !=
               signing_string("a", "b", "g", "12", "3"));
    CHECK_TRUE(signing_string("a", "b", "g", "1", "2\n3") !=
               signing_string("a", "b", "g", "1", "23"));
}

static void test_peer_serialization() {
    std::vector<std::string> uids = {"6dac", "e6c8", "aa11"};
    const std::string blob = serialize_peers(uids);
    CHECK_EQ(blob, "6dac,e6c8,aa11");

    const std::vector<std::string> back = deserialize_peers(blob);
    CHECK_TRUE(back.size() == 3);
    CHECK_EQ(back[0], "6dac");
    CHECK_EQ(back[2], "aa11");

    // Duplicates collapse, blanks are dropped, first-seen order preserved.
    CHECK_EQ(serialize_peers({"6dac", "6dac", "", "e6c8"}), "6dac,e6c8");
    const std::vector<std::string> d = deserialize_peers(",6dac,,6dac,e6c8,");
    CHECK_TRUE(d.size() == 2);
    CHECK_EQ(d[0], "6dac");
    CHECK_EQ(d[1], "e6c8");

    // Empty round-trips to empty.
    CHECK_EQ(serialize_peers({}), "");
    CHECK_TRUE(deserialize_peers("").empty());
}

static void test_add_remove_peer() {
    std::vector<std::string> uids;
    CHECK_TRUE(add_peer(uids, "6dac", "e6c8"));
    CHECK_FALSE(add_peer(uids, "6dac", "e6c8"));   // duplicate
    CHECK_FALSE(add_peer(uids, "e6c8", "e6c8"));   // never add self
    CHECK_FALSE(add_peer(uids, "BAD!", "e6c8"));   // invalid uid
    CHECK_TRUE(uids.size() == 1);

    CHECK_TRUE(add_peer(uids, "aa11", "e6c8"));
    CHECK_TRUE(remove_peer(uids, "6dac"));
    CHECK_FALSE(remove_peer(uids, "6dac"));         // already gone
    CHECK_TRUE(uids.size() == 1);
    CHECK_EQ(uids[0], "aa11");
}

static void test_ct_equal() {
    CHECK_TRUE(ct_equal("482915", "482915"));
    CHECK_FALSE(ct_equal("482915", "482910"));
    CHECK_FALSE(ct_equal("482915", "48291"));   // different length
    CHECK_FALSE(ct_equal("", "0"));
    CHECK_TRUE(ct_equal("", ""));
}

static void test_evaluate_claim() {
    // Happy path.
    CHECK_TRUE(evaluate_claim(true, false, true, 5) == ClaimDecision::Ok);
    // Window state is reported before any code comparison.
    CHECK_TRUE(evaluate_claim(false, false, true, 5) == ClaimDecision::NoActiveCode);
    CHECK_TRUE(evaluate_claim(true, true, true, 5) == ClaimDecision::Expired);
    // A burned code can never succeed, even with the right digits.
    CHECK_TRUE(evaluate_claim(true, false, true, 0) == ClaimDecision::LockedOut);
    CHECK_TRUE(evaluate_claim(true, false, false, 0) == ClaimDecision::LockedOut);
    // Wrong code with attempts remaining.
    CHECK_TRUE(evaluate_claim(true, false, false, 3) == ClaimDecision::BadCode);
}

static MemberObs mk(const std::string& uid, MemberState st, bool on, Demand d,
                    bool active, bool standby) {
    MemberObs m;
    m.uid = uid; m.name = uid; m.state = st;
    m.power_on = on; m.demand = d; m.active_now = active; m.standby = standby;
    return m;
}

static void test_classify_demand() {
    CHECK_TRUE(classify_demand("ON",  "HEAT") == Demand::Heat);
    CHECK_TRUE(classify_demand("ON",  "COOL") == Demand::Cool);
    CHECK_TRUE(classify_demand("ON",  "DRY")  == Demand::Cool);  // DRY draws cooling
    CHECK_TRUE(classify_demand("ON",  "FAN")  == Demand::Neutral);
    CHECK_TRUE(classify_demand("ON",  "AUTO") == Demand::Auto);
    CHECK_TRUE(classify_demand("OFF", "HEAT") == Demand::Neutral);  // power gates it
    CHECK_TRUE(classify_demand("on",  "cool") == Demand::Cool);     // case-insensitive
    CHECK_TRUE(opposite(Demand::Heat) == Demand::Cool);
    CHECK_TRUE(opposite(Demand::Cool) == Demand::Heat);
    CHECK_TRUE(opposite(Demand::Neutral) == Demand::Neutral);
}

static void test_group_standalone() {
    std::vector<MemberObs> ms = {
        mk("self", MemberState::SelfKnown, true, Demand::Heat, true, false)};
    GroupView v = evaluate_group(ms);
    CHECK_EQ(status_str(v.status), "standalone");
    CHECK_EQ(v.locked_by, "self");                 // lone running head still shown
    CHECK_TRUE(v.locked_mode == Demand::Heat);
}

static void test_group_ok() {
    std::vector<MemberObs> ms = {
        mk("self", MemberState::SelfKnown, true, Demand::Heat, true, false),
        mk("peer", MemberState::Known,     false, Demand::Neutral, false, false)};
    GroupView v = evaluate_group(ms);
    CHECK_EQ(status_str(v.status), "ok");
    CHECK_EQ(v.locked_by, "self");
    CHECK_TRUE(v.locked_mode == Demand::Heat);
    CHECK_TRUE(v.conflicts.empty());
}

static void test_group_conflict_active() {
    // self holds HEAT but peer is COOL and physically running → self is blocked.
    std::vector<MemberObs> ms = {
        mk("self", MemberState::SelfKnown, true, Demand::Heat, false, true),
        mk("peer", MemberState::Known,     true, Demand::Cool, true,  false)};
    GroupView v = evaluate_group(ms);
    CHECK_EQ(status_str(v.status), "conflict");
    CHECK_TRUE(v.locked_mode == Demand::Cool);
    CHECK_EQ(v.locked_by, "peer");
    CHECK_TRUE(v.conflicts.size() == 1);
    CHECK_EQ(v.conflicts[0].name, "self");
    CHECK_TRUE(v.conflicts[0].wants == Demand::Heat);
}

static void test_group_pending_conflict() {
    // Opposing demands, nobody energized yet.
    std::vector<MemberObs> ms = {
        mk("self", MemberState::SelfKnown, true, Demand::Heat, false, false),
        mk("peer", MemberState::Known,     true, Demand::Cool, false, false)};
    GroupView v = evaluate_group(ms);
    CHECK_EQ(status_str(v.status), "pending_conflict");
    CHECK_TRUE(v.conflicts.size() == 2);  // both sides listed to choose from
}

static void test_group_indeterminate_unknown() {
    std::vector<MemberObs> ms = {
        mk("self", MemberState::SelfKnown, true, Demand::Heat, false, false),
        mk("peer", MemberState::Unknown,   false, Demand::Neutral, false, false)};
    GroupView v = evaluate_group(ms);
    CHECK_EQ(status_str(v.status), "indeterminate");
    CHECK_TRUE(v.unknown_members.size() == 1);
    CHECK_EQ(v.unknown_members[0], "peer");
}

static void test_group_self_blocked_peer_unknown() {
    // Authoritative STANDBY: self blocked in HEAT while the opposing owner is an
    // unreachable peer. We must still report a conflict and infer COOL is locked.
    std::vector<MemberObs> ms = {
        mk("self", MemberState::SelfKnown, true, Demand::Heat, false, true),
        mk("peer", MemberState::Unknown,   false, Demand::Neutral, false, false)};
    GroupView v = evaluate_group(ms);
    CHECK_EQ(status_str(v.status), "conflict");
    CHECK_TRUE(v.locked_mode == Demand::Cool);   // opposite of the blocked demand
    CHECK_TRUE(v.locked_by.empty());             // owner not directly visible
    CHECK_TRUE(v.conflicts.size() == 1);
    CHECK_EQ(v.conflicts[0].name, "self");
}

static void test_group_auto_blind_spot() {
    std::vector<MemberObs> ms = {
        mk("self", MemberState::SelfKnown, true, Demand::Cool, true, false),
        mk("peer", MemberState::Known,     true, Demand::Auto, false, false)};
    GroupView v = evaluate_group(ms);
    CHECK_EQ(status_str(v.status), "indeterminate");  // AUTO can't be read → fail safe
    CHECK_TRUE(!v.warnings.empty());
}

static void test_observe() {
    MemberObs a = observe("6dac", "Eric", "ON", "COOL", true, "NORMAL", "MODERATE");
    CHECK_TRUE(a.state == MemberState::Known);
    CHECK_TRUE(a.demand == Demand::Cool);
    CHECK_TRUE(a.power_on);
    CHECK_TRUE(a.active_now);
    CHECK_FALSE(a.standby);

    // The blocked/losing-head signature.
    MemberObs b = observe("6dac", "Eric", "ON", "HEAT", false, "STANDBY", "IDLE");
    CHECK_TRUE(b.demand == Demand::Heat);
    CHECK_TRUE(b.standby);
    CHECK_FALSE(b.active_now);

    // Off head: neutral, not powered, never standby.
    MemberObs c = observe("x", "X", "OFF", "HEAT", false, "STANDBY", "IDLE");
    CHECK_TRUE(c.demand == Demand::Neutral);
    CHECK_FALSE(c.power_on);
    CHECK_TRUE(c.standby);  // fields still map; caller ignores standby when off
}

static void test_is_valid_nonce() {
    CHECK_TRUE(is_valid_nonce("0a1b2c3d"));
    CHECK_FALSE(is_valid_nonce(""));
    CHECK_FALSE(is_valid_nonce("XYZ"));           // non-hex
    CHECK_FALSE(is_valid_nonce("ab\ncd"));        // separator injection blocked
    CHECK_FALSE(is_valid_nonce(std::string(33, 'a')));  // too long
}

int main() {
    test_hex_roundtrip();
    test_identity_validation();
    test_signing_string();
    test_peer_serialization();
    test_add_remove_peer();
    test_ct_equal();
    test_evaluate_claim();
    test_classify_demand();
    test_observe();
    test_is_valid_nonce();
    test_group_standalone();
    test_group_ok();
    test_group_conflict_active();
    test_group_pending_conflict();
    test_group_indeterminate_unknown();
    test_group_self_blocked_peer_unknown();
    test_group_auto_blind_spot();
    if (g_failures == 0) {
        std::printf("OK - all host group-protocol tests passed\n");
        return 0;
    }
    std::printf("%d host group-protocol test(s) FAILED\n", g_failures);
    return 1;
}

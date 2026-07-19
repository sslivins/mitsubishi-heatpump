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

int main() {
    test_hex_roundtrip();
    test_identity_validation();
    test_signing_string();
    test_peer_serialization();
    test_add_remove_peer();
    if (g_failures == 0) {
        std::printf("OK - all host group-protocol tests passed\n");
        return 0;
    }
    std::printf("%d host group-protocol test(s) FAILED\n", g_failures);
    return 1;
}

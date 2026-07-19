// Host-side unit tests for the pure HA topic helpers (hvac_topics.cpp).
//
// These run on the build host with a plain C++17 compiler — no ESP-IDF, no
// hardware. Build & run (see .github/workflows/host-tests.yml); pass these to
// one g++ invocation: -std=c++17, -I components/hvac_mqtt/include, the source
// components/hvac_mqtt/hvac_topics.cpp and test/host/test_hvac_topics.cpp.
// Guards the regression that made a renamed head vanish from Home Assistant:
// the discovery object_id MUST derive from the immutable device_uid, never from
// the mutable friendly_name.

#include "hvac_topics.h"

#include <cstdio>
#include <string>

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

using hvac_mqtt::slugify;
using hvac_mqtt::discovery_object_id;
using hvac_mqtt::fan_device_to_ha;
using hvac_mqtt::fan_ha_to_device;

static void test_slugify() {
    // Already-valid names pass through unchanged.
    CHECK_EQ(slugify("living_room"), "living_room");
    CHECK_EQ(slugify("Master-Bedroom_2"), "Master-Bedroom_2");

    // Spaces (the original HA-drop bug) become underscores.
    CHECK_EQ(slugify("Master Bedroom"), "Master_Bedroom");

    // Apostrophes and other punctuation map to '_' and collapse.
    CHECK_EQ(slugify("Eric's Room"), "Eric_s_Room");
    CHECK_EQ(slugify("Kitchen / Dining"), "Kitchen_Dining");
    CHECK_EQ(slugify("a@#b"), "a_b");

    // Runs of separators collapse to a single '_'.
    CHECK_EQ(slugify("a    b"), "a_b");

    // Leading/trailing separators are trimmed.
    CHECK_EQ(slugify("  padded  "), "padded");
    CHECK_EQ(slugify("__weird__"), "weird");

    // Empty / all-invalid input falls back to the safe default.
    CHECK_EQ(slugify(""), "heatpump");
    CHECK_EQ(slugify("   "), "heatpump");
    CHECK_EQ(slugify("!!!"), "heatpump");

    // Output only ever contains HA-legal characters.
    const std::string s = slugify("Eric's Room #1");
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        CHECK_TRUE(ok);
    }
}

static void test_discovery_object_id() {
    // Stable id is hostname-shaped and derived from the hardware uid.
    CHECK_EQ(discovery_object_id("6dac"), "mitsubishi-heatpump-6dac");
    CHECK_EQ(discovery_object_id("e6c8"), "mitsubishi-heatpump-e6c8");

    // Regression guard: the discovery id depends ONLY on device_uid. It must be
    // identical regardless of friendly_name, so renaming never orphans the old
    // retained config / collides on unique_id.
    const std::string before_rename = discovery_object_id("6dac");
    const std::string after_rename  = discovery_object_id("6dac");
    CHECK_EQ(before_rename, after_rename);

    // Two units with the same friendly_name still get distinct discovery ids.
    CHECK_TRUE(discovery_object_id("6dac") != discovery_object_id("e6c8"));
}

static void test_fan_mode_mapping() {
    // device -> HA standard names (so the HA frontend draws labelled icons).
    CHECK_EQ(fan_device_to_ha("AUTO"),  "auto");
    CHECK_EQ(fan_device_to_ha("QUIET"), "diffuse");
    CHECK_EQ(fan_device_to_ha("1"),     "low");
    CHECK_EQ(fan_device_to_ha("2"),     "middle");
    CHECK_EQ(fan_device_to_ha("3"),     "medium");
    CHECK_EQ(fan_device_to_ha("4"),     "high");
    // Empty settings (unit not yet reporting) fall back to auto.
    CHECK_EQ(fan_device_to_ha(""), "auto");

    // HA name -> native device value (what the heat pump expects).
    CHECK_EQ(fan_ha_to_device("auto"),    "AUTO");
    CHECK_EQ(fan_ha_to_device("diffuse"), "QUIET");
    CHECK_EQ(fan_ha_to_device("low"),     "1");
    CHECK_EQ(fan_ha_to_device("middle"),  "2");
    CHECK_EQ(fan_ha_to_device("medium"),  "3");
    CHECK_EQ(fan_ha_to_device("high"),    "4");

    // Round-trips: every published fan_mode must survive HA->device->HA.
    for (const char* ha : {"auto", "diffuse", "low", "middle", "medium", "high"})
        CHECK_EQ(fan_device_to_ha(fan_ha_to_device(ha)), ha);

    // A raw native value coming back on the command topic still maps sanely
    // (robustness — e.g. a manual publish or older automation).
    CHECK_EQ(fan_ha_to_device("QUIET"), "QUIET");
    CHECK_EQ(fan_ha_to_device("1"),     "1");
}

int main() {
    test_slugify();
    test_discovery_object_id();
    test_fan_mode_mapping();
    if (g_failures == 0) {
        std::printf("OK - all host topic tests passed\n");
        return 0;
    }
    std::printf("%d host topic test(s) FAILED\n", g_failures);
    return 1;
}

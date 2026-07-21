/// @file capability.cpp
/// @brief Wide-vane capability detection + persistence (see capability.h).

#include "capability.h"

#include <atomic>
#include <cstring>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "events.h"

namespace capability {
namespace {

constexpr char TAG[] = "capability";

// NVS: dedicated namespace so it can't collide with wifi/device keys.
constexpr char kNs[]        = "capab";
constexpr char kKeyDetect[] = "wv_det";  // u8 WideVane
constexpr char kKeyOverride[] = "wv_ovr"; // u8 Override
constexpr char kKeyAlgo[]   = "wv_alg";  // u8 detection algorithm version

// Bump when the detection logic changes materially so stored verdicts from an
// older algorithm are discarded and re-detected. v2: one-shot probe with
// enforcement suspended (v1 mis-read manual vanes as supported because the
// driver kept re-asserting the commanded value).
constexpr uint8_t kAlgoVersion = 2;

// Probe timing. The revert-to-centre on a manual louver was observed to take
// ~18 s live, so the hold window is comfortably longer.
constexpr uint32_t kAcceptTimeoutMs = 20000;  // must be honoured within this
constexpr uint32_t kHoldMs          = 30000;  // must stay put for this long
constexpr uint32_t kSampleMs        = 1500;   // poll cadence during a probe
constexpr int      kRevertConfirm   = 2;      // consecutive off-target reads = reverted

// Auto-run pacing.
constexpr uint32_t kIdlePollMs      = 5000;    // how often the task wakes to check
constexpr uint32_t kInconclusiveBackoffMs = 5UL * 60UL * 1000UL;  // retry after 5 min

Hooks s_hooks;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Shared state (guarded by s_mux for the small scalars).
WideVane s_detect{WideVane::Unknown};
Override s_override{Override::Auto};
std::atomic<bool>     s_detecting{false};
std::atomic<bool>     s_manual_request{false};
// Monotonic counter bumped on every user/HA wide-vane change; the probe
// snapshots it and aborts if it moves.
std::atomic<uint32_t> s_user_seq{0};

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

void load_nvs() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t algo = 0, det = 0, ovr = 0;
    nvs_get_u8(h, kKeyAlgo, &algo);
    if (nvs_get_u8(h, kKeyDetect, &det) == ESP_OK && algo == kAlgoVersion &&
        det <= static_cast<uint8_t>(WideVane::Inconclusive)) {
        // Inconclusive is never persisted as a final verdict; treat as Unknown.
        WideVane w = static_cast<WideVane>(det);
        s_detect = (w == WideVane::Inconclusive) ? WideVane::Unknown : w;
    }
    if (nvs_get_u8(h, kKeyOverride, &ovr) == ESP_OK &&
        ovr <= static_cast<uint8_t>(Override::ForceHide)) {
        s_override = static_cast<Override>(ovr);
    }
    nvs_close(h);
}

void persist_detect(WideVane w) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, kKeyAlgo, kAlgoVersion);
    nvs_set_u8(h, kKeyDetect, static_cast<uint8_t>(w));
    nvs_commit(h);
    nvs_close(h);
}

void persist_override(Override o) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, kKeyOverride, static_cast<uint8_t>(o));
    nvs_commit(h);
    nvs_close(h);
}

// Pick an off-centre target distinct from the current position so a change is
// actually requested. "|" is centre; prefer ">>", fall back to "<<".
std::string probe_target(const std::string& current) {
    if (current == ">>") return "<<";
    return ">>";
}

// Run one full probe. Returns the verdict. Assumes unit connected + on at entry.
//
// The driver suspends wide-vane enforcement for the duration (begin_probe), so
// what we read back is the unit's own reported position:
//   * powered vane  -> accepts the target and HOLDS it            -> Supported
//   * manual louver -> ignores it, or accepts then reverts to its
//                      physical detent                            -> Unsupported
// Only an external disturbance (user/HA change, disconnect, power-off) yields
// Inconclusive, since then we can't trust the observation.
WideVane run_probe() {
    if (!s_hooks.begin_probe || !s_hooks.get_wide_vane) return WideVane::Inconclusive;

    const uint32_t seq0 = s_user_seq.load();
    const std::string pre = s_hooks.get_wide_vane();
    const std::string target = probe_target(pre);

    ESP_LOGI(TAG, "wide-vane probe: pre='%s' target='%s'", pre.c_str(), target.c_str());
    s_hooks.begin_probe(target);

    auto disturbed = [&]() -> bool {
        return s_user_seq.load() != seq0 || !s_hooks.unit_connected() ||
               !s_hooks.unit_on();
    };
    // Clean finish restores the pre-probe position; a user change during the
    // probe wins (abort leaves their value in place).
    auto finish = [&](WideVane v) -> WideVane {
        if (s_user_seq.load() != seq0) s_hooks.abort_probe();
        else s_hooks.end_probe(pre);
        return v;
    };

    // --- Acceptance: a powered vane echoes the target within the timeout. A
    //     unit that never echoes it (while healthy) is ignoring the command. ---
    const uint32_t t0 = now_ms();
    bool accepted = false;
    while (now_ms() - t0 < kAcceptTimeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(kSampleMs));
        if (disturbed()) {
            ESP_LOGI(TAG, "probe disturbed (accept phase) -> inconclusive");
            return finish(WideVane::Inconclusive);
        }
        if (s_hooks.get_wide_vane() == target) { accepted = true; break; }
    }
    if (!accepted) {
        ESP_LOGI(TAG, "wide-vane command ignored -> UNSUPPORTED (manual)");
        return finish(WideVane::Unsupported);
    }

    // --- Durability: it must STAY at the target for the hold window. A manual
    //     louver accepts briefly then reverts to its physical detent. ---
    const uint32_t th = now_ms();
    int offTarget = 0;
    while (now_ms() - th < kHoldMs) {
        vTaskDelay(pdMS_TO_TICKS(kSampleMs));
        if (disturbed()) {
            ESP_LOGI(TAG, "probe disturbed (hold phase) -> inconclusive");
            return finish(WideVane::Inconclusive);
        }
        if (s_hooks.get_wide_vane() != target) {
            if (++offTarget >= kRevertConfirm) {
                ESP_LOGI(TAG, "wide-vane reverted -> UNSUPPORTED (manual)");
                return finish(WideVane::Unsupported);
            }
        } else {
            offTarget = 0;
        }
    }

    ESP_LOGI(TAG, "wide-vane held target -> SUPPORTED (powered)");
    return finish(WideVane::Supported);
}

void log_verdict(WideVane w) {
    const char* msg = nullptr;
    switch (w) {
        case WideVane::Supported:   msg = "Wide vane detected: powered"; break;
        case WideVane::Unsupported: msg = "Wide vane detected: manual (control hidden)"; break;
        default: return;  // don't spam the log for unknown/inconclusive
    }
    events::log(events::Cat::System, events::Actor::System, msg);
}

void detect_task(void*) {
    uint32_t next_auto_retry = 0;  // 0 = eligible now
    for (;;) {
        bool manual = s_manual_request.exchange(false);

        WideVane cur;
        Override ovr;
        portENTER_CRITICAL(&s_mux);
        cur = s_detect;
        ovr = s_override;
        portEXIT_CRITICAL(&s_mux);

        // Auto-run only while still Unknown; manual request forces a run.
        bool want_auto = (cur == WideVane::Unknown) &&
                         (next_auto_retry == 0 || now_ms() >= next_auto_retry);
        bool run = manual || want_auto;

        // Never probe a control the user has forced hidden — it's moot, and it
        // would move the vane for no reason. (Force-show still benefits from a
        // real verdict, so we allow probing there.)
        if (ovr == Override::ForceHide && !manual) run = false;

        if (run && s_hooks.unit_connected && s_hooks.unit_connected() &&
            s_hooks.unit_on && s_hooks.unit_on()) {
            s_detecting.store(true);
            WideVane v = run_probe();
            s_detecting.store(false);

            if (v == WideVane::Supported || v == WideVane::Unsupported) {
                portENTER_CRITICAL(&s_mux);
                s_detect = v;
                portEXIT_CRITICAL(&s_mux);
                persist_detect(v);
                log_verdict(v);
                next_auto_retry = 0;
            } else {
                // Inconclusive: keep Unknown, back off before auto-retrying.
                next_auto_retry = now_ms() + kInconclusiveBackoffMs;
            }
        } else if (manual) {
            // Requested but unit offline/off: nothing to do; snapshot() shows
            // detecting=false and the UI explains it can't run right now.
            ESP_LOGI(TAG, "manual detect requested but unit offline/off");
        }

        vTaskDelay(pdMS_TO_TICKS(kIdlePollMs));
    }
}

}  // namespace

void init(const Hooks& hooks) {
    s_hooks = hooks;
    load_nvs();
    ESP_LOGI(TAG, "wide-vane support: %s, override: %s",
             wide_vane_to_str(s_detect), override_to_str(s_override));
    xTaskCreate(detect_task, "cap_detect", 4096, nullptr, 3, nullptr);
}

Snapshot snapshot() {
    Snapshot s;
    portENTER_CRITICAL(&s_mux);
    s.wideVane = s_detect;
    s.override_m = s_override;
    portEXIT_CRITICAL(&s_mux);
    s.detecting = s_detecting.load();

    switch (s.override_m) {
        case Override::ForceShow: s.show = true; break;
        case Override::ForceHide: s.show = false; break;
        case Override::Auto:
        default:
            // Option A: only show when positively detected as supported.
            s.show = (s.wideVane == WideVane::Supported);
            break;
    }
    return s;
}

esp_err_t set_wide_vane_override(Override o) {
    portENTER_CRITICAL(&s_mux);
    s_override = o;
    portEXIT_CRITICAL(&s_mux);
    persist_override(o);
    return ESP_OK;
}

void request_wide_vane_detect() { s_manual_request.store(true); }

void notify_user_wide_vane_change() { s_user_seq.fetch_add(1); }

Override override_from_str(const char* s) {
    if (!s) return Override::Auto;
    if (strcasecmp(s, "show") == 0 || strcasecmp(s, "force_show") == 0) return Override::ForceShow;
    if (strcasecmp(s, "hide") == 0 || strcasecmp(s, "force_hide") == 0) return Override::ForceHide;
    return Override::Auto;
}

const char* override_to_str(Override o) {
    switch (o) {
        case Override::ForceShow: return "show";
        case Override::ForceHide: return "hide";
        case Override::Auto:
        default: return "auto";
    }
}

const char* wide_vane_to_str(WideVane w) {
    switch (w) {
        case WideVane::Supported:   return "supported";
        case WideVane::Unsupported: return "unsupported";
        case WideVane::Inconclusive: return "inconclusive";
        case WideVane::Unknown:
        default: return "unknown";
    }
}

}  // namespace capability

/// @file capability.h
/// @brief Per-unit HVAC feature capability detection (currently: wide vane).
///
/// Some Mitsubishi indoor units ship with a *manual* wide-vane louver that has
/// no motor. The CN105 protocol carries NO capability flag distinguishing a
/// powered wide vane from a manual one — the only reliable signal is
/// behavioural: command an off-centre position and see whether the unit holds
/// it (powered) or accepts it briefly then reverts to centre (manual).
///
/// This module runs that probe once (automatically, when the unit is connected
/// and powered on), persists the verdict to NVS, and exposes it so the web UI
/// can hide the wide-vane control when it would do nothing. The user can force
/// the control shown/hidden regardless of the detected value, and re-run the
/// probe on demand.
///
/// Design notes (why it looks the way it does):
///  * We separate *acceptance* (the requested value appears in the unit's
///    confirmed state) from *durability* (it stays there past the observed
///    ~18 s revert window). A manual vane passes acceptance but fails
///    durability — so acceptance alone would be a false "supported".
///  * The probe never shares the driver's wanted_/dirty machinery: it drives
///    the public setter and reads back the published (unit-confirmed) state,
///    exactly like a user would. A concurrent *user* wide-vane change bumps a
///    sequence counter (see notify_user_change) which aborts the probe as
///    inconclusive rather than mis-reading the user's value as a revert.
///  * We never power the unit on to probe. If it's off, detection defers.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "esp_err.h"

namespace capability {

/// Detected wide-vane support. `Unknown` = not yet determined; `Inconclusive`
/// = a probe ran but conditions changed mid-way (disconnect, user override,
/// timeout) so no verdict — will be retried.
enum class WideVane : uint8_t { Unknown = 0, Supported, Unsupported, Inconclusive };

/// User override of the detected value (persisted separately from detection).
enum class Override : uint8_t { Auto = 0, ForceShow, ForceHide };

/// Detection-loop drivers, supplied by the application (wired to the CN105
/// driver in main). All are called from the detection task only.
struct Hooks {
    std::function<void(const std::string&)> begin_probe;   ///< suspend enforce + one-shot send
    std::function<void(const std::string&)> end_probe;     ///< resume enforce + restore value
    std::function<void()>                   abort_probe;   ///< resume enforce, keep wanted (user wins)
    std::function<std::string()>            get_wide_vane; ///< unit-confirmed position
    std::function<bool()>                   unit_connected; ///< CN105 link up
    std::function<bool()>                   unit_on;        ///< power == ON
};

/// Load persisted state from NVS and start the background detection task.
/// Call once at boot, after nvs_flash_init() and after the CN105 driver exists.
void init(const Hooks& hooks);

/// Snapshot of everything the UI needs, taken atomically.
struct Snapshot {
    WideVane wideVane;   ///< detected support
    Override override_m; ///< user override
    bool     detecting;  ///< a probe is currently running
    bool     show;       ///< effective: should the UI show the wide-vane control?
};
Snapshot snapshot();

/// Set (and persist) the user override for the wide-vane control.
esp_err_t set_wide_vane_override(Override o);

/// Request an on-demand detection run. Ignored if one is already in flight or
/// the unit is offline/off (the caller can surface that via snapshot()).
void request_wide_vane_detect();

/// Called whenever a *user/HA* wide-vane change is applied (NOT the probe's own
/// writes). Aborts an in-flight probe so a real change isn't misread as a
/// unit-initiated revert.
void notify_user_wide_vane_change();

/// Parse/format helpers for the override enum used by the web API.
Override    override_from_str(const char* s);
const char* override_to_str(Override o);
const char* wide_vane_to_str(WideVane w);

}  // namespace capability

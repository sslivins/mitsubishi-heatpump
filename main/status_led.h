/// @file status_led.h
/// @brief Onboard RGB-LED status indicator for shared-compressor coordination.
///
/// The M5Stamp-S3 is screenless; its only local output is a single onboard
/// WS2812 RGB LED (GPIO21). This driver lets the firmware surface a
/// group-coordination warning glyph without a display:
///   * Alert (red)  — a real, retained mode conflict on the shared compressor.
///   * Warn  (amber)— indeterminate: a peer is unreachable so a conflict can't
///                    be ruled out (fail-safe).
///   * Off          — in sync, standalone, or ungrouped.
///
/// Init is failure-safe: if the LED can't be brought up (e.g. bench board with
/// no RGB LED, or RMT exhausted) the module degrades to a no-op and the rest of
/// the firmware is unaffected.
#pragma once

#include "esp_err.h"

namespace status_led {

enum class Level { Off, Warn, Alert };

/// Bring up the onboard WS2812 LED. Safe to call once at boot. Returns the
/// underlying error if the LED is unavailable; the module then no-ops.
esp_err_t init();

/// Set the indicator. Thread-safe and idempotent — the hardware is only
/// touched when the level actually changes.
void set(Level lvl);

}  // namespace status_led

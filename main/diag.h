/// @file diag.h
/// @brief Boot / brownout / power-sag diagnostics.
///
/// Answers the question "did the board actually brown out?" — which is otherwise
/// invisible because a brownout wipes RAM and silently resets the chip. On every
/// boot we latch esp_reset_reason() and, when it is ESP_RST_BROWNOUT, bump a
/// persistent counter in NVS. We also track the lowest input voltage ever seen
/// (record-low, persisted) plus a session-scoped VIN-sag counter, so a rail that
/// is dipping toward the brownout threshold shows up *before* it actually trips.
#pragma once

#include <cstdint>

namespace diag {

struct Snapshot {
    // --- persisted across reboots (NVS) ---
    uint32_t    boot_count      = 0;    ///< total boots since first flash
    uint32_t    brownout_count  = 0;    ///< boots whose reset reason was BROWNOUT
    uint16_t    vin_min_ever_mv = 0;    ///< all-time lowest effective input (0 = none yet)
    // --- this-boot / session-scoped (RAM only) ---
    const char* reset_reason    = "unknown";  ///< human string for the last reset
    bool        last_was_brownout = false;    ///< reset reason of *this* boot
    uint16_t    vin_min_mv      = 0;    ///< lowest effective input seen this session
    uint32_t    vin_sag_count   = 0;    ///< times input dropped below the floor this session
};

/// Latch reset reason, bump boot/brownout counters. Call once, after
/// nvs_flash_init() and before starting tasks.
void init();

/// Feed the latest effective input voltage (mV). Updates session/all-time minima
/// and counts a "sag" each time the rail crosses below `floor_mv` (edge-triggered).
/// Cheap; NVS is only written when a new all-time low is set (rare).
void note_vin(uint16_t input_mv, uint16_t floor_mv);

/// Copy the current diagnostics (thread-safe).
Snapshot get();

}  // namespace diag

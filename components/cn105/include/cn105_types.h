/// @file cn105_types.h
/// @brief Plain, ESP-IDF-free value types shared by the CN105 driver and the
///        host-testable wire codec (cn105_codec.h).
///
/// Kept deliberately free of any ESP-IDF / FreeRTOS headers so that the codec
/// and its host unit tests can include it and compile with a bare g++.

#pragma once

#include <string>

namespace cn105 {

/// Desired/echoed unit settings. String fields use the same vocabularies as
/// the SwiCago library so the MQTT contract from mitsubishi2MQTT carries over
/// 1:1.
struct Settings {
    std::string power;       ///< "ON" | "OFF"
    std::string mode;        ///< "HEAT" | "DRY" | "COOL" | "FAN" | "AUTO"
    float       temperature; ///< setpoint, °C (0.5 steps; some units 1.0)
    std::string fan;         ///< "AUTO" | "QUIET" | "1".."4"
    std::string vane;        ///< "AUTO" | "1".."5" | "SWING"
    std::string wideVane;    ///< "<<" | "<" | "|" | ">" | ">>" | "<>" | "SWING"
    bool        connected;   ///< true once a valid packet has been exchanged
};

/// Read-only status reported by the unit.
///
/// The first three fields are always populated once connected. The remaining
/// "extended telemetry" fields are decoded from info replies we already poll
/// (0x02/0x03/0x06/0x09/0x04) but which many units only partially fill in, so
/// each carries a has_* validity flag — consumers should render "--"/omit the
/// field unless its has_* flag is true. These are additive and never affect the
/// control (SET) path.
struct Status {
    float roomTemperature;     ///< measured room temp, °C
    bool  operating;           ///< compressor actively heating/cooling
    int   compressorFrequency; ///< Hz (0 when idle), if the unit reports it

    // --- extended telemetry (validity-guarded) ---
    float outsideTemp{0.0f};       ///< outdoor coil/air temp, °C (0x03)
    bool  has_outsideTemp{false};
    float runtimeHours{0.0f};      ///< cumulative run hours (0x03)
    bool  has_runtimeHours{false};
    int   inputPowerW{0};          ///< instantaneous input power, W (0x06)
    bool  has_inputPowerW{false};
    float energyKwh{0.0f};         ///< cumulative energy, kWh (0x06)
    bool  has_energyKwh{false};
    int   targetHumidity{0};       ///< target humidity, % (0x02)
    bool  has_targetHumidity{false};
    std::string subMode;           ///< NORMAL/DEFROST/PREHEAT/... (0x09 data[3])
    std::string stage;             ///< IDLE/LOW/.../HIGH (0x09 data[4])
    std::string errorCode;         ///< "No Error" or "Error 0xXX sub 0xXX" (0x04)
};

}  // namespace cn105

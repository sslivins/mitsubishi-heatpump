/// @file m5pm1.h
/// @brief Driver for the M5Stack Stamp-S3Bat power-management IC ("M5PM1").
///
/// On the Stamp-S3Bat (SKU S015) the "M5PM1" is physically a PY32 Cortex-M0+
/// MCU running M5's power-management firmware. It is an I2C slave at 0x6E that
/// supervises the rails, the LGS4056HDA Li-ion charger (CHG_EN / PROG / STAT),
/// the button/wake logic, and ADC telemetry (VBAT / VIN / VOUT).
///
/// Charge CURRENT is fixed in hardware by the LGS4056 PROG resistor network
/// (200 mA float / 650 mA when PY_G3_CHG_PROG is pulled low) and is NOT
/// settable over I2C. The firmware lever is CHG_EN (on/off). To keep the
/// CN105 5V supply within budget we gate CHG_EN closed-loop on the VIN reading
/// — see governor_tick().
///
/// Register map: https://github.com/m5stack/M5PM1 (src/M5PM1.h)

#pragma once

#include <cstdint>
#include "driver/i2c_master.h"
#include "esp_err.h"

namespace m5pm1 {

constexpr uint8_t kI2CAddr = 0x6E;

/// Register addresses (subset we use).
enum Reg : uint8_t {
    REG_PWR_SRC  = 0x04, ///< 0=5VIN, 1=5VINOUT, 2=BAT
    REG_PWR_CFG  = 0x06, ///< bit0 CHG_EN, bit1 DCDC_EN, bit2 LDO_EN, bit3 BOOST_EN
    REG_BATT_LVP = 0x08, ///< low-voltage cutoff: mV = 2000 + n*7.81 (2000..4000)
    REG_I2C_CFG  = 0x09, ///< bit4 speed, [3:0] auto-sleep timeout sec (0=off)
    REG_WDT_CNT  = 0x0A, ///< watchdog timeout sec (0=off)
    REG_WDT_KEY  = 0x0B, ///< write 0xA5 to feed the watchdog
    REG_SYS_CMD  = 0x0C, ///< shutdown/reboot/download (key 0xA in high nibble)
    REG_VBAT_H   = 0x22, ///< battery voltage, mV, big-endian 16-bit
    REG_VIN_H    = 0x24, ///< input (5V) voltage, mV, big-endian 16-bit
};

/// PWR_CFG bit positions.
enum PwrCfgBit : uint8_t {
    BIT_CHG_EN   = 0,
    BIT_DCDC_EN  = 1,
    BIT_LDO_EN   = 2,
    BIT_BOOST_EN = 3,
};

enum class PowerSource : uint8_t { VIN = 0, VIN_OUT = 1, BATTERY = 2, UNKNOWN = 0xFF };

/// Closed-loop charge-governor configuration.
struct GovernorConfig {
    uint16_t vin_floor_mv   = 4800; ///< pause charging if VIN sags below this
    uint16_t vin_resume_mv  = 4950; ///< resume only once VIN recovers above this
    uint16_t vbat_target_mv = 4150; ///< stop charging at/above this VBAT
    bool     charge_on_battery_only_when_vin_ok = true;
};

class PMIC {
public:
    /// Probe the PMIC on an existing I2C master bus. Safe to call when the chip
    /// is absent (e.g. bench testing on a non-Bat board) — returns ESP_ERR_NOT_FOUND.
    esp_err_t init(i2c_master_bus_handle_t bus);

    bool present() const { return dev_ != nullptr; }

    // --- Telemetry ---
    esp_err_t read_vbat_mv(uint16_t& mv);
    esp_err_t read_vin_mv(uint16_t& mv);
    esp_err_t read_power_source(PowerSource& src);

    // --- Charge control ---
    esp_err_t set_charge_enable(bool en);
    esp_err_t set_batt_lvp_mv(uint16_t mv);   ///< over-discharge cutoff

    // --- Supervisor ---
    esp_err_t feed_watchdog();
    esp_err_t set_watchdog_seconds(uint8_t sec); ///< 0 disables

    /// Run one governor step: read VIN/VBAT/source and toggle CHG_EN so the
    /// input draw stays within the CN105 budget. Call ~1 Hz from a task.
    /// NOTE: CHG_EN auto-clears on PMIC reset/download — re-assert after boot.
    esp_err_t governor_tick(const GovernorConfig& cfg);

private:
    esp_err_t read_reg8(uint8_t reg, uint8_t& val);
    esp_err_t write_reg8(uint8_t reg, uint8_t val);
    esp_err_t read_u16be(uint8_t reg_hi, uint16_t& val);

    i2c_master_dev_handle_t dev_{nullptr};
    bool charging_{false};
};

}  // namespace m5pm1

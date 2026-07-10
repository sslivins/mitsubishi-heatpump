/// @file m5pm1.cpp
/// @brief M5PM1 (PY32) PMIC driver implementation.
///
/// The I2C register reads/writes are real and should work against the chip.
/// The exact ADC scaling and BATT_LVP encoding follow the M5PM1 docs; verify
/// against hardware once the Stamp-S3Bat arrives (marked TODO(verify)).

#include "m5pm1.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace m5pm1 {

static const char* TAG = "m5pm1";

esp_err_t PMIC::init(i2c_master_bus_handle_t bus) {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = kI2CAddr,
        .scl_speed_hz     = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev_);
    if (err != ESP_OK) { dev_ = nullptr; return err; }

    // Confirm the chip answers by reading the power-source register.
    uint8_t v;
    if (read_reg8(REG_PWR_SRC, v) != ESP_OK) {
        ESP_LOGW(TAG, "no ACK at 0x%02X — PMIC absent? running without it", kI2CAddr);
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "PMIC present (PWR_SRC=%u)", v);
    return ESP_OK;
}

esp_err_t PMIC::read_reg8(uint8_t reg, uint8_t& val) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(dev_, &reg, 1, &val, 1, 100);
}

esp_err_t PMIC::write_reg8(uint8_t reg, uint8_t val) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev_, buf, sizeof(buf), 100);
}

esp_err_t PMIC::read_u16le(uint8_t reg_lo, uint16_t& val) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    uint8_t out[2];
    esp_err_t err = i2c_master_transmit_receive(dev_, &reg_lo, 1, out, 2, 100);
    // M5PM1 voltage registers are little-endian: low byte at the base address.
    if (err == ESP_OK) val = static_cast<uint16_t>(out[0]) | (static_cast<uint16_t>(out[1]) << 8);
    return err;
}

esp_err_t PMIC::read_vbat_mv(uint16_t& mv)   { return read_u16le(REG_VBAT_L, mv); }
esp_err_t PMIC::read_vin_mv(uint16_t& mv)    { return read_u16le(REG_VIN_L, mv); }
esp_err_t PMIC::read_vinout_mv(uint16_t& mv) { return read_u16le(REG_VINOUT_L, mv); }

esp_err_t PMIC::read_input_mv(uint16_t& mv) {
    uint16_t vin = 0, vinout = 0;
    read_vin_mv(vin);
    read_vinout_mv(vinout);
    mv = (vin > vinout) ? vin : vinout;
    return ESP_OK;
}

esp_err_t PMIC::read_power_source(PowerSource& src) {
    uint8_t v;
    esp_err_t err = read_reg8(REG_PWR_SRC, v);
    src = (err == ESP_OK) ? static_cast<PowerSource>(v) : PowerSource::UNKNOWN;
    return err;
}

esp_err_t PMIC::set_charge_enable(bool en) {
    uint8_t cfg;
    esp_err_t err = read_reg8(REG_PWR_CFG, cfg);
    if (err != ESP_OK) return err;
    if (en) cfg |= (1 << BIT_CHG_EN);
    else    cfg &= ~(1 << BIT_CHG_EN);
    err = write_reg8(REG_PWR_CFG, cfg);
    if (err == ESP_OK) charging_ = en;
    return err;
}

esp_err_t PMIC::set_batt_lvp_mv(uint16_t mv) {
    // mV = 2000 + n*7.81  =>  n = (mv - 2000) / 7.81   TODO(verify) scaling
    if (mv < 2000) mv = 2000;
    if (mv > 4000) mv = 4000;
    uint8_t n = static_cast<uint8_t>((mv - 2000) / 7.81f + 0.5f);
    return write_reg8(REG_BATT_LVP, n);
}

esp_err_t PMIC::feed_watchdog()                  { return write_reg8(REG_WDT_KEY, 0xA5); }
esp_err_t PMIC::set_watchdog_seconds(uint8_t s)  { return write_reg8(REG_WDT_CNT, s); }

esp_err_t PMIC::governor_tick(const GovernorConfig& cfg) {
    if (!dev_) return ESP_ERR_INVALID_STATE;

    uint16_t vin = 0, vbat = 0;
    read_input_mv(vin);   // effective supply: max(5VIN, 5VINOUT)
    read_vbat_mv(vbat);

    bool want_charge = charging_;

    if (vbat >= cfg.vbat_target_mv) {
        want_charge = false;                 // battery full enough
    } else if (vin < cfg.vin_floor_mv) {
        want_charge = false;                 // input sagging — back off
    } else if (vin >= cfg.vin_resume_mv) {
        want_charge = true;                  // healthy input — top up
    }
    // Hysteresis band (floor..resume) leaves the state unchanged.

    if (want_charge != charging_) {
        ESP_LOGI(TAG, "governor: charge %s (vin=%umV vbat=%umV)",
                 want_charge ? "ON" : "OFF", vin, vbat);
        return set_charge_enable(want_charge);
    }
    return ESP_OK;
}

}  // namespace m5pm1

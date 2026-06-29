/// @file main.cpp
/// @brief mitsubishi-heatpump entry point.
///
/// Wiring:  NVS -> M5PM1 PMIC (charge governor) -> WiFi -> MQTT -> CN105.
///
/// The CN105 task pumps the Mitsubishi serial protocol and republishes state to
/// MQTT; inbound MQTT commands are applied back to the heat pump. The PMIC task
/// keeps battery charging within the CN105 5V budget and feeds the PMIC
/// watchdog. Both protocol/PMIC layers are stubs today (see their .cpp files)
/// but the control flow and threading model are real.

#include "wifi_manager.h"
#include "cn105.h"
#include "m5pm1.h"
#include "hvac_mqtt.h"

#include <cstdlib>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"

static const char* TAG = "main";

namespace {
cn105::HeatPump g_hp;
m5pm1::PMIC     g_pmic;

// --- PMIC / charge-governor task (~1 Hz) ---
void pmic_task(void*) {
    m5pm1::GovernorConfig gov{};
#ifdef CONFIG_PMIC_CHARGE_GOVERNOR
    gov.vin_floor_mv   = CONFIG_PMIC_VIN_FLOOR_MV;
    gov.vin_resume_mv  = CONFIG_PMIC_VIN_RESUME_MV;
    gov.vbat_target_mv = CONFIG_PMIC_VBAT_TARGET_MV;
#endif
    while (true) {
        if (g_pmic.present()) {
            g_pmic.feed_watchdog();
#ifdef CONFIG_PMIC_CHARGE_GOVERNOR
            g_pmic.governor_tick(gov);
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- CN105 task: pump protocol, publish changes ---
void cn105_task(void*) {
    g_hp.setSettingsChangedCallback([](const cn105::Settings& s) {
        hvac_mqtt::publish_settings(s);
        hvac_mqtt::publish_state(s, g_hp.getStatus());
    });
    g_hp.setStatusChangedCallback([](const cn105::Status& st) {
        hvac_mqtt::publish_state(g_hp.getSettings(), st);
    });

    g_hp.enableExternalUpdate();
    g_hp.enableAutoUpdate();
    g_hp.connect(static_cast<uart_port_t>(CONFIG_CN105_UART_PORT),
                 CONFIG_CN105_UART_TX_PIN, CONFIG_CN105_UART_RX_PIN);
    hvac_mqtt::publish_discovery(g_hp.getSettings());

    while (true) {
        g_hp.update();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Apply an inbound MQTT command to the heat pump.
void on_mqtt_command(const hvac_mqtt::Command& cmd) {
    using K = hvac_mqtt::Command::Kind;
    switch (cmd.kind) {
        case K::Power:       g_hp.setPowerSetting(cmd.value); break;
        case K::Mode:        g_hp.setModeSetting(cmd.value); break;
        case K::Temperature: g_hp.setTemperature(strtof(cmd.value.c_str(), nullptr)); break;
        case K::Fan:         g_hp.setFanSpeed(cmd.value); break;
        case K::Vane:        g_hp.setVaneSetting(cmd.value); break;
        case K::WideVane:    g_hp.setWideVaneSetting(cmd.value); break;
        case K::RemoteTemp:  g_hp.setRemoteTemperature(strtof(cmd.value.c_str(), nullptr)); break;
        case K::System:      ESP_LOGI(TAG, "system cmd: %s", cmd.value.c_str()); break;
    }
}

void init_pmic() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(CONFIG_PMIC_I2C_SDA_PIN);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(CONFIG_PMIC_I2C_SCL_PIN);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed — PMIC unavailable");
        return;
    }
    if (g_pmic.init(bus) == ESP_OK) {
        // CHG_EN auto-clears on PMIC reset, so re-assert intent here. The
        // governor takes over modulation immediately after.
        g_pmic.set_charge_enable(true);
    }
}
}  // namespace

extern "C" void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Power management first — it owns the battery/charge path.
    init_pmic();
    xTaskCreate(pmic_task, "pmic", 3072, nullptr, 5, nullptr);

    // Network.
    esp_err_t wret = wifi::init();
    if (wret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi not connected (provisioning) — MQTT/CN105 deferred");
        return;  // pmic_task keeps running; reboot after provisioning.
    }
    ESP_LOGI(TAG, "WiFi up: %s", wifi::get_ip());

    // MQTT bridge.
    hvac_mqtt::Config mcfg{
        .broker_uri    = CONFIG_MQTT_BROKER_URI,
        .username      = CONFIG_MQTT_USERNAME,
        .password      = CONFIG_MQTT_PASSWORD,
        .base_topic    = CONFIG_MQTT_BASE_TOPIC,
        .friendly_name = CONFIG_MQTT_FRIENDLY_NAME,
    };
    if (hvac_mqtt::init(mcfg, on_mqtt_command) != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed");
    }

    // Heat pump protocol.
    xTaskCreate(cn105_task, "cn105", 4096, nullptr, 6, nullptr);

    ESP_LOGI(TAG, "mitsubishi-heatpump %s running",
             esp_app_get_description()->version);
}

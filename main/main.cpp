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
#include "web_ui.h"
#include "ota.h"

#include <cstdlib>
#include <cstdio>
#include <string>
#include <mutex>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"

static const char* TAG = "main";

namespace {
cn105::HeatPump g_hp;
m5pm1::PMIC     g_pmic;

// Latest PMIC telemetry, refreshed by pmic_task and read by the web server so
// the HTTP task never touches the I2C bus directly.
struct PowerSnap {
    bool                present  = false;
    uint16_t            vbat_mv  = 0;
    uint16_t            vin_mv   = 0;
    m5pm1::PowerSource  source   = m5pm1::PowerSource::UNKNOWN;
    bool                charging = false;
};
std::mutex g_pwr_mtx;
PowerSnap  g_pwr;

const char* power_source_str(m5pm1::PowerSource s) {
    switch (s) {
        case m5pm1::PowerSource::VIN:     return "vin";
        case m5pm1::PowerSource::VIN_OUT: return "vin_out";
        case m5pm1::PowerSource::BATTERY: return "battery";
        default:                          return "unknown";
    }
}

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
            PowerSnap snap;
            snap.present = true;
            g_pmic.read_vbat_mv(snap.vbat_mv);
            g_pmic.read_vin_mv(snap.vin_mv);
            g_pmic.read_power_source(snap.source);
            // Best-effort: on external power and below target → likely charging.
            snap.charging = (snap.source != m5pm1::PowerSource::BATTERY) &&
                            (snap.vbat_mv < gov.vbat_target_mv);
            std::lock_guard<std::mutex> lock(g_pwr_mtx);
            g_pwr = snap;
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
        case K::Ota:         ota::start_url(cmd.value); break;
        case K::UpdateInstall: ota::install_latest(); break;
    }
}

// Split a "mqtt://host:port" broker URI into host + port for the settings model.
void parse_broker_uri(const std::string& uri, std::string& host, int& port) {
    std::string s = uri;
    auto scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        host = s.substr(0, colon);
        port = atoi(s.substr(colon + 1).c_str());
    } else {
        host = s;
    }
    if (port <= 0) port = 1883;
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

    ota::init();

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

    // We reached the network — the running image is healthy. Confirm it so the
    // bootloader doesn't roll back a freshly-OTA'd slot on the next reset.
    ota::mark_valid();

    // MQTT bridge. Runtime settings (web UI, persisted to NVS) win over the
    // Kconfig fallback. The friendly_name is the MQTT node and HA device label;
    // if blank, fall back to a unique per-chip name so 4 units don't collide.
    std::string uid = wifi::device_uid();

    hvac_mqtt::StoredSettings fallback;
    parse_broker_uri(CONFIG_MQTT_BROKER_URI, fallback.host, fallback.port);
    fallback.username      = CONFIG_MQTT_USERNAME;
    fallback.password      = CONFIG_MQTT_PASSWORD;
    fallback.base_topic    = CONFIG_MQTT_BASE_TOPIC;
    fallback.friendly_name = CONFIG_MQTT_FRIENDLY_NAME;
    hvac_mqtt::StoredSettings ms = hvac_mqtt::load_settings(fallback);

    std::string friendly = ms.friendly_name;
    if (friendly.empty()) friendly = "heatpump-" + uid;
    char uri[128];
    std::snprintf(uri, sizeof(uri), "mqtt://%s:%d", ms.host.c_str(), ms.port);
    ESP_LOGI(TAG, "device uid %s, broker %s, mqtt node '%s'",
             uid.c_str(), uri, friendly.c_str());

    hvac_mqtt::Config mcfg{
        .broker_uri    = uri,
        .username      = ms.username,
        .password      = ms.password,
        .base_topic    = ms.base_topic,
        .friendly_name = friendly,
        .device_uid    = uid,
        .sw_version    = esp_app_get_description()->version,
    };
    if (hvac_mqtt::init(mcfg, on_mqtt_command) != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed");
    }

    // On every (re)connect, (re)publish the retained HA discovery configs and the
    // current state so Home Assistant re-syncs after a broker drop. Retained
    // discovery alone is lost if it was queued before the link was up.
    hvac_mqtt::set_on_connected([] {
        hvac_mqtt::publish_discovery(g_hp.getSettings());
        hvac_mqtt::publish_update_discovery();
        hvac_mqtt::publish_settings(g_hp.getSettings());
        hvac_mqtt::publish_state(g_hp.getSettings(), g_hp.getStatus());
        ota::UpdateInfo u = ota::get_update_info();
        hvac_mqtt::publish_update_state(u.current_version, u.latest_version, u.release_url);
    });

    // On-device web UI (REST + dashboard). Reuses on_mqtt_command so the web
    // and MQTT control paths funnel through identical apply logic.
    web_ui::Hooks hooks;
    hooks.get_settings   = [] { return g_hp.getSettings(); };
    hooks.get_status     = [] { return g_hp.getStatus(); };
    hooks.unit_connected = [] { return g_hp.isConnected(); };
    hooks.mqtt_connected = [] { return hvac_mqtt::is_connected(); };
    hooks.apply_command  = [](const hvac_mqtt::Command& c) { on_mqtt_command(c); };
    hooks.get_power = [] {
        web_ui::PowerTelemetry t;
        std::lock_guard<std::mutex> lock(g_pwr_mtx);
        t.present  = g_pwr.present;
        t.vbat_mv  = g_pwr.vbat_mv;
        t.vin_mv   = g_pwr.vin_mv;
        t.source   = power_source_str(g_pwr.source);
        t.charging = g_pwr.charging;
        return t;
    };
    if (web_ui::init(hooks) != ESP_OK) {
        ESP_LOGW(TAG, "web UI failed to start");
    }

    // GitHub release auto-update: poll /releases/latest every 6h (and on demand
    // from the web UI). Each completed check surfaces installed/latest versions
    // to the web UI and to a Home Assistant `update` entity (with an Install
    // button wired back to ota::install_latest()).
    ota::set_on_update_changed([] {
        ota::UpdateInfo u = ota::get_update_info();
        hvac_mqtt::publish_update_state(u.current_version, u.latest_version, u.release_url);
    });
    ota::start_update_checker();

    // Heat pump protocol.
    xTaskCreate(cn105_task, "cn105", 4096, nullptr, 6, nullptr);

    ESP_LOGI(TAG, "mitsubishi-heatpump %s running",
             esp_app_get_description()->version);
}

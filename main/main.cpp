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
#include "diag.h"

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
    uint16_t            vin_mv   = 0;   ///< raw dedicated 5VIN pin
    uint16_t            vinout_mv= 0;   ///< raw bidirectional 5VINOUT port
    uint16_t            input_mv = 0;   ///< effective supply = max(vin, vinout)
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

// Derive the active power source from the measured rails. The PMIC's PWR_SRC
// register proved unreliable on the Stamp-S3Bat (reads "BAT" while USB-powered),
// so trust the ADC: whichever rail is at a healthy 5V is the live source.
m5pm1::PowerSource derive_source(uint16_t vin_mv, uint16_t vinout_mv, uint16_t vbat_mv) {
    if (vinout_mv >= 4500) return m5pm1::PowerSource::VIN_OUT;
    if (vin_mv    >= 4500) return m5pm1::PowerSource::VIN;
    if (vbat_mv   >= 3000) return m5pm1::PowerSource::BATTERY;
    return m5pm1::PowerSource::UNKNOWN;
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
            g_pmic.read_vinout_mv(snap.vinout_mv);
            snap.input_mv = (snap.vin_mv > snap.vinout_mv) ? snap.vin_mv : snap.vinout_mv;
            snap.source = derive_source(snap.vin_mv, snap.vinout_mv, snap.vbat_mv);
            // Charging only makes sense when a real battery is present (>=3.0V)
            // and we're on external power below the target voltage.
            snap.charging = (snap.source == m5pm1::PowerSource::VIN ||
                             snap.source == m5pm1::PowerSource::VIN_OUT) &&
                            (snap.vbat_mv >= 3000) &&
                            (snap.vbat_mv < gov.vbat_target_mv);
            std::lock_guard<std::mutex> lock(g_pwr_mtx);
            g_pwr = snap;
            // Feed the sag/brownout early-warning tracker with effective input.
            diag::note_vin(snap.input_mv, gov.vin_floor_mv);
        }
        // Push diagnostics to HA only when the sag count or record-low changes,
        // so a healthy rail never spams the broker.
        {
            static uint32_t last_sags = UINT32_MAX;
            static uint16_t last_min  = UINT16_MAX;
            diag::Snapshot ds = diag::get();
            if (ds.vin_sag_count != last_sags || ds.vin_min_mv != last_min) {
                last_sags = ds.vin_sag_count;
                last_min  = ds.vin_min_mv;
                if (hvac_mqtt::is_connected()) {
                    hvac_mqtt::publish_diag_state({ds.reset_reason, ds.brownout_count,
                                                   ds.vin_sag_count, ds.vin_min_mv});
                }
            }
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

    // Latch reset reason + brownout/boot counters from NVS before anything else
    // touches the power path.
    diag::init();

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
        hvac_mqtt::publish_diag_discovery();
        hvac_mqtt::publish_settings(g_hp.getSettings());
        hvac_mqtt::publish_state(g_hp.getSettings(), g_hp.getStatus());
        ota::UpdateInfo u = ota::get_update_info();
        hvac_mqtt::publish_update_state(u.current_version, u.latest_version, u.release_url);
        diag::Snapshot ds = diag::get();
        hvac_mqtt::publish_diag_state({ds.reset_reason, ds.brownout_count,
                                       ds.vin_sag_count, ds.vin_min_mv});
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
        t.present   = g_pwr.present;
        t.vbat_mv   = g_pwr.vbat_mv;
        t.vin_mv    = g_pwr.vin_mv;
        t.vinout_mv = g_pwr.vinout_mv;
        t.input_mv  = g_pwr.input_mv;
        t.source    = power_source_str(g_pwr.source);
        t.charging  = g_pwr.charging;
        return t;
    };
    hooks.get_diag = [] {
        diag::Snapshot s = diag::get();
        web_ui::DiagTelemetry t;
        t.boot_count        = s.boot_count;
        t.brownout_count    = s.brownout_count;
        t.reset_reason      = s.reset_reason;
        t.last_was_brownout = s.last_was_brownout;
        t.vin_min_mv        = s.vin_min_mv;
        t.vin_min_ever_mv   = s.vin_min_ever_mv;
        t.vin_sag_count     = s.vin_sag_count;
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
    // Stream install progress to HA's update entity so its modal shows a live
    // progress bar instead of appearing to do nothing until the device reboots.
    ota::set_on_progress([](const ota::Status& s) {
        ota::UpdateInfo u = ota::get_update_info();
        // While installing, report >= 0 (treat unknown size as 0%) so HA shows
        // the bar as in-progress; any terminal state clears it (-1 -> null).
        int pct = (s.state == ota::State::InProgress) ? (s.progress < 0 ? 0 : s.progress)
                                                      : -1;
        hvac_mqtt::publish_update_state(u.current_version, u.latest_version,
                                        u.release_url, "", pct);
    });
    ota::start_update_checker();

    // Heat pump protocol.
    xTaskCreate(cn105_task, "cn105", 8192, nullptr, 6, nullptr);

    ESP_LOGI(TAG, "mitsubishi-heatpump %s running",
             esp_app_get_description()->version);
}

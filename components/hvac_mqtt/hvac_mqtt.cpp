/// @file hvac_mqtt.cpp
/// @brief MQTT bridge implementation (esp-mqtt).
///
/// The client lifecycle, topic construction, subscriptions, LWT, the HA
/// discovery configs (climate + firmware-update) and the state/settings JSON
/// payloads are implemented. Inbound command payloads are routed to the
/// CN105 driver via the command callback; the CN105 packet engine itself is
/// still a STUB, so until hardware bring-up the published state reflects the
/// driver's seeded placeholder values.

#include "hvac_mqtt.h"

#include <cstring>
#include <utility>
#include "esp_log.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "cJSON.h"

namespace hvac_mqtt {

static const char* TAG = "hvac_mqtt";

namespace {
Config             g_cfg;
CommandCallback    g_on_command;
std::function<void()> g_on_connected;
esp_mqtt_client_handle_t g_client = nullptr;
bool               g_connected = false;
std::string        g_base;  ///< "<base_topic>/<friendly_name>"
StoredSettings     g_settings;          ///< settings the client was started with
constexpr char     kNvsNs[] = "mqtt";   ///< NVS namespace for persisted settings

// Read an NVS string key into `out`; leaves `out` unchanged if the key is absent.
void nvs_get_string(nvs_handle_t h, const char* key, std::string& out) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return;
    std::string tmp(len, '\0');
    if (nvs_get_str(h, key, tmp.data(), &len) == ESP_OK) {
        if (!tmp.empty() && tmp.back() == '\0') tmp.pop_back();
        out = tmp;
    }
}

std::string t(const char* suffix) { return g_base + suffix; }

// Build the HA `device` block shared by every entity this firmware exposes, so
// the climate, firmware-update (and future) entities all group under one device
// in Home Assistant. Keyed on the hardware-unique id so 4 physical units never
// merge even if they share a friendly_name.
cJSON* make_device_block() {
    cJSON* dev = cJSON_CreateObject();
    cJSON* ids = cJSON_CreateArray();
    std::string ident = "mitsubishi-heatpump-" + g_cfg.device_uid;
    cJSON_AddItemToArray(ids, cJSON_CreateString(ident.c_str()));
    cJSON_AddItemToObject(dev, "identifiers", ids);
    cJSON_AddStringToObject(dev, "name", g_cfg.friendly_name.c_str());
    cJSON_AddStringToObject(dev, "manufacturer", "Mitsubishi");
    cJSON_AddStringToObject(dev, "model", "CN105 heat-pump bridge");
    if (!g_cfg.sw_version.empty())
        cJSON_AddStringToObject(dev, "sw_version", g_cfg.sw_version.c_str());
    return dev;
}

// Map an inbound topic to a Command::Kind. Returns false if not a command topic.
bool topic_to_kind(const std::string& topic, Command::Kind& kind) {
    struct Map { const char* suffix; Command::Kind kind; };
    static const Map kMap[] = {
        {"/mode/set",        Command::Kind::Mode},
        {"/temp/set",        Command::Kind::Temperature},
        {"/remote_temp/set", Command::Kind::RemoteTemp},
        {"/fan/set",         Command::Kind::Fan},
        {"/vane/set",        Command::Kind::Vane},
        {"/wideVane/set",    Command::Kind::WideVane},
        {"/system/set",      Command::Kind::System},
        {"/ota/set",         Command::Kind::Ota},
        {"/update/install",  Command::Kind::UpdateInstall},
    };
    for (const auto& m : kMap) {
        if (topic == g_base + m.suffix) { kind = m.kind; return true; }
    }
    return false;
}

void subscribe_all() {
    const char* suffixes[] = {
        "/mode/set", "/temp/set", "/remote_temp/set", "/fan/set",
        "/vane/set", "/wideVane/set", "/system/set", "/ota/set",
        "/update/install",
    };
    for (const char* s : suffixes) {
        std::string topic = g_base + s;
        esp_mqtt_client_subscribe(g_client, topic.c_str(), 1);
    }
}

void event_handler(void*, esp_event_base_t, int32_t id, void* data) {
    auto* e = static_cast<esp_mqtt_event_handle_t>(data);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
        case MQTT_EVENT_CONNECTED:
            g_connected = true;
            ESP_LOGI(TAG, "connected to %s", g_cfg.broker_uri.c_str());
            subscribe_all();
            publish_availability(true);
            if (g_on_connected) g_on_connected();
            break;
        case MQTT_EVENT_DISCONNECTED:
            g_connected = false;
            ESP_LOGW(TAG, "disconnected");
            break;
        case MQTT_EVENT_DATA: {
            std::string topic(e->topic, e->topic_len);
            std::string payload(e->data, e->data_len);
            Command::Kind kind;
            if (topic_to_kind(topic, kind) && g_on_command) {
                g_on_command(Command{kind, payload});
            }
            break;
        }
        default:
            break;
    }
}
}  // namespace

esp_err_t init(const Config& cfg, CommandCallback on_command) {
    g_cfg = cfg;
    g_on_command = std::move(on_command);
    g_base = cfg.base_topic + "/" + cfg.friendly_name;

    std::string lwt_topic = g_base + "/availability";

    esp_mqtt_client_config_t mcfg = {};
    mcfg.broker.address.uri = g_cfg.broker_uri.c_str();
    if (!g_cfg.username.empty()) mcfg.credentials.username = g_cfg.username.c_str();
    if (!g_cfg.password.empty()) mcfg.credentials.authentication.password = g_cfg.password.c_str();
    mcfg.session.last_will.topic   = lwt_topic.c_str();
    mcfg.session.last_will.msg     = "offline";
    mcfg.session.last_will.qos     = 1;
    mcfg.session.last_will.retain  = true;

    g_client = esp_mqtt_client_init(&mcfg);
    if (!g_client) return ESP_FAIL;

    esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY, event_handler, nullptr);
    return esp_mqtt_client_start(g_client);
}

void set_on_connected(std::function<void()> cb) { g_on_connected = std::move(cb); }

StoredSettings load_settings(const StoredSettings& fallback) {
    StoredSettings s = fallback;
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_string(h, "host", s.host);
        int32_t port = 0;
        if (nvs_get_i32(h, "port", &port) == ESP_OK && port > 0) s.port = port;
        nvs_get_string(h, "user", s.username);
        nvs_get_string(h, "pass", s.password);
        nvs_get_string(h, "base", s.base_topic);
        nvs_get_string(h, "friendly", s.friendly_name);
        nvs_close(h);
    }
    g_settings = s;  // cache so get_settings() reflects the active config
    return s;
}

esp_err_t save_settings(const StoredSettings& s) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "host", s.host.c_str());
    nvs_set_i32(h, "port", s.port);
    nvs_set_str(h, "user", s.username.c_str());
    nvs_set_str(h, "pass", s.password.c_str());
    nvs_set_str(h, "base", s.base_topic.c_str());
    nvs_set_str(h, "friendly", s.friendly_name.c_str());
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

StoredSettings get_settings() { return g_settings; }

esp_err_t publish_availability(bool online) {
    if (!g_client) return ESP_ERR_INVALID_STATE;
    std::string topic = t("/availability");
    const char* msg = online ? "online" : "offline";
    int id = esp_mqtt_client_publish(g_client, topic.c_str(), msg, 0, 1, true);
    return id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t publish_update_discovery() {
    if (!g_client) return ESP_ERR_INVALID_STATE;

    std::string state_topic   = t("/update/state");
    std::string command_topic = t("/update/install");
    std::string avail_topic   = t("/availability");
    std::string unique_id     = g_cfg.device_uid + "_firmware";

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Firmware");
    cJSON_AddStringToObject(root, "unique_id", unique_id.c_str());
    cJSON_AddStringToObject(root, "object_id", unique_id.c_str());
    cJSON_AddStringToObject(root, "state_topic", state_topic.c_str());
    cJSON_AddStringToObject(root, "command_topic", command_topic.c_str());
    cJSON_AddStringToObject(root, "payload_install", "install");
    cJSON_AddStringToObject(root, "device_class", "firmware");
    // The state_topic carries JSON with installed_version/latest_version keys,
    // which HA's update platform parses natively. Adding value_template/
    // latest_version_template here instead left the entity stuck on "unknown",
    // so we rely on native JSON parsing.
    cJSON_AddStringToObject(root, "availability_topic", avail_topic.c_str());
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");

    cJSON_AddItemToObject(root, "device", make_device_block());

    char* payload = cJSON_PrintUnformatted(root);
    std::string topic = "homeassistant/update/" + g_cfg.friendly_name + "/config";
    int id = payload ? esp_mqtt_client_publish(g_client, topic.c_str(), payload, 0, 1, true) : -1;
    ESP_LOGI(TAG, "publish_update_discovery -> %s", topic.c_str());
    if (payload) cJSON_free(payload);
    cJSON_Delete(root);
    return id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t publish_update_state(const std::string& installed,
                               const std::string& latest,
                               const std::string& release_url,
                               const std::string& release_summary,
                               int update_percentage) {
    if (!g_client) return ESP_ERR_INVALID_STATE;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "installed_version", installed.c_str());
    // Until a check completes, report latest == installed so HA shows "up to date".
    cJSON_AddStringToObject(root, "latest_version",
                            latest.empty() ? installed.c_str() : latest.c_str());
    if (!release_url.empty())
        cJSON_AddStringToObject(root, "release_url", release_url.c_str());
    if (!release_summary.empty())
        cJSON_AddStringToObject(root, "release_summary", release_summary.c_str());
    // Progress for HA's update modal. A number drives the progress bar; null
    // resets the in-progress state (per the MQTT update integration schema).
    if (update_percentage >= 0) {
        cJSON_AddNumberToObject(root, "update_percentage", update_percentage);
        cJSON_AddBoolToObject(root, "in_progress", true);
    } else {
        cJSON_AddNullToObject(root, "update_percentage");
    }

    char* payload = cJSON_PrintUnformatted(root);
    std::string topic = t("/update/state");
    int id = payload ? esp_mqtt_client_publish(g_client, topic.c_str(), payload, 0, 1, true) : -1;
    if (payload) cJSON_free(payload);
    cJSON_Delete(root);
    return id < 0 ? ESP_FAIL : ESP_OK;
}

// Helper: emit one HA diagnostic sensor discovery config (retained).
static esp_err_t publish_diag_sensor(const char* name, const char* id_suffix,
                                     const char* value_template,
                                     const char* device_class,
                                     const char* unit,
                                     const char* icon) {
    std::string state_topic = t("/diag/state");
    std::string avail_topic = t("/availability");
    std::string unique_id   = g_cfg.device_uid + "_" + id_suffix;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "unique_id", unique_id.c_str());
    cJSON_AddStringToObject(root, "object_id", unique_id.c_str());
    cJSON_AddStringToObject(root, "state_topic", state_topic.c_str());
    cJSON_AddStringToObject(root, "value_template", value_template);
    cJSON_AddStringToObject(root, "entity_category", "diagnostic");
    if (device_class) cJSON_AddStringToObject(root, "device_class", device_class);
    if (unit)         cJSON_AddStringToObject(root, "unit_of_measurement", unit);
    if (icon)         cJSON_AddStringToObject(root, "icon", icon);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic.c_str());
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");
    cJSON_AddItemToObject(root, "device", make_device_block());

    char* payload = cJSON_PrintUnformatted(root);
    std::string topic = "homeassistant/sensor/" + g_cfg.friendly_name + "_" +
                        id_suffix + "/config";
    int id = payload ? esp_mqtt_client_publish(g_client, topic.c_str(), payload, 0, 1, true) : -1;
    if (payload) cJSON_free(payload);
    cJSON_Delete(root);
    return id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t publish_diag_discovery() {
    if (!g_client) return ESP_ERR_INVALID_STATE;
    esp_err_t rc = ESP_OK;
    if (publish_diag_sensor("Last reset reason", "reset_reason",
                            "{{ value_json.reset_reason }}",
                            nullptr, nullptr, "mdi:restart") != ESP_OK) rc = ESP_FAIL;
    if (publish_diag_sensor("Brownout count", "brownout_count",
                            "{{ value_json.brownout_count }}",
                            nullptr, nullptr, "mdi:flash-alert") != ESP_OK) rc = ESP_FAIL;
    if (publish_diag_sensor("Input sags", "vin_sag_count",
                            "{{ value_json.vin_sag_count }}",
                            nullptr, nullptr, "mdi:sine-wave") != ESP_OK) rc = ESP_FAIL;
    if (publish_diag_sensor("Min input voltage", "vin_min",
                            "{{ value_json.vin_min_mv }}",
                            "voltage", "mV", nullptr) != ESP_OK) rc = ESP_FAIL;
    ESP_LOGI(TAG, "publish_diag_discovery");
    return rc;
}

esp_err_t publish_diag_state(const DiagState& d) {
    if (!g_client) return ESP_ERR_INVALID_STATE;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "reset_reason", d.reset_reason);
    cJSON_AddNumberToObject(root, "brownout_count", d.brownout_count);
    cJSON_AddNumberToObject(root, "vin_sag_count", d.vin_sag_count);
    cJSON_AddNumberToObject(root, "vin_min_mv", d.vin_min_mv);

    char* payload = cJSON_PrintUnformatted(root);
    std::string topic = t("/diag/state");
    int id = payload ? esp_mqtt_client_publish(g_client, topic.c_str(), payload, 0, 1, true) : -1;
    if (payload) cJSON_free(payload);
    cJSON_Delete(root);
    return id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t publish_settings(const cn105::Settings& s) {
    if (!g_client) return ESP_ERR_INVALID_STATE;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "power", s.power.c_str());
    cJSON_AddStringToObject(root, "mode", s.mode.c_str());
    cJSON_AddNumberToObject(root, "temperature", s.temperature);
    cJSON_AddStringToObject(root, "fan", s.fan.c_str());
    cJSON_AddStringToObject(root, "vane", s.vane.c_str());
    cJSON_AddStringToObject(root, "wideVane", s.wideVane.c_str());
    cJSON_AddBoolToObject(root, "connected", s.connected);

    char* payload = cJSON_PrintUnformatted(root);
    std::string topic = t("/settings");
    int id = payload ? esp_mqtt_client_publish(g_client, topic.c_str(), payload, 0, 1, true) : -1;
    if (payload) cJSON_free(payload);
    cJSON_Delete(root);
    return id < 0 ? ESP_FAIL : ESP_OK;
}

namespace {
// Map the heat pump's native mode/power to a Home Assistant climate `mode`.
std::string ha_mode(const cn105::Settings& s) {
    if (s.power.empty() || s.power == "OFF") return "off";
    if (s.mode == "HEAT") return "heat";
    if (s.mode == "COOL") return "cool";
    if (s.mode == "DRY")  return "dry";
    if (s.mode == "FAN")  return "fan_only";
    if (s.mode == "AUTO") return "auto";
    return "off";
}

// Map to a Home Assistant climate `hvac_action` (what the unit is doing now).
std::string ha_action(const cn105::Settings& s, const cn105::Status& st) {
    if (s.power.empty() || s.power == "OFF") return "off";
    if (!st.operating)                       return "idle";
    if (s.mode == "HEAT") return "heating";
    if (s.mode == "COOL") return "cooling";
    if (s.mode == "DRY")  return "drying";
    if (s.mode == "FAN")  return "fan";
    return "idle";  // AUTO: direction unknown from settings alone
}
}  // namespace

esp_err_t publish_state(const cn105::Settings& s, const cn105::Status& st) {
    if (!g_client) return ESP_ERR_INVALID_STATE;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", ha_mode(s).c_str());
    cJSON_AddStringToObject(root, "action", ha_action(s, st).c_str());
    cJSON_AddNumberToObject(root, "temperature", s.temperature);
    cJSON_AddNumberToObject(root, "roomTemperature", st.roomTemperature);
    cJSON_AddStringToObject(root, "fan", s.fan.empty() ? "AUTO" : s.fan.c_str());
    cJSON_AddStringToObject(root, "vane", s.vane.empty() ? "AUTO" : s.vane.c_str());
    cJSON_AddBoolToObject(root, "operating", st.operating);
    cJSON_AddNumberToObject(root, "compressorFrequency", st.compressorFrequency);

    char* payload = cJSON_PrintUnformatted(root);
    std::string topic = t("/state");
    int id = payload ? esp_mqtt_client_publish(g_client, topic.c_str(), payload, 0, 1, true) : -1;
    if (payload) cJSON_free(payload);
    cJSON_Delete(root);
    return id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t publish_discovery(const cn105::Settings& s) {
    if (!g_client) return ESP_ERR_INVALID_STATE;
    (void)s;

    std::string state_topic = t("/state");
    std::string avail_topic = t("/availability");
    std::string unique_id   = g_cfg.device_uid + "_climate";

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Heat pump");
    cJSON_AddStringToObject(root, "unique_id", unique_id.c_str());
    cJSON_AddStringToObject(root, "object_id", g_cfg.friendly_name.c_str());

    cJSON_AddStringToObject(root, "availability_topic", avail_topic.c_str());
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");

    cJSON_AddStringToObject(root, "temperature_unit", "C");
    cJSON_AddNumberToObject(root, "min_temp", 16);
    cJSON_AddNumberToObject(root, "max_temp", 31);
    cJSON_AddNumberToObject(root, "temp_step", 0.5);

    // mode
    cJSON_AddStringToObject(root, "mode_command_topic", t("/mode/set").c_str());
    cJSON_AddStringToObject(root, "mode_state_topic", state_topic.c_str());
    cJSON_AddStringToObject(root, "mode_state_template", "{{ value_json.mode }}");
    cJSON* modes = cJSON_CreateArray();
    for (const char* m : {"off", "heat", "cool", "dry", "fan_only", "auto"})
        cJSON_AddItemToArray(modes, cJSON_CreateString(m));
    cJSON_AddItemToObject(root, "modes", modes);

    // target / current temperature
    cJSON_AddStringToObject(root, "temperature_command_topic", t("/temp/set").c_str());
    cJSON_AddStringToObject(root, "temperature_state_topic", state_topic.c_str());
    cJSON_AddStringToObject(root, "temperature_state_template", "{{ value_json.temperature }}");
    cJSON_AddStringToObject(root, "current_temperature_topic", state_topic.c_str());
    cJSON_AddStringToObject(root, "current_temperature_template", "{{ value_json.roomTemperature }}");

    // fan
    cJSON_AddStringToObject(root, "fan_mode_command_topic", t("/fan/set").c_str());
    cJSON_AddStringToObject(root, "fan_mode_state_topic", state_topic.c_str());
    cJSON_AddStringToObject(root, "fan_mode_state_template", "{{ value_json.fan }}");
    cJSON* fans = cJSON_CreateArray();
    for (const char* f : {"AUTO", "QUIET", "1", "2", "3", "4"})
        cJSON_AddItemToArray(fans, cJSON_CreateString(f));
    cJSON_AddItemToObject(root, "fan_modes", fans);

    // swing (vane)
    cJSON_AddStringToObject(root, "swing_mode_command_topic", t("/vane/set").c_str());
    cJSON_AddStringToObject(root, "swing_mode_state_topic", state_topic.c_str());
    cJSON_AddStringToObject(root, "swing_mode_state_template", "{{ value_json.vane }}");
    cJSON* swings = cJSON_CreateArray();
    for (const char* v : {"AUTO", "1", "2", "3", "4", "5", "SWING"})
        cJSON_AddItemToArray(swings, cJSON_CreateString(v));
    cJSON_AddItemToObject(root, "swing_modes", swings);

    // current action
    cJSON_AddStringToObject(root, "action_topic", state_topic.c_str());
    cJSON_AddStringToObject(root, "action_template", "{{ value_json.action }}");

    cJSON_AddItemToObject(root, "device", make_device_block());

    char* payload = cJSON_PrintUnformatted(root);
    std::string topic = "homeassistant/climate/" + g_cfg.friendly_name + "/config";
    int id = payload ? esp_mqtt_client_publish(g_client, topic.c_str(), payload, 0, 1, true) : -1;
    ESP_LOGI(TAG, "publish_discovery -> %s", topic.c_str());
    if (payload) cJSON_free(payload);
    cJSON_Delete(root);
    return id < 0 ? ESP_FAIL : ESP_OK;
}

bool is_connected() { return g_connected; }

}  // namespace hvac_mqtt

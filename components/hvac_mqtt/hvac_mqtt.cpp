/// @file hvac_mqtt.cpp
/// @brief MQTT bridge implementation (esp-mqtt).
///
/// The client lifecycle, topic construction, subscriptions, LWT, and publish
/// helpers are implemented. The inbound-payload -> Command parsing and the JSON
/// state/discovery payloads are left as focused STUBs (TODO) so behaviour is
/// obvious and safe before hardware bring-up.

#include "hvac_mqtt.h"

#include <cstring>
#include <utility>
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"

namespace hvac_mqtt {

static const char* TAG = "hvac_mqtt";

namespace {
Config             g_cfg;
CommandCallback    g_on_command;
esp_mqtt_client_handle_t g_client = nullptr;
bool               g_connected = false;
std::string        g_base;  ///< "<base_topic>/<friendly_name>"

std::string t(const char* suffix) { return g_base + suffix; }

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
    std::string unique_id     = g_cfg.friendly_name + "_firmware";

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Firmware");
    cJSON_AddStringToObject(root, "unique_id", unique_id.c_str());
    cJSON_AddStringToObject(root, "object_id", unique_id.c_str());
    cJSON_AddStringToObject(root, "state_topic", state_topic.c_str());
    cJSON_AddStringToObject(root, "command_topic", command_topic.c_str());
    cJSON_AddStringToObject(root, "payload_install", "install");
    cJSON_AddStringToObject(root, "device_class", "firmware");
    // state_topic carries JSON; let HA pull installed/latest straight from it.
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.installed_version }}");
    cJSON_AddStringToObject(root, "latest_version_template", "{{ value_json.latest_version }}");
    cJSON_AddStringToObject(root, "availability_topic", avail_topic.c_str());
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");

    cJSON* dev = cJSON_CreateObject();
    cJSON* ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(g_cfg.friendly_name.c_str()));
    cJSON_AddItemToObject(dev, "identifiers", ids);
    cJSON_AddStringToObject(dev, "name", g_cfg.friendly_name.c_str());
    cJSON_AddStringToObject(dev, "manufacturer", "Mitsubishi");
    cJSON_AddStringToObject(dev, "model", "CN105 heat-pump bridge");
    cJSON_AddItemToObject(root, "device", dev);

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
                               const std::string& release_summary) {
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

    char* payload = cJSON_PrintUnformatted(root);
    std::string topic = t("/update/state");
    int id = payload ? esp_mqtt_client_publish(g_client, topic.c_str(), payload, 0, 1, true) : -1;
    if (payload) cJSON_free(payload);
    cJSON_Delete(root);
    return id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t publish_settings(const cn105::Settings& s) {
    if (!g_client) return ESP_ERR_INVALID_STATE;
    // TODO(port): serialize Settings to JSON (ArduinoJson -> cJSON) and publish
    // retained to t("/settings"), matching mitsubishi2MQTT's payload shape.
    std::string topic = t("/settings");
    ESP_LOGD(TAG, "publish_settings -> %s (stub)", topic.c_str());
    (void)s;
    return ESP_OK;
}

esp_err_t publish_state(const cn105::Settings& s, const cn105::Status& st) {
    if (!g_client) return ESP_ERR_INVALID_STATE;
    // TODO(port): build the HA climate state JSON (mode/action/temperature/
    // current_temperature/fan/vane) and publish retained to t("/state").
    std::string topic = t("/state");
    ESP_LOGD(TAG, "publish_state -> %s (stub, room=%.1f)", topic.c_str(),
             st.roomTemperature);
    (void)s;
    return ESP_OK;
}

esp_err_t publish_discovery(const cn105::Settings& s) {
    if (!g_client) return ESP_ERR_INVALID_STATE;
    // TODO(port): emit the HA MQTT-discovery climate config, retained, to
    // homeassistant/climate/<friendly_name>/config — reuse the field set from
    // mitsubishi2MQTT (modes, fan_modes, swing_modes, *_command_topic, etc.).
    std::string topic = "homeassistant/climate/" + g_cfg.friendly_name + "/config";
    ESP_LOGI(TAG, "publish_discovery -> %s (stub)", topic.c_str());
    (void)s;
    return ESP_OK;
}

bool is_connected() { return g_connected; }

}  // namespace hvac_mqtt

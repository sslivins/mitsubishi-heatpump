/// @file web_ui.cpp
/// @brief On-device diagnostics/control web server (implementation).

#include "web_ui.h"
#include "wifi_manager.h"

#include <cstring>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "web";

// Embedded gzip'd dashboard — produced at configure time by gzip_html.py.
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

namespace web_ui {

namespace {

httpd_handle_t s_server = nullptr;
Hooks          s_hooks;

void set_cors(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

// ── GET / ──────────────────────────────────────────────────────────────
esp_err_t handle_root(httpd_req_t* req) {
    size_t len = (size_t)(index_html_gz_end - index_html_gz_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char*)index_html_gz_start, len);
}

// ── GET /api/status ────────────────────────────────────────────────────
esp_err_t handle_status(httpd_req_t* req) {
    set_cors(req);
    const esp_app_desc_t* app = esp_app_get_description();
    PowerTelemetry pwr = s_hooks.get_power ? s_hooks.get_power() : PowerTelemetry{};

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", app->version);
    cJSON_AddStringToObject(root, "hostname", CONFIG_MDNS_HOSTNAME);
    cJSON_AddStringToObject(root, "ip", wifi::get_ip());
    cJSON_AddBoolToObject(root, "unit_connected",
                          s_hooks.unit_connected && s_hooks.unit_connected());
    cJSON_AddBoolToObject(root, "mqtt_connected",
                          s_hooks.mqtt_connected && s_hooks.mqtt_connected());
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    cJSON* p = cJSON_CreateObject();
    cJSON_AddBoolToObject(p, "present", pwr.present);
    cJSON_AddNumberToObject(p, "vbat_mv", pwr.vbat_mv);
    cJSON_AddNumberToObject(p, "vin_mv", pwr.vin_mv);
    cJSON_AddStringToObject(p, "source", pwr.source);
    cJSON_AddBoolToObject(p, "charging", pwr.charging);
    cJSON_AddItemToObject(root, "power", p);

    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── GET /api/settings ──────────────────────────────────────────────────
esp_err_t handle_get_settings(httpd_req_t* req) {
    set_cors(req);
    cn105::Settings st = s_hooks.get_settings ? s_hooks.get_settings() : cn105::Settings{};
    cn105::Status   sta = s_hooks.get_status ? s_hooks.get_status() : cn105::Status{};

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "power", st.power.c_str());
    cJSON_AddStringToObject(root, "mode", st.mode.c_str());
    cJSON_AddNumberToObject(root, "temperature", st.temperature);
    cJSON_AddStringToObject(root, "fan", st.fan.c_str());
    cJSON_AddStringToObject(root, "vane", st.vane.c_str());
    cJSON_AddStringToObject(root, "wideVane", st.wideVane.c_str());
    cJSON_AddBoolToObject(root, "connected", st.connected);
    cJSON_AddNumberToObject(root, "roomTemperature", sta.roomTemperature);
    cJSON_AddBoolToObject(root, "operating", sta.operating);
    cJSON_AddNumberToObject(root, "compressorFrequency", sta.compressorFrequency);

    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// Helper: read full request body into a heap buffer (caller frees).
char* recv_body(httpd_req_t* req) {
    int total = req->content_len;
    if (total <= 0 || total > 1024) return nullptr;
    char* buf = (char*)malloc(total + 1);
    if (!buf) return nullptr;
    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, buf + off, total - off);
        if (r <= 0) { free(buf); return nullptr; }
        off += r;
    }
    buf[total] = '\0';
    return buf;
}

void apply(hvac_mqtt::Command::Kind kind, const char* value) {
    if (!s_hooks.apply_command) return;
    hvac_mqtt::Command cmd{kind, value ? value : ""};
    s_hooks.apply_command(cmd);
}

// ── POST /api/settings ─────────────────────────────────────────────────
esp_err_t handle_post_settings(httpd_req_t* req) {
    set_cors(req);
    char* body = recv_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    using K = hvac_mqtt::Command::Kind;
    const cJSON* v;
    if ((v = cJSON_GetObjectItem(json, "power")) && cJSON_IsString(v))
        apply(K::Power, v->valuestring);
    if ((v = cJSON_GetObjectItem(json, "mode")) && cJSON_IsString(v))
        apply(K::Mode, v->valuestring);
    if ((v = cJSON_GetObjectItem(json, "fan")) && cJSON_IsString(v))
        apply(K::Fan, v->valuestring);
    if ((v = cJSON_GetObjectItem(json, "vane")) && cJSON_IsString(v))
        apply(K::Vane, v->valuestring);
    if ((v = cJSON_GetObjectItem(json, "wideVane")) && cJSON_IsString(v))
        apply(K::WideVane, v->valuestring);
    if ((v = cJSON_GetObjectItem(json, "temperature")) && cJSON_IsNumber(v)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", v->valuedouble);
        apply(K::Temperature, buf);
    }
    if ((v = cJSON_GetObjectItem(json, "remoteTemp")) && cJSON_IsNumber(v)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", v->valuedouble);
        apply(K::RemoteTemp, buf);
    }
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// ── POST /api/system/restart ───────────────────────────────────────────
esp_err_t handle_restart(httpd_req_t* req) {
    set_cors(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ── POST /api/system/factory_reset ─────────────────────────────────────
esp_err_t handle_factory_reset(httpd_req_t* req) {
    set_cors(req);
    wifi::erase_credentials();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"reset\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ── OPTIONS /api/* (CORS preflight) ────────────────────────────────────
esp_err_t handle_options(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
}

}  // namespace

esp_err_t init(const Hooks& hooks) {
    s_hooks = hooks;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const httpd_uri_t uris[] = {
        {"/",                        HTTP_GET,     handle_root,           nullptr},
        {"/api/status",              HTTP_GET,     handle_status,         nullptr},
        {"/api/settings",            HTTP_GET,     handle_get_settings,   nullptr},
        {"/api/settings",            HTTP_POST,    handle_post_settings,  nullptr},
        {"/api/system/restart",      HTTP_POST,    handle_restart,        nullptr},
        {"/api/system/factory_reset",HTTP_POST,    handle_factory_reset,  nullptr},
        {"/api/*",                   HTTP_OPTIONS, handle_options,        nullptr},
    };
    for (const auto& u : uris) {
        httpd_register_uri_handler(s_server, &u);
    }

    ESP_LOGI(TAG, "web server started — http://%s/", wifi::get_ip());
    return ESP_OK;
}

}  // namespace web_ui

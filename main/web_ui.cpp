/// @file web_ui.cpp
/// @brief On-device diagnostics/control web server (implementation).

#include "web_ui.h"
#include "wifi_manager.h"
#include "ota.h"
#include "auth_manager.h"

#include <cstring>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
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
    // API responses must never be cached. A stale /api/auth in particular
    // would keep the login gate up (or hidden) even after the real state
    // changed, producing a "stuck on the login page" loop.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

// ── Auth middleware ────────────────────────────────────────────────────
constexpr const char* kCookieName = "mitsubishi_session";

void send_unauthorized(httpd_req_t* req) {
    set_cors(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
}

// Pull the session token out of the Cookie header, if present.
bool get_cookie_token(httpd_req_t* req, char* out, size_t out_len) {
    size_t hlen = httpd_req_get_hdr_value_len(req, "Cookie");
    if (hlen == 0) return false;
    char* cookie = (char*)malloc(hlen + 1);
    if (!cookie) return false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, hlen + 1) != ESP_OK) {
        free(cookie);
        return false;
    }
    bool found = false;
    char needle[40];
    snprintf(needle, sizeof(needle), "%s=", kCookieName);
    char* p = strstr(cookie, needle);
    if (p) {
        p += strlen(needle);
        size_t i = 0;
        while (p[i] && p[i] != ';' && p[i] != ' ' && i < out_len - 1) {
            out[i] = p[i];
            i++;
        }
        out[i] = '\0';
        found = i > 0;
    }
    free(cookie);
    return found;
}

bool get_api_key(httpd_req_t* req, char* out, size_t out_len) {
    size_t hlen = httpd_req_get_hdr_value_len(req, "X-API-Key");
    if (hlen == 0 || hlen >= out_len) return false;
    return httpd_req_get_hdr_value_str(req, "X-API-Key", out, out_len) == ESP_OK;
}

// Authorize a data/control request. Web login is the gate for human/browser
// access to the UI and the data endpoints it calls; when it's disabled the UI
// is open, so these endpoints are too — regardless of the API-key setting,
// which can't distinguish a browser from a script on these shared routes.
// (Mirrors admin_authorized's "no web login → full access".) When web login is
// on, accept a valid session cookie OR, if API auth is on, a valid X-API-Key.
bool api_authorized(httpd_req_t* req) {
    if (!auth_mgr_web_auth_enabled()) return true;
    char tok[AUTH_SESSION_TOKEN_LEN + 1];
    if (get_cookie_token(req, tok, sizeof(tok)) && auth_mgr_validate_session(tok))
        return true;
    if (auth_mgr_api_auth_enabled()) {
        char key[AUTH_API_KEY_LEN + 1];
        if (get_api_key(req, key, sizeof(key)) && auth_mgr_validate_api_key(key))
            return true;
    }
    return false;
}

// True if the request is allowed admin-level access. The climate-only "user"
// role is denied. An API key (when API auth is on) and a web-auth-disabled
// device both count as admin.
bool admin_authorized(httpd_req_t* req) {
    if (!auth_mgr_web_auth_enabled()) return true;  // no web login → full access
    if (auth_mgr_api_auth_enabled()) {
        char key[AUTH_API_KEY_LEN + 1];
        if (get_api_key(req, key, sizeof(key)) && auth_mgr_validate_api_key(key))
            return true;
    }
    char tok[AUTH_SESSION_TOKEN_LEN + 1];
    return get_cookie_token(req, tok, sizeof(tok)) &&
           auth_mgr_session_role(tok) == AUTH_ROLE_ADMIN;
}

void set_session_cookie(httpd_req_t* req, const char* token) {
    // HTTP-only device (no TLS), so the Secure flag is intentionally omitted.
    // NOTE: httpd_resp_set_hdr() stores the pointer WITHOUT copying, so the
    // buffer must stay valid until the response is sent. A stack buffer here
    // would dangle the moment this function returns (the caller then reuses
    // that stack for its JSON body, corrupting the Set-Cookie header). The
    // httpd request handler runs single-threaded, so a static buffer is safe.
    static char hdr[160];
    snprintf(hdr, sizeof(hdr),
             "%s=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%ld",
             kCookieName, token, (long)AUTH_SESSION_LIFETIME_SEC);
    httpd_resp_set_hdr(req, "Set-Cookie", hdr);
}

void clear_session_cookie(httpd_req_t* req) {
    static char hdr[120];  // must outlive the response — see set_session_cookie
    snprintf(hdr, sizeof(hdr),
             "%s=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0", kCookieName);
    httpd_resp_set_hdr(req, "Set-Cookie", hdr);
}

// Guard helper used at the top of protected handlers.
#define REQUIRE_API_AUTH(req)                         \
    do {                                              \
        if (!api_authorized(req)) {                   \
            send_unauthorized(req);                   \
            return ESP_FAIL;                          \
        }                                             \
    } while (0)

// Guard for admin-only handlers: requires API auth AND an admin-level role
// (the climate-only "user" gets a 403).
#define REQUIRE_ADMIN(req)                                                  \
    do {                                                                    \
        if (!api_authorized(req)) { send_unauthorized(req); return ESP_FAIL; } \
        if (!admin_authorized(req)) {                                       \
            set_cors(req);                                                  \
            httpd_resp_set_status(req, "403 Forbidden");                    \
            httpd_resp_set_type(req, "application/json");                   \
            httpd_resp_sendstr(req, "{\"error\":\"admin access required\"}"); \
            return ESP_FAIL;                                                \
        }                                                                   \
    } while (0)


// ── GET / ──────────────────────────────────────────────────────────────
esp_err_t handle_root(httpd_req_t* req) {
    size_t len = (size_t)(index_html_gz_end - index_html_gz_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char*)index_html_gz_start, len);
}

// ── GET /api/status ────────────────────────────────────────────────────
esp_err_t handle_status(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_API_AUTH(req);
    const esp_app_desc_t* app = esp_app_get_description();
    PowerTelemetry pwr = s_hooks.get_power ? s_hooks.get_power() : PowerTelemetry{};

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", app->version);
    cJSON_AddStringToObject(root, "hostname", wifi::mdns_hostname().c_str());
    cJSON_AddStringToObject(root, "ip", wifi::get_ip());
    cJSON_AddNumberToObject(root, "rssi", wifi::get_rssi());
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
    REQUIRE_API_AUTH(req);
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
    REQUIRE_API_AUTH(req);
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
    REQUIRE_ADMIN(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ── POST /api/system/factory_reset ─────────────────────────────────────
esp_err_t handle_factory_reset(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    wifi::erase_credentials();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"reset\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ── POST /api/ota — stream a raw .bin upload into the inactive slot ─────
esp_err_t handle_ota_upload(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    int total = req->content_len;
    if (ota::local_begin(total > 0 ? total : 0) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA busy or no partition");
        return ESP_FAIL;
    }
    char buf[2048];
    int remaining = total;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, sizeof(buf) < (size_t)remaining
                                              ? sizeof(buf) : remaining);
        if (r <= 0) {
            ota::local_abort();
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload interrupted");
            return ESP_FAIL;
        }
        if (ota::local_write(buf, r) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }
        remaining -= r;
    }
    if (ota::local_end() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Image validation failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ── POST /api/ota/url — pull firmware from a URL (background) ───────────
esp_err_t handle_ota_url(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
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
    const cJSON* jurl = cJSON_GetObjectItem(json, "url");
    if (!jurl || !cJSON_IsString(jurl) || strlen(jurl->valuestring) == 0) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing url");
        return ESP_FAIL;
    }
    esp_err_t err = ota::start_url(jurl->valuestring);
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"status\":\"busy\"}");
    }
    return httpd_resp_sendstr(req, "{\"status\":\"started\"}");
}

// ── GET /api/ota/status ────────────────────────────────────────────────
esp_err_t handle_ota_status(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    ota::Status s = ota::get_status();
    const char* state = "idle";
    switch (s.state) {
        case ota::State::InProgress: state = "in_progress"; break;
        case ota::State::Success:    state = "success"; break;
        case ota::State::Failed:     state = "failed"; break;
        default:                     state = "idle"; break;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddNumberToObject(root, "progress", s.progress);
    cJSON_AddStringToObject(root, "message", s.message.c_str());
    cJSON_AddBoolToObject(root, "busy", ota::busy());
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── GET /api/update — cached GitHub release check result ───────────────
esp_err_t handle_update_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    ota::UpdateInfo u = ota::get_update_info();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "current", u.current_version.c_str());
    cJSON_AddStringToObject(root, "latest", u.latest_version.c_str());
    cJSON_AddBoolToObject(root, "update_available", u.update_available);
    cJSON_AddBoolToObject(root, "checking", u.checking);
    cJSON_AddBoolToObject(root, "checked", u.checked);
    cJSON_AddStringToObject(root, "published_at", u.published_at.c_str());
    cJSON_AddStringToObject(root, "release_url", u.release_url.c_str());
    cJSON_AddStringToObject(root, "error", u.error.c_str());
    cJSON_AddStringToObject(root, "last_trigger", u.last_trigger.c_str());
    cJSON_AddStringToObject(root, "last_requester", u.last_requester.c_str());
    int64_t age_s = u.last_checked_ms > 0
                        ? (esp_timer_get_time() / 1000 - u.last_checked_ms) / 1000
                        : -1;
    cJSON_AddNumberToObject(root, "last_check_age_s", (double)age_s);
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── POST /api/update/check — trigger an immediate GitHub poll ──────────
// Build an "IP ua=..." string for the calling client so an unexpected check
// (not the dashboard button) can be traced to its source.
std::string requester_str(httpd_req_t* req) {
    char ip[48] = "unknown";
    int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        struct sockaddr_in6 a;
        socklen_t l = sizeof(a);
        if (getpeername(fd, (struct sockaddr*)&a, &l) == 0)
            inet_ntop(AF_INET6, &a.sin6_addr, ip, sizeof(ip));
    }
    char ua[96] = "";
    size_t ualen = httpd_req_get_hdr_value_len(req, "User-Agent");
    if (ualen > 0 && ualen < sizeof(ua))
        httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua));
    char buf[160];
    snprintf(buf, sizeof(buf), "%s ua=\"%s\"", ip, ua);
    return buf;
}

esp_err_t handle_update_check(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    std::string who = requester_str(req);
    ESP_LOGI(TAG, "/api/update/check from %s", who.c_str());
    ota::check_now(who.c_str());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"checking\"}");
}

// ── POST /api/update/install — install the latest GitHub release ───────
esp_err_t handle_update_install(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    ESP_LOGI(TAG, "/api/update/install from %s", requester_str(req).c_str());
    esp_err_t err = ota::install_latest();
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"status\":\"unavailable\"}");
    }
    return httpd_resp_sendstr(req, "{\"status\":\"started\"}");
}

// ── GET /api/mqtt — current broker settings (password masked) ──────────
esp_err_t handle_mqtt_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    hvac_mqtt::StoredSettings s = hvac_mqtt::get_settings();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "host", s.host.c_str());
    cJSON_AddNumberToObject(root, "port", s.port);
    cJSON_AddStringToObject(root, "username", s.username.c_str());
    cJSON_AddStringToObject(root, "base_topic", s.base_topic.c_str());
    cJSON_AddStringToObject(root, "friendly_name", s.friendly_name.c_str());
    cJSON_AddBoolToObject(root, "password_set", !s.password.empty());
    cJSON_AddBoolToObject(root, "connected",
                          s_hooks.mqtt_connected && s_hooks.mqtt_connected());
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── POST /api/mqtt — save broker settings to NVS, then reboot ───────────
esp_err_t handle_mqtt_post(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
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

    // Start from the current settings so omitted fields are preserved (notably
    // the password, which the GET never returns).
    hvac_mqtt::StoredSettings s = hvac_mqtt::get_settings();
    const cJSON* v;
    if ((v = cJSON_GetObjectItem(json, "host")) && cJSON_IsString(v))
        s.host = v->valuestring;
    if ((v = cJSON_GetObjectItem(json, "port")) && cJSON_IsNumber(v))
        s.port = v->valueint;
    if ((v = cJSON_GetObjectItem(json, "username")) && cJSON_IsString(v))
        s.username = v->valuestring;
    if ((v = cJSON_GetObjectItem(json, "password")) && cJSON_IsString(v))
        s.password = v->valuestring;
    if ((v = cJSON_GetObjectItem(json, "base_topic")) && cJSON_IsString(v))
        s.base_topic = v->valuestring;
    if ((v = cJSON_GetObjectItem(json, "friendly_name")) && cJSON_IsString(v))
        s.friendly_name = v->valuestring;
    cJSON_Delete(json);

    if (s.host.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "host is required");
        return ESP_FAIL;
    }
    if (s.port <= 0) s.port = 1883;

    esp_err_t err = hvac_mqtt::save_settings(s);
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"status\":\"saved\",\"message\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ── GET /api/wifi — current WiFi network (password never returned) ─────
esp_err_t handle_wifi_get(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    wifi::Mode m = wifi::get_mode();
    const char* mode = m == wifi::Mode::CONNECTED    ? "connected"
                     : m == wifi::Mode::CONNECTING   ? "connecting"
                     : m == wifi::Mode::PROVISIONING ? "provisioning"
                                                     : "idle";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", wifi::get_ssid());
    cJSON_AddStringToObject(root, "mode", mode);
    cJSON_AddBoolToObject(root, "connected", wifi::is_connected());
    cJSON_AddStringToObject(root, "ip", wifi::get_ip());
    cJSON_AddStringToObject(root, "ap_name", wifi::get_ap_name());
    cJSON_AddBoolToObject(root, "password_set", wifi::has_password());
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── GET /api/scan — scan nearby WiFi networks (admin only) ────────────
esp_err_t handle_scan(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
    std::string json;
    if (wifi::scan_json(json) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ── POST /api/wifi — set network credentials to NVS, then reboot ───────
esp_err_t handle_wifi_post(httpd_req_t* req) {
    set_cors(req);
    REQUIRE_ADMIN(req);
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

    const cJSON* jssid = cJSON_GetObjectItem(json, "ssid");
    const cJSON* jpass = cJSON_GetObjectItem(json, "password");
    std::string ssid = (jssid && cJSON_IsString(jssid)) ? jssid->valuestring : "";
    // Preserve the stored password when the field is omitted/blank (so the user
    // can change only the SSID without re-typing the key).
    std::string pass;
    if (jpass && cJSON_IsString(jpass) && jpass->valuestring[0] != '\0')
        pass = jpass->valuestring;
    else
        pass = wifi::get_password();
    cJSON_Delete(json);

    if (ssid.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid is required");
        return ESP_FAIL;
    }

    esp_err_t err = wifi::save_credentials(ssid.c_str(), pass.c_str());
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"status\":\"saved\",\"message\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ── POST /api/login ────────────────────────────────────────────────────
esp_err_t handle_login(httpd_req_t* req) {
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
    const cJSON* jp = cJSON_GetObjectItem(json, "password");
    const char* pass = (jp && cJSON_IsString(jp)) ? jp->valuestring : "";

    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    auth_role_t role = auth_mgr_login(pass, token);
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    if (role == AUTH_ROLE_NONE) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid password\"}");
    }
    if (token[0] != '\0') set_session_cookie(req, token);
    const char* rname = role == AUTH_ROLE_ADMIN ? "admin" : "user";
    char out[48];
    snprintf(out, sizeof(out), "{\"status\":\"ok\",\"role\":\"%s\"}", rname);
    return httpd_resp_sendstr(req, out);
}

// ── POST /api/logout ───────────────────────────────────────────────────
esp_err_t handle_logout(httpd_req_t* req) {
    set_cors(req);
    char tok[AUTH_SESSION_TOKEN_LEN + 1];
    if (get_cookie_token(req, tok, sizeof(tok))) auth_mgr_logout(tok);
    clear_session_cookie(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// ── GET /api/auth — auth config + whether this client is authenticated ──
esp_err_t handle_auth_get(httpd_req_t* req) {
    set_cors(req);
    auth_role_t role;
    if (!auth_mgr_web_auth_enabled()) {
        role = AUTH_ROLE_ADMIN;  // no web login → full access
    } else {
        char tok[AUTH_SESSION_TOKEN_LEN + 1];
        role = get_cookie_token(req, tok, sizeof(tok)) ? auth_mgr_session_role(tok)
                                                       : AUTH_ROLE_NONE;
    }
    bool authed = role != AUTH_ROLE_NONE;
    const char* rname = role == AUTH_ROLE_ADMIN ? "admin"
                        : role == AUTH_ROLE_USER ? "user" : "none";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "web_auth_enabled", auth_mgr_web_auth_enabled());
    cJSON_AddBoolToObject(root, "api_auth_enabled", auth_mgr_api_auth_enabled());
    cJSON_AddStringToObject(root, "username", auth_mgr_get_username());
    cJSON_AddStringToObject(root, "role", rname);
    cJSON_AddBoolToObject(root, "web_password_set", auth_mgr_web_password_set());
    cJSON_AddBoolToObject(root, "user_password_set", auth_mgr_user_password_set());
    cJSON_AddBoolToObject(root, "authenticated", authed);
    // Only reveal the API key to an admin client.
    if (role == AUTH_ROLE_ADMIN) {
        char key[AUTH_API_KEY_LEN + 1] = {0};
        if (auth_mgr_get_api_key(key)) cJSON_AddStringToObject(root, "api_key", key);
    }
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── POST /api/auth — update auth settings (admin only) ─────────────────
esp_err_t handle_auth_post(httpd_req_t* req) {
    set_cors(req);
    if (!admin_authorized(req)) { send_unauthorized(req); return ESP_FAIL; }
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

    const cJSON* v;
    bool admin_pw_provided = false;
    // Admin password first, so enabling web auth in the same request uses it.
    if ((v = cJSON_GetObjectItem(json, "password")) && cJSON_IsString(v) &&
        v->valuestring[0] != '\0') {
        if (!auth_mgr_set_admin_password(v->valuestring)) {
            cJSON_Delete(json);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req,
                "{\"error\":\"Admin password must differ from the user password\"}");
        }
        admin_pw_provided = true;
    }

    // Climate-only "user" account: set its password, or remove it.
    if ((v = cJSON_GetObjectItem(json, "user_password")) && cJSON_IsString(v) &&
        v->valuestring[0] != '\0') {
        if (!auth_mgr_set_user_password(v->valuestring)) {
            cJSON_Delete(json);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req,
                "{\"error\":\"User password must differ from the admin password\"}");
        }
    }
    if ((v = cJSON_GetObjectItem(json, "remove_user")) && cJSON_IsTrue(v))
        auth_mgr_set_user_password(nullptr);

    if ((v = cJSON_GetObjectItem(json, "web_auth_enabled")) && cJSON_IsBool(v)) {
        bool want_web = cJSON_IsTrue(v);
        bool was_web = auth_mgr_web_auth_enabled();
        // Turning login ON from OFF requires a freshly-typed admin password in
        // this same request — not merely a password that happens to already be
        // stored (which the user may have set long ago and forgotten). This
        // closes the "flip login on with a blank field" lockout vector.
        if (want_web && !was_web && !admin_pw_provided) {
            cJSON_Delete(json);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req,
                "{\"error\":\"Enter an administrator password to enable login\"}");
            return ESP_FAIL;
        }
        // auth_mgr enforces the "no password → can't enable" invariant too and
        // returns false if violated (defence in depth against a bad state).
        if (!auth_mgr_set_web_auth_enabled(want_web)) {
            cJSON_Delete(json);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req,
                "{\"error\":\"Set a web-UI password before requiring login\"}");
            return ESP_FAIL;
        }
    }
    if ((v = cJSON_GetObjectItem(json, "api_auth_enabled")) && cJSON_IsBool(v))
        auth_mgr_set_api_auth_enabled(cJSON_IsTrue(v));

    char key[AUTH_API_KEY_LEN + 1] = {0};
    if ((v = cJSON_GetObjectItem(json, "regenerate_key")) && cJSON_IsTrue(v))
        auth_mgr_regenerate_api_key(key);
    else
        auth_mgr_get_api_key(key);
    cJSON_Delete(json);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddBoolToObject(root, "web_auth_enabled", auth_mgr_web_auth_enabled());
    cJSON_AddBoolToObject(root, "api_auth_enabled", auth_mgr_api_auth_enabled());
    cJSON_AddBoolToObject(root, "web_password_set", auth_mgr_web_password_set());
    cJSON_AddBoolToObject(root, "user_password_set", auth_mgr_user_password_set());
    if (key[0] != '\0') cJSON_AddStringToObject(root, "api_key", key);
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
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
    auth_mgr_init();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 28;
    cfg.stack_size = 8192;
    // The default 7 sockets reserve 3 for internal use, leaving only 4 for
    // clients — fewer than the 6 keep-alive connections a single browser tab
    // opens, which wedged the server (ERR_EMPTY_RESPONSE). Give real headroom.
    cfg.max_open_sockets = 13;
    // Recycle the least-recently-used connection instead of refusing new ones.
    cfg.lru_purge_enable = true;
    // Reap dead/abandoned connections (closed tab, slept laptop) via TCP
    // keepalive so their sockets don't linger and starve the pool.
    cfg.keep_alive_enable   = true;
    cfg.keep_alive_idle     = 5;   // seconds idle before first probe
    cfg.keep_alive_interval = 5;   // seconds between probes
    cfg.keep_alive_count    = 3;   // drop after 3 unanswered probes

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
        {"/api/ota",                 HTTP_POST,    handle_ota_upload,     nullptr},
        {"/api/ota/url",             HTTP_POST,    handle_ota_url,        nullptr},
        {"/api/ota/status",          HTTP_GET,     handle_ota_status,     nullptr},
        {"/api/update",              HTTP_GET,     handle_update_get,     nullptr},
        {"/api/update/check",        HTTP_POST,    handle_update_check,   nullptr},
        {"/api/update/install",      HTTP_POST,    handle_update_install, nullptr},
        {"/api/mqtt",                HTTP_GET,      handle_mqtt_get,       nullptr},
        {"/api/mqtt",                HTTP_POST,     handle_mqtt_post,      nullptr},
        {"/api/wifi",                HTTP_GET,      handle_wifi_get,       nullptr},
        {"/api/scan",                HTTP_GET,      handle_scan,           nullptr},
        {"/api/wifi",                HTTP_POST,     handle_wifi_post,      nullptr},
        {"/api/system/restart",      HTTP_POST,    handle_restart,        nullptr},
        {"/api/system/factory_reset",HTTP_POST,    handle_factory_reset,  nullptr},
        {"/api/login",               HTTP_POST,    handle_login,          nullptr},
        {"/api/logout",              HTTP_POST,    handle_logout,         nullptr},
        {"/api/auth",                HTTP_GET,      handle_auth_get,       nullptr},
        {"/api/auth",                HTTP_POST,     handle_auth_post,      nullptr},
        {"/api/*",                   HTTP_OPTIONS, handle_options,        nullptr},
    };
    for (const auto& u : uris) {
        httpd_register_uri_handler(s_server, &u);
    }

    ESP_LOGI(TAG, "web server started — http://%s/", wifi::get_ip());
    return ESP_OK;
}

}  // namespace web_ui

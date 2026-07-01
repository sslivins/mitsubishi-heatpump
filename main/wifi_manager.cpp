/// @file wifi_manager.cpp
/// @brief WiFi STA bring-up + SoftAP provisioning fallback.

#include "wifi_manager.h"
#include "dns_server.h"

#include <cstring>
#include <cstdio>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "sdkconfig.h"

// Embedded gzip'd captive-portal page (produced at configure time).
extern const uint8_t portal_html_gz_start[] asm("_binary_portal_html_gz_start");
extern const uint8_t portal_html_gz_end[]   asm("_binary_portal_html_gz_end");

namespace wifi {

static const char* TAG = "wifi";

namespace {
constexpr int kConnectedBit = BIT0;
constexpr int kFailBit      = BIT1;
constexpr char kNvsNs[]     = "wifi";

EventGroupHandle_t s_events = nullptr;
Mode  s_mode = Mode::IDLE;
char  s_ip[16] = "0.0.0.0";
char  s_ap_name[33] = {0};
char  s_ssid[33] = {0};
char  s_pass[65] = {0};
int   s_retries = 0;

httpd_handle_t s_portal = nullptr;
wifi_ap_record_t s_scan[20];
uint16_t s_scan_count = 0;

bool load_creds(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        size_t sl = ssid_len, pl = pass_len;
        esp_err_t e1 = nvs_get_str(h, "ssid", ssid, &sl);
        esp_err_t e2 = nvs_get_str(h, "pass", pass, &pl);
        nvs_close(h);
        if (e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0') return true;
    }
    // Fall back to compile-time creds.
    strlcpy(ssid, CONFIG_WIFI_SSID, ssid_len);
    strlcpy(pass, CONFIG_WIFI_PASSWORD, pass_len);
    return ssid[0] != '\0';
}

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries++ < 5) {
            esp_wifi_connect();
            ESP_LOGW(TAG, "retry STA connect (%d)", s_retries);
        } else {
            xEventGroupSetBits(s_events, kFailBit);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* e = static_cast<ip_event_got_ip_t*>(data);
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof(s_ip));
        s_retries = 0;
        xEventGroupSetBits(s_events, kConnectedBit);
    }
}

// ── mDNS (called once STA is connected) ────────────────────────────────
// A unique hostname avoids ".local" collisions, but on its own it is not
// discoverable (nobody knows the suffix to type). So we also advertise a
// DNS-SD "_http._tcp" service with a unique instance name and TXT metadata —
// the standard Zeroconf/Bonjour pattern (ESPHome/Shelly/printers) that lets a
// controller browse and list every unit without guessing hostnames.
void start_mdns() {
    if (mdns_init() != ESP_OK) return;

    std::string uid      = device_uid();
    std::string host     = mdns_hostname();
    std::string instance = std::string("Mitsubishi Heat Pump ") + uid;
    const char* fw       = esp_app_get_description()->version;

    mdns_hostname_set(host.c_str());
    mdns_instance_name_set(instance.c_str());

    mdns_txt_item_t txt[] = {
        {"id",    uid.c_str()},
        {"fw",    fw},
        {"model", "stamp-s3"},
        {"path",  "/"},
    };
    mdns_service_add(nullptr, "_http", "_tcp", 80, txt,
                     sizeof(txt) / sizeof(txt[0]));

    ESP_LOGI(TAG, "mDNS: %s.local  (_http._tcp instance '%s')",
             host.c_str(), instance.c_str());
}

// ── Captive-portal HTTP handlers ───────────────────────────────────────
esp_err_t portal_page(httpd_req_t* req) {
    size_t len = (size_t)(portal_html_gz_end - portal_html_gz_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char*)portal_html_gz_start, len);
}

// OS captive-portal probes → 302 to the portal so the setup page pops up.
esp_err_t captive_redirect(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// GET /api/scan — blocking scan, returns [{ssid,rssi,auth}].
esp_err_t portal_scan(httpd_req_t* req) {
    wifi_scan_config_t scan_cfg = {};
    esp_wifi_scan_start(&scan_cfg, true);
    s_scan_count = sizeof(s_scan) / sizeof(s_scan[0]);
    esp_wifi_scan_get_ap_records(&s_scan_count, s_scan);

    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < s_scan_count; i++) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (char*)s_scan[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", s_scan[i].rssi);
        cJSON_AddNumberToObject(o, "auth", s_scan[i].authmode);
        cJSON_AddItemToArray(arr, o);
    }
    char* str = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(arr);
    return ESP_OK;
}

// POST /api/connect — { "ssid": "...", "pass": "..." } → save + reboot.
esp_err_t portal_connect(httpd_req_t* req) {
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    cJSON* json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON* jssid = cJSON_GetObjectItem(json, "ssid");
    cJSON* jpass = cJSON_GetObjectItem(json, "pass");
    if (!jssid || !cJSON_IsString(jssid) || strlen(jssid->valuestring) == 0) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }
    const char* ssid = jssid->valuestring;
    const char* pass = (jpass && cJSON_IsString(jpass)) ? jpass->valuestring : "";

    ESP_LOGI(TAG, "portal: saving creds for '%s' and rebooting", ssid);
    save_credentials(ssid, pass);
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"saved\",\"message\":\"Rebooting…\"}");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;  // unreachable
}

esp_err_t start_provisioning() {
    s_mode = Mode::PROVISIONING;

    // AP name carries the device_uid so multiple units are distinct and the
    // SoftAP, mDNS hostname and MQTT node all share one suffix.
    std::snprintf(s_ap_name, sizeof(s_ap_name), "%s-%s",
                  CONFIG_MDNS_HOSTNAME, device_uid().c_str());

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {};
    strlcpy(reinterpret_cast<char*>(ap.ap.ssid), s_ap_name, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = std::strlen(s_ap_name);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    // APSTA so we can scan for networks while serving the portal.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_dns_server();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_portal, &cfg) == ESP_OK) {
        const httpd_uri_t page = {"/", HTTP_GET, portal_page, nullptr};
        httpd_register_uri_handler(s_portal, &page);
        const httpd_uri_t scan = {"/api/scan", HTTP_GET, portal_scan, nullptr};
        httpd_register_uri_handler(s_portal, &scan);
        const httpd_uri_t conn = {"/api/connect", HTTP_POST, portal_connect, nullptr};
        httpd_register_uri_handler(s_portal, &conn);

        static const char* probes[] = {
            "/generate_204", "/gen_204", "/hotspot-detect.html",
            "/library/test/success.html", "/connecttest.txt",
            "/redirect", "/ncsi.txt",
        };
        for (auto* p : probes) {
            const httpd_uri_t r = {p, HTTP_GET, captive_redirect, nullptr};
            httpd_register_uri_handler(s_portal, &r);
        }
        const httpd_uri_t catchall = {"/*", HTTP_GET, captive_redirect, nullptr};
        httpd_register_uri_handler(s_portal, &catchall);

        ESP_LOGW(TAG, "captive portal up at http://192.168.4.1/ (SSID '%s')", s_ap_name);
    }
    return ESP_ERR_NOT_FINISHED;
}
}  // namespace

// ── Identity (public) ──────────────────────────────────────────────────
// Short, stable per-chip id from the low 2 bytes of the factory base MAC
// (Espressif assigns every chip a globally-unique MAC, so this is collision-free
// for a handful of units). Reused for the SoftAP name, the mDNS hostname, and the
// default MQTT friendly_name so all three are unique and consistent.
std::string device_uid() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[5];
    std::snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
    return std::string(buf);
}

std::string mdns_hostname() {
    return std::string(CONFIG_MDNS_HOSTNAME) + "-" + device_uid();
}

esp_err_t init() {
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr, nullptr));

    char ssid[33] = {0}, pass[65] = {0};
    if (!load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "no credentials — entering provisioning");
        return start_provisioning();
    }
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass, sizeof(s_pass));

    s_mode = Mode::CONNECTING;
    esp_netif_create_default_wifi_sta();

    wifi_config_t sta = {};
    strlcpy(reinterpret_cast<char*>(sta.sta.ssid), ssid, sizeof(sta.sta.ssid));
    strlcpy(reinterpret_cast<char*>(sta.sta.password), pass, sizeof(sta.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to '%s'…", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        s_events, kConnectedBit | kFailBit, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(CONFIG_WIFI_CONNECT_TIMEOUT_S * 1000));

    if (bits & kConnectedBit) {
        s_mode = Mode::CONNECTED;
        ESP_LOGI(TAG, "connected, IP %s", s_ip);
        start_mdns();
        return ESP_OK;
    }

    ESP_LOGW(TAG, "STA connect failed — entering provisioning");
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    return start_provisioning();
}

Mode get_mode() { return s_mode; }
bool is_connected() { return s_mode == Mode::CONNECTED; }
const char* get_ip() { return s_ip; }
const char* get_ap_name() { return s_ap_name; }
const char* get_ssid() { return s_ssid; }
bool has_password() { return s_pass[0] != '\0'; }
const char* get_password() { return s_pass; }

// Live RSSI (dBm) of the current STA association, or 0 when not connected.
// Queries the driver each call so callers see the current signal, not the
// value captured at connect time.
int get_rssi() {
    if (s_mode != Mode::CONNECTED) return 0;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

esp_err_t save_credentials(const char* ssid, const char* pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t erase_credentials() {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

}  // namespace wifi

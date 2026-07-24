/// @file wifi_manager.cpp
/// @brief WiFi STA bring-up + SoftAP provisioning fallback.

#include "wifi_manager.h"
#include "dns_server.h"
#include "group_proto.h"

#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "mdns.h"
#include "cJSON.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

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

// User-settable display name (shown in the web UI/login/tab). Decoupled from
// the mDNS hostname and MQTT friendly_name so renaming never breaks Home
// Assistant discovery or .local addressing. Guarded by a mutex because the
// httpd handlers that read it and the POST handler that writes it may run on
// separate tasks.
constexpr char kDeviceNvsNs[] = "device";
SemaphoreHandle_t s_name_mtx = nullptr;
std::string       s_name;
// Web-UI temperature display preference: false = °C (default), true = °F. A pure
// display/input choice for the device web UI — firmware, MQTT and Home Assistant
// values are always °C. Guarded by s_name_mtx (same "device" namespace, both low
// contention).
bool              s_temp_f = false;

void load_name() {
    char buf[64] = {0};
    nvs_handle_t h;
    if (nvs_open(kDeviceNvsNs, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(buf);
        if (nvs_get_str(h, "name", buf, &len) == ESP_OK) s_name = buf;
        uint8_t tf = 0;
        if (nvs_get_u8(h, "tunit", &tf) == ESP_OK) s_temp_f = (tf != 0);
        nvs_close(h);
    }
}

// Trim, strip control characters, and cap to 32 bytes without splitting a
// UTF-8 sequence. Centralised here so every setter (main UI + captive portal)
// stores a consistently clean value.
std::string sanitize_name(const char* in) {
    std::string out;
    for (const char* p = in ? in : ""; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c == 0x7f) continue;  // drop control chars/newlines
        out.push_back(*p);
    }
    size_t a = out.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = out.find_last_not_of(' ');
    out = out.substr(a, b - a + 1);
    constexpr size_t kMax = 32;
    if (out.size() > kMax) {
        size_t cut = kMax;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) cut--;
        out.resize(cut);
    }
    return out;
}

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

    // Also advertise a "_mmhvac._tcp" locator service so a controller (or a
    // sibling head) can browse for shared-compressor group candidates and read
    // their uid / protocol-version without guessing the hostname suffix. The
    // group-id is intentionally omitted — it changes at runtime after pairing;
    // enrolled peers are resolved by hostname (mitsubishi-heatpump-<uid>) and
    // authenticated by HMAC, not discovered by group-id TXT.
    char pv[8];
    snprintf(pv, sizeof(pv), "%d", hvac_group::kProtocolVersion);
    mdns_txt_item_t gtxt[] = {
        {"uid", uid.c_str()},
        {"pv",  pv},
    };
    mdns_service_add(nullptr, "_mmhvac", "_tcp", 80, gtxt,
                     sizeof(gtxt) / sizeof(gtxt[0]));

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
    std::string json;
    if (scan_json(json) != ESP_OK) json = "[]";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// A reboot is what finally applies the saved credentials and tears down the
// SoftAP. We defer it (rather than rebooting immediately in /api/connect) so
// the portal's success screen can stay live while the user copies the device
// URL. The reboot fires on whichever comes first: the user finishing setup
// (/api/finish, e.g. right after they tap Copy) or a device-side safety timer
// (in case the phone/captive sheet disappears and its JS timer never runs).
static esp_timer_handle_t s_reboot_timer = nullptr;
static void reboot_timer_cb(void*) { esp_restart(); }
static void schedule_reboot(uint32_t delay_ms) {
    if (!s_reboot_timer) {
        esp_timer_create_args_t a = {};
        a.callback = &reboot_timer_cb;
        a.dispatch_method = ESP_TIMER_TASK;
        a.name = "provisioning_reboot";
        esp_timer_create(&a, &s_reboot_timer);
    }
    esp_timer_stop(s_reboot_timer);  // harmless if not running; lets us reschedule
    esp_timer_start_once(s_reboot_timer, static_cast<uint64_t>(delay_ms) * 1000ULL);
}

// POST /api/connect — { "ssid": "...", "pass": "..." } → save creds, arm a
// safety reboot, and return the mDNS host so the portal can show its URL.
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

    // Optional friendly display name set during provisioning (sanitised in
    // set_display_name). Handy first-set moment when onboarding many units.
    cJSON* jname = cJSON_GetObjectItem(json, "name");
    if (jname && cJSON_IsString(jname)) set_display_name(jname->valuestring);

    ESP_LOGI(TAG, "portal: saving creds for '%s' (deferred reboot)", ssid);
    save_credentials(ssid, pass);
    cJSON_Delete(json);

    // Hand the portal page the exact mDNS hostname the device will come up on
    // after it reboots, so the success screen can show/copy/open that URL — no
    // IP lookup needed. The hostname is [a-z0-9-] only, so it needs no escaping.
    std::string resp = "{\"status\":\"saved\",\"host\":\"" + mdns_hostname() +
                       "\",\"message\":\"Ready to finish\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp.c_str());

    // Keep the AP (and this page) alive so the user isn't racing a reboot while
    // copying the URL. If they never signal /api/finish (e.g. the iOS captive
    // sheet closed and its JS timer stopped), reboot anyway after 3 minutes so
    // the device always comes up on the home network on its own.
    schedule_reboot(180000);
    return ESP_OK;
}

// POST /api/finish — the user is done with the success screen (typically right
// after copying the URL). Reboot promptly so the device joins the home network;
// a short delay lets this response flush before the SoftAP drops.
esp_err_t portal_finish(httpd_req_t* req) {
    ESP_LOGI(TAG, "portal: finish requested — rebooting");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");
    schedule_reboot(800);
    return ESP_OK;
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
        const httpd_uri_t fin = {"/api/finish", HTTP_POST, portal_finish, nullptr};
        httpd_register_uri_handler(s_portal, &fin);

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

// ── Pairing discovery (public) ─────────────────────────────────────────
// While an "add a zone" pairing window is open, the owning head advertises
// pair=1 in its _mmhvac._tcp TXT (plus the human-readable group label and its
// own display name) so an ungrouped head can *discover* the pairing session
// without the user typing an address. The TXT is cleared the moment the window
// closes (join/cancel/expiry/lockout) — mDNS announces the update so browsers
// stop offering it promptly. The pairing code itself is NEVER advertised; it is
// only ever exchanged over the authenticated claim, so pair=1 leaks nothing but
// "this head is currently accepting one new member".
void set_pairing_advert(bool active, const std::string& glabel,
                        const std::string& name) {
    if (active) {
        mdns_service_txt_item_set("_mmhvac", "_tcp", "pair", "1");
        // Non-empty only; keep the TXT small and avoid empty-value quirks.
        if (!glabel.empty())
            mdns_service_txt_item_set("_mmhvac", "_tcp", "glabel", glabel.c_str());
        else
            mdns_service_txt_item_remove("_mmhvac", "_tcp", "glabel");
        if (!name.empty())
            mdns_service_txt_item_set("_mmhvac", "_tcp", "name", name.c_str());
        else
            mdns_service_txt_item_remove("_mmhvac", "_tcp", "name");
    } else {
        mdns_service_txt_item_remove("_mmhvac", "_tcp", "pair");
        mdns_service_txt_item_remove("_mmhvac", "_tcp", "glabel");
        mdns_service_txt_item_remove("_mmhvac", "_tcp", "name");
    }
}

// Browse the LAN for heads currently advertising pair=1 and return a JSON array
// [{"uid":..,"name":..,"glabel":..,"host":"<ip>"}], excluding ourselves. The
// caller (the ungrouped head's web UI) presents this list so the user taps the
// intended group — the code is then sent to that ONE selected host only, never
// fanned out. Bounded to a handful of results with a short query timeout.
esp_err_t discover_pairing(std::string& out_json) {
    mdns_result_t* results = nullptr;
    esp_err_t err = mdns_query_ptr("_mmhvac", "_tcp", 2500, 20, &results);
    if (err != ESP_OK) return err;

    const std::string self_uid = device_uid();
    cJSON* arr = cJSON_CreateArray();
    for (mdns_result_t* r = results; r; r = r->next) {
        // Read TXT: require pair=1; collect uid/name/glabel.
        bool pairing = false;
        std::string uid, name, glabel;
        for (size_t i = 0; i < r->txt_count; i++) {
            const char* k = r->txt[i].key;
            const char* v = r->txt[i].value ? r->txt[i].value : "";
            if (!k) continue;
            if (!strcmp(k, "pair"))        pairing = (!strcmp(v, "1"));
            else if (!strcmp(k, "uid"))    uid = v;
            else if (!strcmp(k, "name"))   name = v;
            else if (!strcmp(k, "glabel")) glabel = v;
        }
        if (!pairing) continue;
        if (!uid.empty() && uid == self_uid) continue;  // never offer ourselves

        // First IPv4 address wins.
        char ip[16] = {0};
        for (mdns_ip_addr_t* a = r->addr; a; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                uint32_t v = a->addr.u_addr.ip4.addr;  // LSB = first octet
                snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                         (unsigned)(v & 0xff), (unsigned)((v >> 8) & 0xff),
                         (unsigned)((v >> 16) & 0xff), (unsigned)((v >> 24) & 0xff));
                break;
            }
        }
        if (ip[0] == '\0') continue;  // no usable address

        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "uid",    uid.c_str());
        cJSON_AddStringToObject(o, "name",   name.c_str());
        cJSON_AddStringToObject(o, "glabel", glabel.c_str());
        cJSON_AddStringToObject(o, "host",   ip);
        cJSON_AddItemToArray(arr, o);
    }
    mdns_query_results_free(results);

    char* s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!s) return ESP_ERR_NO_MEM;
    out_json = s;
    cJSON_free(s);
    return ESP_OK;
}

// ── Identity (public) ──────────────────────────────────────────────────
// Short, stable per-chip id from the low 2 bytes of the factory base MAC
// (Espressif assigns every chip a globally-unique MAC, so this is collision-free
// for a handful of units). Reused for the SoftAP name, the mDNS hostname, and the
// default MQTT friendly_name so all three are unique and consistent.
std::string device_uid() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[5];
    std::snprintf(buf, sizeof(buf), "%02x%02x", mac[4], mac[5]);
    return std::string(buf);
}

std::string mdns_hostname() {
    return std::string(CONFIG_MDNS_HOSTNAME) + "-" + device_uid();
}

std::string device_display_name() {
    if (!s_name_mtx) return s_name;  // pre-init (shouldn't happen post-boot)
    xSemaphoreTake(s_name_mtx, portMAX_DELAY);
    std::string copy = s_name;
    xSemaphoreGive(s_name_mtx);
    return copy;
}

esp_err_t set_display_name(const char* name) {
    std::string n = sanitize_name(name);
    nvs_handle_t h;
    esp_err_t err = nvs_open(kDeviceNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "name", n.c_str());
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;  // keep the old cached value on failure
    xSemaphoreTake(s_name_mtx, portMAX_DELAY);
    s_name = n;
    xSemaphoreGive(s_name_mtx);
    return ESP_OK;
}

bool temp_unit_fahrenheit() {
    if (!s_name_mtx) return s_temp_f;
    xSemaphoreTake(s_name_mtx, portMAX_DELAY);
    bool v = s_temp_f;
    xSemaphoreGive(s_name_mtx);
    return v;
}

esp_err_t set_temp_unit(bool fahrenheit) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kDeviceNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "tunit", fahrenheit ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;  // keep the old cached value on failure
    if (s_name_mtx) xSemaphoreTake(s_name_mtx, portMAX_DELAY);
    s_temp_f = fahrenheit;
    if (s_name_mtx) xSemaphoreGive(s_name_mtx);
    return ESP_OK;
}

esp_err_t init() {
    s_events = xEventGroupCreate();
    s_name_mtx = xSemaphoreCreateMutex();
    load_name();
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

int get_auth() {
    if (s_mode != Mode::CONNECTED) return -1;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return -1;
    return ap.authmode;
}

esp_err_t scan_json(std::string& out) {
    wifi_scan_config_t scan_cfg = {};
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  // blocking
    if (err != ESP_OK) return err;
    s_scan_count = sizeof(s_scan) / sizeof(s_scan[0]);
    esp_wifi_scan_get_ap_records(&s_scan_count, s_scan);

    // Dedup by SSID keeping the strongest record, drop hidden/empty, sort desc.
    struct Net { std::string ssid; int8_t rssi; uint8_t auth; };
    std::vector<Net> nets;
    for (int i = 0; i < s_scan_count; i++) {
        const char* ssid = reinterpret_cast<const char*>(s_scan[i].ssid);
        if (ssid[0] == '\0') continue;
        auto it = std::find_if(nets.begin(), nets.end(),
                               [&](const Net& n) { return n.ssid == ssid; });
        if (it == nets.end()) {
            nets.push_back({ssid, s_scan[i].rssi,
                            static_cast<uint8_t>(s_scan[i].authmode)});
        } else if (s_scan[i].rssi > it->rssi) {
            it->rssi = s_scan[i].rssi;
            it->auth = static_cast<uint8_t>(s_scan[i].authmode);
        }
    }
    std::sort(nets.begin(), nets.end(),
              [](const Net& a, const Net& b) { return a.rssi > b.rssi; });

    cJSON* arr = cJSON_CreateArray();
    for (const auto& n : nets) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", n.ssid.c_str());
        cJSON_AddNumberToObject(o, "rssi", n.rssi);
        cJSON_AddNumberToObject(o, "auth", n.auth);
        cJSON_AddItemToArray(arr, o);
    }
    char* str = cJSON_PrintUnformatted(arr);
    out = str ? str : "[]";
    cJSON_free(str);
    cJSON_Delete(arr);
    return ESP_OK;
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

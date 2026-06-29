/// @file wifi_manager.cpp
/// @brief WiFi STA bring-up + SoftAP provisioning fallback.

#include "wifi_manager.h"

#include <cstring>
#include <cstdio>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "sdkconfig.h"

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
int   s_retries = 0;

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

esp_err_t start_provisioning() {
    s_mode = Mode::PROVISIONING;
    std::snprintf(s_ap_name, sizeof(s_ap_name), "%s-setup", CONFIG_MDNS_HOSTNAME);

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {};
    strlcpy(reinterpret_cast<char*>(ap.ap.ssid), s_ap_name, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = std::strlen(s_ap_name);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGW(TAG, "provisioning SoftAP '%s' up — connect to set credentials", s_ap_name);
    // TODO: serve a captive-portal page (port arctic-sniffer dns_server + portal
    // HTML) that calls wifi::save_credentials() then reboots.
    return ESP_ERR_NOT_FINISHED;
}
}  // namespace

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

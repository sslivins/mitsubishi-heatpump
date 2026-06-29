/// @file ota.cpp
/// @brief OTA firmware update implementation.

#include "ota.h"

#include <cstring>
#include <mutex>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ota";

namespace ota {

namespace {

std::mutex  s_mtx;
Status      s_status;
bool        s_busy = false;

// Local-upload streaming state.
esp_ota_handle_t       s_handle    = 0;
const esp_partition_t* s_partition = nullptr;
size_t                 s_total     = 0;
size_t                 s_written   = 0;

void set_status(State state, int progress, const char* msg) {
    std::lock_guard<std::mutex> lock(s_mtx);
    s_status.state = state;
    s_status.progress = progress;
    s_status.message = msg ? msg : "";
}

// Atomically claim the OTA pipeline; returns false if an update is already busy.
bool try_claim() {
    std::lock_guard<std::mutex> lock(s_mtx);
    if (s_busy) return false;
    s_busy = true;
    return true;
}

void release() {
    std::lock_guard<std::mutex> lock(s_mtx);
    s_busy = false;
}

void url_task(void* arg) {
    std::string* url_ptr = static_cast<std::string*>(arg);
    std::string url = *url_ptr;
    delete url_ptr;

    ESP_LOGI(TAG, "URL OTA: %s", url.c_str());
    set_status(State::InProgress, -1, "connecting");

    esp_http_client_config_t http = {};
    http.url = url.c_str();
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = 20000;
    http.keep_alive_enable = true;
    // GitHub release downloads 302-redirect to a long presigned CDN URL; the
    // default 512-byte buffers overflow ("HTTP_CLIENT: Out of buffer") when the
    // redirected request line is written. Size them up so redirects work.
    http.buffer_size = 4096;
    http.buffer_size_tx = 4096;

    esp_https_ota_config_t cfg = {};
    cfg.http_config = &http;

    esp_https_ota_handle_t h = nullptr;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK || h == nullptr) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        set_status(State::Failed, 0, esp_err_to_name(err));
        release();
        vTaskDelete(nullptr);
        return;
    }

    int image_size = esp_https_ota_get_image_size(h);
    set_status(State::InProgress, image_size > 0 ? 0 : -1, "downloading");

    while (true) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int read = esp_https_ota_get_image_len_read(h);
        if (image_size > 0) {
            int pct = (int)((int64_t)read * 100 / image_size);
            set_status(State::InProgress, pct, "downloading");
        }
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        err = esp_https_ota_finish(h);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "URL OTA complete — rebooting");
            set_status(State::Success, 100, "complete; rebooting");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
            set_status(State::Failed, 0, esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "URL OTA failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        set_status(State::Failed, 0, esp_err_to_name(err));
    }
    release();
    vTaskDelete(nullptr);
}

}  // namespace

void init() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
        ESP_LOGI(TAG, "running '%s' (state=%d, version=%s)", running->label,
                 (int)st, esp_app_get_description()->version);
    }
}

void mark_valid() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "image marked valid — rollback cancelled");
        } else {
            ESP_LOGW(TAG, "failed to mark image valid");
        }
    }
}

esp_err_t local_begin(size_t total_size) {
    if (!try_claim()) {
        ESP_LOGW(TAG, "local OTA rejected — already busy");
        return ESP_ERR_INVALID_STATE;
    }
    s_partition = esp_ota_get_next_update_partition(nullptr);
    if (!s_partition) {
        set_status(State::Failed, 0, "no OTA partition");
        release();
        return ESP_FAIL;
    }
    s_total = total_size;
    s_written = 0;
    esp_err_t err = esp_ota_begin(s_partition, OTA_SIZE_UNKNOWN, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        set_status(State::Failed, 0, esp_err_to_name(err));
        release();
        return err;
    }
    ESP_LOGI(TAG, "local OTA -> '%s' (%u bytes)", s_partition->label,
             (unsigned)total_size);
    set_status(State::InProgress, s_total ? 0 : -1, "uploading");
    return ESP_OK;
}

esp_err_t local_write(const void* data, size_t len) {
    if (!s_handle) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        local_abort();
        return err;
    }
    s_written += len;
    if (s_total > 0) {
        int pct = (int)((int64_t)s_written * 100 / s_total);
        set_status(State::InProgress, pct, "uploading");
    }
    return ESP_OK;
}

esp_err_t local_end() {
    if (!s_handle) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        set_status(State::Failed, 0, esp_err_to_name(err));
        release();
        return err;
    }
    err = esp_ota_set_boot_partition(s_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        set_status(State::Failed, 0, esp_err_to_name(err));
        release();
        return err;
    }
    ESP_LOGI(TAG, "local OTA complete (%u bytes) — boot set to '%s'",
             (unsigned)s_written, s_partition->label);
    set_status(State::Success, 100, "complete; rebooting");
    release();
    return ESP_OK;
}

void local_abort() {
    if (s_handle) {
        esp_ota_abort(s_handle);
        s_handle = 0;
    }
    set_status(State::Failed, 0, "aborted");
    release();
}

esp_err_t start_url(const std::string& url) {
    if (url.empty()) return ESP_ERR_INVALID_ARG;
    if (busy()) {
        ESP_LOGW(TAG, "URL OTA rejected — already busy");
        return ESP_ERR_INVALID_STATE;
    }
    if (!try_claim()) return ESP_ERR_INVALID_STATE;
    // try_claim succeeded; hand ownership of the busy flag to the task.
    auto* arg = new std::string(url);
    if (xTaskCreate(url_task, "ota_url", 8192, arg, 5, nullptr) != pdPASS) {
        delete arg;
        set_status(State::Failed, 0, "task spawn failed");
        release();
        return ESP_FAIL;
    }
    return ESP_OK;
}

Status get_status() {
    std::lock_guard<std::mutex> lock(s_mtx);
    return s_status;
}

bool busy() {
    std::lock_guard<std::mutex> lock(s_mtx);
    return s_busy;
}

}  // namespace ota

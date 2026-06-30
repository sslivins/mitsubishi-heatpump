/// @file auth_manager.cpp
/// @brief Implementation of web session auth + API-key auth.
///
/// Ported from sslivins/arctic-controller (auth_manager.cpp), trimmed to the
/// pieces this firmware needs. Web auth and API auth both default OFF.
#include "auth_manager.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/sha256.h"

static const char* TAG = "auth_mgr";

// NVS namespace and keys
static const char* NVS_NAMESPACE = "auth";
static const char* NVS_KEY_WEB_ENABLED = "web_en";
static const char* NVS_KEY_API_ENABLED = "api_en";
static const char* NVS_KEY_USERNAME = "username";
static const char* NVS_KEY_PASS_HASH = "pass_hash";
static const char* NVS_KEY_API_KEY = "api_key";

typedef struct {
    bool active;
    char token[AUTH_SESSION_TOKEN_LEN + 1];
    time_t created_at;
    time_t last_used;
} session_t;

static struct {
    bool initialized;
    bool web_auth_enabled;
    bool api_auth_enabled;
    char username[AUTH_MAX_USERNAME_LEN + 1];
    uint8_t password_hash[32];  // SHA-256
    bool password_set;
    char api_key[AUTH_API_KEY_LEN + 1];
    session_t sessions[AUTH_MAX_SESSIONS];
} state = {};

// ── Helpers ────────────────────────────────────────────────────────────────

static void generate_random_hex(char* buffer, size_t len) {
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        buffer[i] = hex_chars[esp_random() % 16];
    }
    buffer[len] = '\0';
}

static void hash_password(const char* password, uint8_t* hash_out) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA-256 (not SHA-224)
    mbedtls_sha256_update(&ctx, (const unsigned char*)password, strlen(password));
    mbedtls_sha256_finish(&ctx, hash_out);
    mbedtls_sha256_free(&ctx);
}

static bool load_from_nvs(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "no auth settings in NVS, using defaults");
        return false;
    }

    uint8_t enabled = 0;
    if (nvs_get_u8(nvs, NVS_KEY_WEB_ENABLED, &enabled) == ESP_OK)
        state.web_auth_enabled = enabled != 0;
    if (nvs_get_u8(nvs, NVS_KEY_API_ENABLED, &enabled) == ESP_OK)
        state.api_auth_enabled = enabled != 0;

    size_t len = sizeof(state.username);
    if (nvs_get_str(nvs, NVS_KEY_USERNAME, state.username, &len) != ESP_OK)
        state.username[0] = '\0';

    len = sizeof(state.password_hash);
    if (nvs_get_blob(nvs, NVS_KEY_PASS_HASH, state.password_hash, &len) == ESP_OK)
        state.password_set = true;

    len = sizeof(state.api_key);
    if (nvs_get_str(nvs, NVS_KEY_API_KEY, state.api_key, &len) != ESP_OK)
        state.api_key[0] = '\0';

    nvs_close(nvs);
    return true;
}

static bool save_to_nvs(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }

    nvs_set_u8(nvs, NVS_KEY_WEB_ENABLED, state.web_auth_enabled ? 1 : 0);
    nvs_set_u8(nvs, NVS_KEY_API_ENABLED, state.api_auth_enabled ? 1 : 0);
    if (state.username[0] != '\0')
        nvs_set_str(nvs, NVS_KEY_USERNAME, state.username);
    if (state.password_set)
        nvs_set_blob(nvs, NVS_KEY_PASS_HASH, state.password_hash, sizeof(state.password_hash));
    else
        nvs_erase_key(nvs, NVS_KEY_PASS_HASH);  // no-op if absent
    if (state.api_key[0] != '\0')
        nvs_set_str(nvs, NVS_KEY_API_KEY, state.api_key);

    nvs_commit(nvs);
    nvs_close(nvs);
    return true;
}

static session_t* find_session(const char* token) {
    if (token == NULL || token[0] == '\0') return NULL;
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (state.sessions[i].active && strcmp(state.sessions[i].token, token) == 0)
            return &state.sessions[i];
    }
    return NULL;
}

static session_t* find_free_session(void) {
    session_t* oldest = NULL;
    time_t oldest_time = 0;
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (!state.sessions[i].active) return &state.sessions[i];
        if (oldest == NULL || state.sessions[i].last_used < oldest_time) {
            oldest = &state.sessions[i];
            oldest_time = state.sessions[i].last_used;
        }
    }
    if (oldest != NULL) {  // all slots full → evict oldest
        oldest->active = false;
        return oldest;
    }
    return NULL;
}

static bool is_session_expired(const session_t* session) {
    if (session == NULL || !session->active) return true;
    return (time(NULL) - session->created_at) > AUTH_SESSION_LIFETIME_SEC;
}

// ── Public API ───────────────────────────────────────────────────────────

void auth_mgr_init(void) {
    if (state.initialized) return;

    memset(&state, 0, sizeof(state));
    bool loaded = load_from_nvs();

    // Default username for convenience, but intentionally NO default password:
    // a fresh device has web auth OFF and no password, and the web handler
    // refuses to enable web auth until the user sets one.
    if (state.username[0] == '\0') {
        strncpy(state.username, "admin", sizeof(state.username) - 1);
        state.username[sizeof(state.username) - 1] = '\0';
        save_to_nvs();
    }

    // Migration: older firmware auto-seeded admin/admin. If a device still
    // carries that exact default, clear it so there is no default password.
    // A user-chosen password (anything but "admin") is left untouched.
    if (state.password_set && strcmp(state.username, "admin") == 0) {
        uint8_t admin_hash[32];
        hash_password("admin", admin_hash);
        if (memcmp(state.password_hash, admin_hash, sizeof(admin_hash)) == 0) {
            state.password_set = false;
            memset(state.password_hash, 0, sizeof(state.password_hash));
            state.web_auth_enabled = false;  // can't require login with no password
            save_to_nvs();
            ESP_LOGI(TAG, "cleared legacy default admin password");
        }
    }

    if (state.api_key[0] == '\0') {
        generate_random_hex(state.api_key, AUTH_API_KEY_LEN);
        save_to_nvs();
    }

    state.initialized = true;
    ESP_LOGI(TAG, "auth ready — web_auth=%s api_auth=%s",
             state.web_auth_enabled ? "on" : "off",
             state.api_auth_enabled ? "on" : "off");
}

// ── Web auth ─────────────────────────────────────────────────────────────

bool auth_mgr_web_auth_enabled(void) { return state.web_auth_enabled; }

void auth_mgr_set_web_auth_enabled(bool enabled) {
    if (state.web_auth_enabled == enabled) return;
    state.web_auth_enabled = enabled;
    save_to_nvs();
    if (!enabled) auth_mgr_logout_all();
    ESP_LOGI(TAG, "web auth %s", enabled ? "enabled" : "disabled");
}

bool auth_mgr_set_credentials(const char* username, const char* password) {
    bool changed = false;
    if (username != NULL && username[0] != '\0') {
        strncpy(state.username, username, AUTH_MAX_USERNAME_LEN);
        state.username[AUTH_MAX_USERNAME_LEN] = '\0';
        changed = true;
    }
    if (password != NULL && password[0] != '\0') {
        hash_password(password, state.password_hash);
        state.password_set = true;
        changed = true;
    }
    if (changed) {
        save_to_nvs();
        auth_mgr_logout_all();  // force re-login after a credential change
    }
    return true;
}

const char* auth_mgr_get_username(void) { return state.username; }

bool auth_mgr_web_password_set(void) { return state.password_set; }

bool auth_mgr_login(const char* username, const char* password, char* session_token) {
    if (!state.web_auth_enabled) return true;  // auth disabled → always ok
    if (username == NULL || password == NULL) return false;
    if (strcmp(username, state.username) != 0) return false;
    if (!state.password_set) return false;

    uint8_t input_hash[32];
    hash_password(password, input_hash);
    if (memcmp(input_hash, state.password_hash, 32) != 0) return false;

    session_t* session = find_free_session();
    if (session == NULL) return false;

    session->active = true;
    generate_random_hex(session->token, AUTH_SESSION_TOKEN_LEN);
    session->created_at = time(NULL);
    session->last_used = session->created_at;
    if (session_token != NULL) strcpy(session_token, session->token);

    ESP_LOGI(TAG, "login ok, session created");
    return true;
}

bool auth_mgr_validate_session(const char* token) {
    if (!state.web_auth_enabled) return true;
    session_t* session = find_session(token);
    if (session == NULL) return false;
    if (is_session_expired(session)) {
        session->active = false;
        return false;
    }
    session->last_used = time(NULL);
    return true;
}

void auth_mgr_logout(const char* token) {
    session_t* session = find_session(token);
    if (session != NULL) session->active = false;
}

void auth_mgr_logout_all(void) {
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) state.sessions[i].active = false;
}

// ── API-key auth ─────────────────────────────────────────────────────────

bool auth_mgr_api_auth_enabled(void) { return state.api_auth_enabled; }

void auth_mgr_set_api_auth_enabled(bool enabled) {
    if (state.api_auth_enabled == enabled) return;
    state.api_auth_enabled = enabled;
    save_to_nvs();
    ESP_LOGI(TAG, "API auth %s", enabled ? "enabled" : "disabled");
}

bool auth_mgr_get_api_key(char* buffer) {
    if (buffer == NULL) return false;
    if (state.api_key[0] == '\0') { buffer[0] = '\0'; return false; }
    strcpy(buffer, state.api_key);
    return true;
}

bool auth_mgr_regenerate_api_key(char* buffer) {
    generate_random_hex(state.api_key, AUTH_API_KEY_LEN);
    save_to_nvs();
    if (buffer != NULL) strcpy(buffer, state.api_key);
    ESP_LOGI(TAG, "API key regenerated");
    return true;
}

bool auth_mgr_validate_api_key(const char* key) {
    if (!state.api_auth_enabled) return true;
    if (key == NULL || key[0] == '\0') return false;
    return strcmp(key, state.api_key) == 0;
}

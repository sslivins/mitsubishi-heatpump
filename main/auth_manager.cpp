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
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/sha256.h"

static const char* TAG = "auth_mgr";

// Monotonic seconds since boot. Session lifetimes MUST use this rather than
// time(NULL): the wall clock jumps forward when SNTP syncs (typically a few
// seconds after an OTA reboot), which would otherwise instantly "expire" every
// session created before the sync and log the user straight back out.
static time_t monotonic_now(void) {
    return (time_t)(esp_timer_get_time() / 1000000);
}

// NVS namespace and keys
static const char* NVS_NAMESPACE = "auth";
static const char* NVS_KEY_WEB_ENABLED = "web_en";
static const char* NVS_KEY_API_ENABLED = "api_en";
static const char* NVS_KEY_USERNAME = "username";
static const char* NVS_KEY_PASS_HASH = "pass_hash";
static const char* NVS_KEY_USER_HASH = "user_hash";
static const char* NVS_KEY_API_KEY = "api_key";

typedef struct {
    bool active;
    char token[AUTH_SESSION_TOKEN_LEN + 1];
    auth_role_t role;
    time_t created_at;
    time_t last_used;
} session_t;

static struct {
    bool initialized;
    bool web_auth_enabled;
    bool api_auth_enabled;
    char username[AUTH_MAX_USERNAME_LEN + 1];
    uint8_t password_hash[32];  // SHA-256, admin
    bool password_set;
    uint8_t user_password_hash[32];  // SHA-256, shared climate-only user
    bool user_password_set;
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

    len = sizeof(state.user_password_hash);
    if (nvs_get_blob(nvs, NVS_KEY_USER_HASH, state.user_password_hash, &len) == ESP_OK)
        state.user_password_set = true;

    len = sizeof(state.api_key);
    if (nvs_get_str(nvs, NVS_KEY_API_KEY, state.api_key, &len) != ESP_OK)
        state.api_key[0] = '\0';

    nvs_close(nvs);
    return true;
}

// Persist the whole auth state. Returns ESP_OK only if the write AND commit
// both succeed — callers MUST treat a non-OK result as "not persisted" so RAM
// state is never reported as saved when NVS actually rejected it. A half-write
// that survives a reboot is a lockout vector (e.g. web_auth on but no password
// hash), so we surface failures rather than silently swallow them.
static esp_err_t save_to_nvs(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    // erase_key returns ESP_ERR_NVS_NOT_FOUND when the key is absent — that is a
    // benign no-op, not a failure, so it is excluded from the error accumulator.
    esp_err_t e = ESP_OK;
    auto acc = [&](esp_err_t r) { if (r != ESP_OK && e == ESP_OK) e = r; };

    acc(nvs_set_u8(nvs, NVS_KEY_WEB_ENABLED, state.web_auth_enabled ? 1 : 0));
    acc(nvs_set_u8(nvs, NVS_KEY_API_ENABLED, state.api_auth_enabled ? 1 : 0));
    if (state.username[0] != '\0')
        acc(nvs_set_str(nvs, NVS_KEY_USERNAME, state.username));
    if (state.password_set)
        acc(nvs_set_blob(nvs, NVS_KEY_PASS_HASH, state.password_hash, sizeof(state.password_hash)));
    else {
        esp_err_t r = nvs_erase_key(nvs, NVS_KEY_PASS_HASH);
        if (r != ESP_ERR_NVS_NOT_FOUND) acc(r);
    }
    if (state.user_password_set)
        acc(nvs_set_blob(nvs, NVS_KEY_USER_HASH, state.user_password_hash, sizeof(state.user_password_hash)));
    else {
        esp_err_t r = nvs_erase_key(nvs, NVS_KEY_USER_HASH);
        if (r != ESP_ERR_NVS_NOT_FOUND) acc(r);
    }
    if (state.api_key[0] != '\0')
        acc(nvs_set_str(nvs, NVS_KEY_API_KEY, state.api_key));

    acc(nvs_commit(nvs));
    nvs_close(nvs);
    if (e != ESP_OK) ESP_LOGE(TAG, "auth NVS save failed: %s", esp_err_to_name(e));
    return e;
}

static session_t* find_session(const char* token) {
    if (token == NULL || token[0] == '\0') return NULL;
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (state.sessions[i].active && strcmp(state.sessions[i].token, token) == 0)
            return &state.sessions[i];
    }
    return NULL;
}

// Find a slot for a new session of @p new_role. A free slot is always preferred;
// otherwise the oldest evictable session is reclaimed. A climate-only USER login
// must never evict an ADMIN session — doing so could knock the only admin out
// while the admin password is forgotten (a lockout vector), so user logins only
// reclaim other user slots and are refused when none are available.
static session_t* find_free_session(auth_role_t new_role) {
    session_t* oldest = NULL;
    time_t oldest_time = 0;
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (!state.sessions[i].active) return &state.sessions[i];
        bool evictable = (new_role == AUTH_ROLE_ADMIN) ||
                         (state.sessions[i].role != AUTH_ROLE_ADMIN);
        if (evictable && (oldest == NULL || state.sessions[i].last_used < oldest_time)) {
            oldest = &state.sessions[i];
            oldest_time = state.sessions[i].last_used;
        }
    }
    if (oldest != NULL) {  // all slots full → evict oldest evictable session
        oldest->active = false;
        return oldest;
    }
    return NULL;
}

static bool is_session_expired(const session_t* session) {
    if (session == NULL || !session->active) return true;
    return (monotonic_now() - session->created_at) > AUTH_SESSION_LIFETIME_SEC;
}

// ── Public API ───────────────────────────────────────────────────────────

void auth_mgr_init(void) {
    if (state.initialized) return;

    memset(&state, 0, sizeof(state));
    load_from_nvs();

    // The admin username is fixed to "admin" and cannot be changed.
    if (strcmp(state.username, "admin") != 0) {
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

    // Startup invariant repair (lockout guard of last resort): web auth must
    // never be enabled without an admin password to log in with. A partial NVS
    // write, corruption, or a future code path could persist that impossible
    // state; self-heal it here so a device always boots into a reachable UI.
    if (state.web_auth_enabled && !state.password_set) {
        ESP_LOGW(TAG, "web auth enabled with no admin password — disabling to avoid lockout");
        state.web_auth_enabled = false;
        save_to_nvs();
    }

    state.initialized = true;
    ESP_LOGI(TAG, "auth ready — web_auth=%s api_auth=%s",
             state.web_auth_enabled ? "on" : "off",
             state.api_auth_enabled ? "on" : "off");
}

// ── Web auth ─────────────────────────────────────────────────────────────

bool auth_mgr_web_auth_enabled(void) { return state.web_auth_enabled; }

bool auth_mgr_set_web_auth_enabled(bool enabled) {
    // Enforce the core invariant at the manager layer, not just in the HTTP
    // handler: never enable web auth without an admin password, or the UI locks
    // out with no recovery. This makes the guard hold for every caller.
    if (enabled && !state.password_set) {
        ESP_LOGW(TAG, "refused to enable web auth: no admin password set");
        return false;
    }
    if (state.web_auth_enabled == enabled) return true;
    state.web_auth_enabled = enabled;
    save_to_nvs();
    if (!enabled) auth_mgr_logout_all();
    ESP_LOGI(TAG, "web auth %s", enabled ? "enabled" : "disabled");
    return true;
}

bool auth_mgr_set_admin_password(const char* password) {
    if (password == NULL || password[0] == '\0') return false;
    uint8_t h[32];
    hash_password(password, h);
    // The admin and user passwords must differ. Login is password-only and the
    // admin is matched first, so an identical user password would silently
    // resolve to admin (a privilege escalation). Reject the collision.
    if (state.user_password_set && memcmp(h, state.user_password_hash, 32) == 0)
        return false;
    memcpy(state.password_hash, h, 32);
    state.password_set = true;
    save_to_nvs();
    auth_mgr_logout_all();  // force re-login after a credential change
    return true;
}

bool auth_mgr_set_user_password(const char* password) {
    if (password == NULL || password[0] == '\0') {
        // Remove (disable) the shared climate-only user account.
        state.user_password_set = false;
        memset(state.user_password_hash, 0, sizeof(state.user_password_hash));
    } else {
        uint8_t h[32];
        hash_password(password, h);
        // Must differ from the admin password (see auth_mgr_set_admin_password):
        // a user password equal to admin's would log the user in as admin.
        if (state.password_set && memcmp(h, state.password_hash, 32) == 0)
            return false;
        memcpy(state.user_password_hash, h, 32);
        state.user_password_set = true;
    }
    save_to_nvs();
    // Invalidate any active user-role sessions so a removed/changed password
    // takes effect immediately.
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++)
        if (state.sessions[i].active && state.sessions[i].role == AUTH_ROLE_USER)
            state.sessions[i].active = false;
    return true;
}

bool auth_mgr_user_password_set(void) { return state.user_password_set; }

const char* auth_mgr_get_username(void) { return state.username; }

bool auth_mgr_web_password_set(void) { return state.password_set; }

auth_role_t auth_mgr_login(const char* password, char* session_token) {
    if (!state.web_auth_enabled) return AUTH_ROLE_ADMIN;  // auth disabled → full access
    if (password == NULL || password[0] == '\0') return AUTH_ROLE_NONE;

    uint8_t input_hash[32];
    hash_password(password, input_hash);

    auth_role_t role = AUTH_ROLE_NONE;
    if (state.password_set && memcmp(input_hash, state.password_hash, 32) == 0)
        role = AUTH_ROLE_ADMIN;  // admin checked first; "admin" is reserved
    else if (state.user_password_set &&
             memcmp(input_hash, state.user_password_hash, 32) == 0)
        role = AUTH_ROLE_USER;
    if (role == AUTH_ROLE_NONE) return AUTH_ROLE_NONE;

    session_t* session = find_free_session(role);
    if (session == NULL) return AUTH_ROLE_NONE;

    session->active = true;
    session->role = role;
    generate_random_hex(session->token, AUTH_SESSION_TOKEN_LEN);
    session->created_at = monotonic_now();
    session->last_used = session->created_at;
    if (session_token != NULL) strcpy(session_token, session->token);

    ESP_LOGI(TAG, "login ok, role=%s", role == AUTH_ROLE_ADMIN ? "admin" : "user");
    return role;
}

auth_role_t auth_mgr_session_role(const char* token) {
    if (!state.web_auth_enabled) return AUTH_ROLE_ADMIN;
    session_t* session = find_session(token);
    if (session == NULL) return AUTH_ROLE_NONE;
    if (is_session_expired(session)) {
        session->active = false;
        return AUTH_ROLE_NONE;
    }
    session->last_used = monotonic_now();
    return session->role;
}

bool auth_mgr_validate_session(const char* token) {
    return auth_mgr_session_role(token) != AUTH_ROLE_NONE;
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

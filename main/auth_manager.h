/// @file auth_manager.h
/// @brief Web-UI session auth + API-key auth for the on-device HTTP server.
///
/// Ported from sslivins/arctic-controller. Stores a username + SHA-256
/// password hash and a random API key in NVS. Web auth uses random session
/// tokens carried in an HttpOnly cookie; API auth uses an X-API-Key header.
/// Both web auth and API auth are independently toggleable and default OFF so
/// existing installs are never locked out after an update.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Session token length (32 hex chars = 128 bits)
#define AUTH_SESSION_TOKEN_LEN 32
// API key length (32 hex chars)
#define AUTH_API_KEY_LEN 32
// Maximum concurrent sessions
#define AUTH_MAX_SESSIONS 4
// Session lifetime in seconds (7 days)
#define AUTH_SESSION_LIFETIME_SEC (7 * 24 * 60 * 60)
// Maximum username length
#define AUTH_MAX_USERNAME_LEN 32
// Maximum password length
#define AUTH_MAX_PASSWORD_LEN 64

/// Initialize the auth manager (loads settings from NVS). Safe to call once;
/// requires nvs_flash_init() to have already run.
void auth_mgr_init(void);

// ── Web authentication (username/password + session cookie) ────────────────

bool auth_mgr_web_auth_enabled(void);
void auth_mgr_set_web_auth_enabled(bool enabled);

/// Set credentials. Pass NULL/empty to keep the existing value. The password
/// is hashed (SHA-256) before storage. All sessions are invalidated on change.
bool auth_mgr_set_credentials(const char* username, const char* password);

const char* auth_mgr_get_username(void);

/// Validate username/password and, on success, create a session. The token is
/// written to @p session_token (buffer must be AUTH_SESSION_TOKEN_LEN+1).
bool auth_mgr_login(const char* username, const char* password, char* session_token);

bool auth_mgr_validate_session(const char* token);
void auth_mgr_logout(const char* token);
void auth_mgr_logout_all(void);

// ── API-key authentication ─────────────────────────────────────────────────

bool auth_mgr_api_auth_enabled(void);
void auth_mgr_set_api_auth_enabled(bool enabled);

/// Copy the current API key into @p buffer (must be AUTH_API_KEY_LEN+1).
bool auth_mgr_get_api_key(char* buffer);

/// Generate a fresh API key, persist it, and copy it into @p buffer.
bool auth_mgr_regenerate_api_key(char* buffer);

bool auth_mgr_validate_api_key(const char* key);

#ifdef __cplusplus
}
#endif

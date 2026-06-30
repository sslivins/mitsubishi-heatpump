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

/// Access role attached to a web session. The "user" role is a shared,
/// climate-only login; "admin" has full access. Roles only apply to web
/// sessions — an API key or web-auth-disabled device is always treated as
/// admin.
typedef enum {
    AUTH_ROLE_NONE = 0,
    AUTH_ROLE_USER = 1,
    AUTH_ROLE_ADMIN = 2,
} auth_role_t;

/// Initialize the auth manager (loads settings from NVS). Safe to call once;
/// requires nvs_flash_init() to have already run.
void auth_mgr_init(void);

// ── Web authentication (password + session cookie) ─────────────────────────

bool auth_mgr_web_auth_enabled(void);
void auth_mgr_set_web_auth_enabled(bool enabled);

/// Set the admin password (hashed with SHA-256). The admin username is fixed
/// to "admin" and cannot be changed. All sessions are invalidated on change.
bool auth_mgr_set_admin_password(const char* password);

/// Whether the admin password is set. Web auth must not be enabled while this
/// is false (there would be no way to log in).
bool auth_mgr_web_password_set(void);

/// Set (or, with NULL/empty, remove) the shared climate-only "user" password.
/// Existing user-role sessions are invalidated on change.
bool auth_mgr_set_user_password(const char* password);

/// Whether the climate-only "user" account is enabled (password set).
bool auth_mgr_user_password_set(void);

const char* auth_mgr_get_username(void);

/// Password-only login: the password is matched against the admin then the
/// user account. Returns the matched role (AUTH_ROLE_NONE on failure). On
/// success a session is created and its token written to @p session_token
/// (buffer must be AUTH_SESSION_TOKEN_LEN+1).
auth_role_t auth_mgr_login(const char* password, char* session_token);

bool auth_mgr_validate_session(const char* token);

/// Role for a valid, unexpired session (AUTH_ROLE_NONE if invalid). Returns
/// AUTH_ROLE_ADMIN when web auth is disabled.
auth_role_t auth_mgr_session_role(const char* token);

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

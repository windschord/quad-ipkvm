#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "auth_logic.h" // For AUTH_LOGIC_MAX_PASSWORD_LEN and auth_state_e

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_GUARD_COOKIE_NAME "__Host-session"

// --- Base64URL ---

int http_guard_base64url_decode(const char *src, size_t src_len, uint8_t *dst, size_t dst_len, size_t *out_len);
int http_guard_base64url_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len, size_t *out_len);

// --- Cookie ---

int http_guard_extract_session_cookie(const char *cookie_header, size_t header_len, uint8_t token_out[32]);

// --- Host & Origin ---

bool http_guard_host_ok(const char *host, size_t host_len, const char *const *allowed_names, size_t allowed_count);
bool http_guard_origin_ok(const char *origin, size_t origin_len, const char *host, size_t host_len);

// --- JSON Parsing ---

typedef struct {
    char password[AUTH_LOGIC_MAX_PASSWORD_LEN + 1];
    size_t password_len;
} http_guard_json_setup_t;

typedef struct {
    char old_password[AUTH_LOGIC_MAX_PASSWORD_LEN + 1];
    size_t old_password_len;
    char new_password[AUTH_LOGIC_MAX_PASSWORD_LEN + 1];
    size_t new_password_len;
} http_guard_json_change_t;

int http_guard_parse_json_password(const char *json, size_t json_len, http_guard_json_setup_t *out);
int http_guard_parse_json_change(const char *json, size_t json_len, http_guard_json_change_t *out);
void http_guard_clear_json_setup(http_guard_json_setup_t *st);
void http_guard_clear_json_change(http_guard_json_change_t *st);

// --- Auth Decision ---

typedef enum {
    HTTP_GUARD_METHOD_GET,
    HTTP_GUARD_METHOD_POST,
    HTTP_GUARD_METHOD_OTHER
} http_guard_method_e;

typedef enum {
    HTTP_GUARD_PATH_STATUS,
    HTTP_GUARD_PATH_SETUP,
    HTTP_GUARD_PATH_LOGIN,
    HTTP_GUARD_PATH_LOGOUT,
    HTTP_GUARD_PATH_WS_VIDEO,
    HTTP_GUARD_PATH_STATIC,
    HTTP_GUARD_PATH_OTHER
} http_guard_path_e;

typedef enum {
    HTTP_GUARD_ACTION_ALLOW,
    HTTP_GUARD_ACTION_401,
    HTTP_GUARD_ACTION_403_SETUP_REQUIRED,
    HTTP_GUARD_ACTION_403_ORIGIN,
    HTTP_GUARD_ACTION_403_HOST,
    HTTP_GUARD_ACTION_405,
    HTTP_GUARD_ACTION_409_ALREADY_SETUP,
    HTTP_GUARD_ACTION_415
} http_guard_action_e;

http_guard_action_e http_guard_decide(auth_state_e state, http_guard_method_e method, http_guard_path_e path, bool has_valid_session, bool origin_ok, bool is_json, bool host_ok);

// --- Response ---

int http_guard_format_error(const char *msg, char *out, size_t out_len);
int http_guard_format_status(auth_state_e state, bool authenticated, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

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

/**
 * 映像専用の httpd(別ポート)用の Origin 検査 (docs/VIDEO_TRANSPORT.md 5 章)。
 * ブラウザの Origin は「ページの Origin」(メインのポート) で、映像サーバーの Host とはポートだけが違う。
 * 許可する条件 (すべて):
 *  - Origin が "https://" で始まる (大小文字を区別しない)。
 *  - Origin のホスト名が、Host のホスト名 (":ポート" を除く) と一致する (大小文字を区別しない)。
 *  - Origin のポートが page_port と一致する。Origin にポートが無ければ 443 と見なす (page_port == 443 のとき ":443" があってもよい)。
 *    Host のポート (映像サーバーのポート) は、形式だけを検査し、値は問わない。
 * 拒否: "null"、"@"、"["(IPv6)、末尾・先頭・連続の "."、ポートが空・数字でない・範囲外 (1〜65535)・先頭ゼロ、
 *       スキームが違う、Origin に "/" などが続く、host が空、page_port == 0 など。
 */
bool http_guard_origin_ok_for_port(const char *origin, size_t origin_len, const char *host, size_t host_len, uint16_t page_port);

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
    HTTP_GUARD_PATH_WS_INPUT,
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
/**
 * 状態の応答 {"state":...,"authenticated":...[,"video_port":N]} を out に書く。成功で 0、バッファ不足などで -1。
 * video_port: 映像専用の HTTPS サーバーのポート(docs/VIDEO_TRANSPORT.md 5 章)。0 は「映像専用のサーバーなし
 * (映像はメインのポートの /video)」で、video_port のキーを出さない(後方互換)。それ以外は整数で出す。
 */
int http_guard_format_status(auth_state_e state, bool authenticated, uint16_t video_port, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

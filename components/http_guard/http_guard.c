#include "http_guard.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// --- Base64URL ---

static const char base64url_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int base64url_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

int http_guard_base64url_decode(const char *src, size_t src_len, uint8_t *dst, size_t dst_len, size_t *out_len) {
    if (!src || !dst || !out_len) return -1;
    if (src_len % 4 == 1) return -1; // Invalid length for unpadded base64url

    size_t dlen = 0;
    uint32_t val = 0;
    int valb = -8;

    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') return -1;

        int d = base64url_decode_char(c);
        if (d == -1) return -1;

        val = (val << 6) | (uint32_t)d;
        valb += 6;
        if (valb >= 0) {
            if (dlen >= dst_len) return -1;
            dst[dlen++] = (val >> valb) & 0xFF;
            valb -= 8;
        }
    }

    if (valb > -8) {
        int leftover_bits = valb + 8;
        uint32_t mask = (1U << leftover_bits) - 1;
        if ((val & mask) != 0) {
            return -1; // Non-canonical: leftover bits are not zero
        }
    }

    *out_len = dlen;
    return 0;
}

int http_guard_base64url_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len, size_t *out_len) {
    if (!src || !dst || !out_len) return -1;

    size_t elen = 0;
    uint32_t val = 0;
    int valb = -6;

    for (size_t i = 0; i < src_len; i++) {
        val = (val << 8) | src[i];
        valb += 8;
        while (valb >= 0) {
            if (elen >= dst_len) return -1;
            dst[elen++] = base64url_chars[(val >> valb) & 0x3F];
            valb -= 6;
        }
    }
    if (valb > -6) {
        if (elen >= dst_len) return -1;
        dst[elen++] = base64url_chars[((val << 8) >> (valb + 8)) & 0x3F];
    }

    if (elen < dst_len) {
        dst[elen] = '\0';
    }
    *out_len = elen;
    return 0;
}

// --- Cookie ---

static void skip_whitespace(const char **p, const char *end) {
    while (*p < end && (**p == ' ' || **p == '\t')) {
        (*p)++;
    }
}

int http_guard_extract_session_cookie(const char *cookie_header, size_t header_len, uint8_t token_out[32]) {
    if (!cookie_header || header_len == 0 || !token_out) return -1;

    const char *p = cookie_header;
    const char *end = cookie_header + header_len;

    bool found = false;
    const char *session_val_start = NULL;
    size_t session_val_len = 0;

    size_t cookie_name_len = strlen(HTTP_GUARD_COOKIE_NAME);

    while (p < end) {
        skip_whitespace(&p, end);
        if (p >= end) break;

        const char *name_start = p;
        while (p < end && *p != '=' && *p != ';') {
            p++;
        }
        size_t name_len = p - name_start;

        while (name_len > 0 && (name_start[name_len - 1] == ' ' || name_start[name_len - 1] == '\t')) {
            name_len--;
        }

        const char *val_start = NULL;
        size_t val_len = 0;

        if (p < end && *p == '=') {
            p++; // Skip '='
            skip_whitespace(&p, end);
            val_start = p;
            while (p < end && *p != ';') {
                p++;
            }
            val_len = p - val_start;

            while (val_len > 0 && (val_start[val_len - 1] == ' ' || val_start[val_len - 1] == '\t')) {
                val_len--;
            }
        } else {
            // No '=' found, just skip to next ';'
            while (p < end && *p != ';') {
                p++;
            }
        }

        if (name_len == cookie_name_len && memcmp(name_start, HTTP_GUARD_COOKIE_NAME, cookie_name_len) == 0) {
            if (found) {
                return -1; // Duplicated session cookie
            }
            if (!val_start || val_len == 0) {
                return -1; // Empty value
            }
            found = true;
            session_val_start = val_start;
            session_val_len = val_len;
        }

        if (p < end && *p == ';') {
            p++; // Skip ';'
        }
    }

    if (!found) return -1;

    // Decode base64url
    size_t dec_len = 0;
    if (http_guard_base64url_decode(session_val_start, session_val_len, token_out, 32, &dec_len) != 0) {
        return -1;
    }
    if (dec_len != 32) {
        return -1; // Token must be exactly 32 bytes
    }

    return 0;
}

// --- Host & Origin ---

static bool is_valid_ipv4_octet(const char *str, size_t len) {
    if (len == 0 || len > 3) return false;
    if (len > 1 && str[0] == '0') return false; // Leading zero not allowed
    int val = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] < '0' || str[i] > '9') return false;
        val = val * 10 + (str[i] - '0');
    }
    return val <= 255;
}

static bool is_valid_ipv4(const char *host, size_t host_len) {
    int octet_count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= host_len; i++) {
        if (i == host_len || host[i] == '.') {
            if (!is_valid_ipv4_octet(host + start, i - start)) return false;
            octet_count++;
            start = i + 1;
        }
    }
    return octet_count == 4;
}

bool http_guard_host_ok(const char *host, size_t host_len, const char *const *allowed_names, size_t allowed_count) {
    if (!host || host_len == 0 || host_len > 255) return false;

    // Parse optional port
    size_t colon_idx = 0;
    bool has_colon = false;
    for (size_t i = 0; i < host_len; i++) {
        if (host[i] == ':') {
            has_colon = true;
            colon_idx = i;
            break;
        }
    }

    size_t name_len = host_len;
    if (has_colon) {
        name_len = colon_idx;
        // Validate port
        size_t port_len = host_len - colon_idx - 1;
        if (port_len == 0 || port_len > 5) return false;
        int port = 0;
        for (size_t i = colon_idx + 1; i < host_len; i++) {
            if (host[i] < '0' || host[i] > '9') return false;
            port = port * 10 + (host[i] - '0');
        }
        if (port < 1 || port > 65535) return false;
        if (port_len > 1 && host[colon_idx + 1] == '0') return false;
        if (port_len > 1 && host[colon_idx + 1] == '0') return false;
    }

    if (name_len == 0) return false;

    // Check if it's a valid IPv4
    if (is_valid_ipv4(host, name_len)) return true;

    // Reject IPv6 literals and other obviously invalid chars early
    for(size_t i=0; i<name_len; i++) {
        if (host[i] == '[' || host[i] == ']' || host[i] == '@' || host[i] == ' ') return false;
    }

    // Check allowed_names (case-insensitive)
    for (size_t i = 0; i < allowed_count; i++) {
        const char *allowed = allowed_names[i];
        size_t alen = strlen(allowed);
        if (alen == name_len) {
            bool match = true;
            for (size_t j = 0; j < name_len; j++) {
                if (tolower((unsigned char)host[j]) != tolower((unsigned char)allowed[j])) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
    }

    return false;
}

bool http_guard_origin_ok(const char *origin, size_t origin_len, const char *host, size_t host_len) {
    if (!origin || origin_len == 0 || !host || host_len == 0) return false;

    if (origin_len == 4 && memcmp(origin, "null", 4) == 0) return false;

    const char *scheme = "https://";
    size_t scheme_len = 8;
    if (origin_len <= scheme_len) return false;

    for (size_t i = 0; i < scheme_len; i++) {
        if (tolower((unsigned char)origin[i]) != scheme[i]) return false;
    }

    const char *origin_host = origin + scheme_len;
    size_t origin_host_len = origin_len - scheme_len;

    for(size_t i=0; i<origin_host_len; i++) {
        if (origin_host[i] == '@') return false;
    }

    if (origin_host_len > 0 && origin_host[0] == '[') return false;

    if (origin_host_len > 0 && origin_host[origin_host_len - 1] == '.') return false;

    if (origin_host_len != host_len) return false;

    for (size_t i = 0; i < host_len; i++) {
        if (tolower((unsigned char)origin_host[i]) != tolower((unsigned char)host[i])) {
            return false;
        }
    }

    return true;
}

// --- JSON Parsing & UTF-8 ---

static bool is_valid_utf8(const char *str, size_t len) {
    const uint8_t *s = (const uint8_t *)str;
    size_t i = 0;
    while (i < len) {
        if (s[i] <= 0x7F) {
            i++;
        } else if (s[i] >= 0xC2 && s[i] <= 0xDF) {
            if (i + 1 >= len || (s[i+1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if (s[i] >= 0xE0 && s[i] <= 0xEF) {
            if (i + 2 >= len || (s[i+1] & 0xC0) != 0x80 || (s[i+2] & 0xC0) != 0x80) return false;
            if (s[i] == 0xE0 && s[i+1] < 0xA0) return false; // Overlong
            if (s[i] == 0xED && s[i+1] >= 0xA0) return false; // Surrogate
            i += 3;
        } else if (s[i] >= 0xF0 && s[i] <= 0xF4) {
            if (i + 3 >= len || (s[i+1] & 0xC0) != 0x80 || (s[i+2] & 0xC0) != 0x80 || (s[i+3] & 0xC0) != 0x80) return false;
            if (s[i] == 0xF0 && s[i+1] < 0x90) return false; // Overlong
            if (s[i] == 0xF4 && s[i+1] >= 0x90) return false; // > U+10FFFF
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

typedef struct {
    const char *p;
    const char *end;
} json_parser_t;

static void json_skip_whitespace(json_parser_t *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) {
        j->p++;
    }
}

static bool json_match_char(json_parser_t *j, char c) {
    json_skip_whitespace(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return true;
    }
    return false;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int json_parse_string(json_parser_t *j, char *out_buf, size_t max_len, size_t *out_len) {
    json_skip_whitespace(j);
    if (j->p >= j->end || *j->p != '"') return -1;
    j->p++; // skip '"'

    size_t len = 0;
    while (j->p < j->end) {
        char c = *j->p;
        if (c == '"') {
            j->p++; // skip ending '"'
            if (len > max_len) return -1;
            out_buf[len] = '\0';
            *out_len = len;
            // Validate UTF-8 of the resulting raw string (as it might contain invalid bytes)
            if (!is_valid_utf8(out_buf, len)) return -1;
            return 0;
        } else if (c == '\\') {
            j->p++;
            if (j->p >= j->end) return -1;
            char ec = *j->p++;
            char unescaped = 0;
            switch (ec) {
                case '"': unescaped = '"'; break;
                case '\\': unescaped = '\\'; break;
                case '/': unescaped = '/'; break;
                case 'b': unescaped = '\b'; break;
                case 'f': unescaped = '\f'; break;
                case 'n': unescaped = '\n'; break;
                case 'r': unescaped = '\r'; break;
                case 't': unescaped = '\t'; break;
                case 'u': {
                    if (j->p + 4 > j->end) return -1;
                    int h1 = hex_val(j->p[0]);
                    int h2 = hex_val(j->p[1]);
                    int h3 = hex_val(j->p[2]);
                    int h4 = hex_val(j->p[3]);
                    if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) return -1;
                    uint32_t cp = (h1 << 12) | (h2 << 8) | (h3 << 4) | h4;
                    j->p += 4;

                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate, expect low surrogate
                        if (j->p + 6 > j->end || j->p[0] != '\\' || j->p[1] != 'u') return -1;
                        int l1 = hex_val(j->p[2]);
                        int l2 = hex_val(j->p[3]);
                        int l3 = hex_val(j->p[4]);
                        int l4 = hex_val(j->p[5]);
                        if (l1 < 0 || l2 < 0 || l3 < 0 || l4 < 0) return -1;
                        uint32_t low_cp = (l1 << 12) | (l2 << 8) | (l3 << 4) | l4;
                        if (low_cp < 0xDC00 || low_cp > 0xDFFF) return -1; // Must be low surrogate
                        j->p += 6;
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (low_cp - 0xDC00));
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return -1; // Isolated low surrogate
                    } else if (cp == 0) {
                        return -1; // Reject \u0000
                    }

                    if (cp <= 0x7F) {
                        if (len >= max_len) return -1;
                        out_buf[len++] = cp;
                    } else if (cp <= 0x7FF) {
                        if (len + 1 >= max_len) return -1;
                        out_buf[len++] = 0xC0 | (cp >> 6);
                        out_buf[len++] = 0x80 | (cp & 0x3F);
                    } else if (cp <= 0xFFFF) {
                        if (len + 2 >= max_len) return -1;
                        out_buf[len++] = 0xE0 | (cp >> 12);
                        out_buf[len++] = 0x80 | ((cp >> 6) & 0x3F);
                        out_buf[len++] = 0x80 | (cp & 0x3F);
                    } else {
                        if (len + 3 >= max_len) return -1;
                        out_buf[len++] = 0xF0 | (cp >> 18);
                        out_buf[len++] = 0x80 | ((cp >> 12) & 0x3F);
                        out_buf[len++] = 0x80 | ((cp >> 6) & 0x3F);
                        out_buf[len++] = 0x80 | (cp & 0x3F);
                    }
                    continue;
                }
                default:
                    return -1; // Unknown escape
            }
            if (len >= max_len) return -1;
            out_buf[len++] = unescaped;
        } else if ((unsigned char)c < 0x20) {
            return -1; // Control characters in string must be escaped
        } else {
            if (len >= max_len) return -1;
            out_buf[len++] = c;
            j->p++;
        }
    }
    return -1; // Missing closing quote
}

static bool string_eq(const char *buf, size_t len, const char *lit) {
    size_t lit_len = strlen(lit);
    if (len != lit_len) return false;
    return memcmp(buf, lit, len) == 0;
}

int http_guard_parse_json_password(const char *json, size_t json_len, http_guard_json_setup_t *out) {
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));

    json_parser_t j = {json, json + json_len};

    if (!json_match_char(&j, '{')) return -1;

    bool found_password = false;

    json_skip_whitespace(&j);
    if (json_match_char(&j, '}')) return -1;

    while (j.p < j.end) {
        char key_buf[32];
        size_t key_len = 0;
        if (json_parse_string(&j, key_buf, sizeof(key_buf)-1, &key_len) != 0) return -1;

        if (!json_match_char(&j, ':')) return -1;

        if (string_eq(key_buf, key_len, "password")) {
            if (found_password) return -1; // Duplicate key
            if (json_parse_string(&j, out->password, AUTH_LOGIC_MAX_PASSWORD_LEN, &out->password_len) != 0) return -1;
            found_password = true;
        } else {
            return -1; // Unknown key
        }

        if (json_match_char(&j, ',')) {
            continue;
        } else if (json_match_char(&j, '}')) {
            break;
        } else {
            return -1; // Expected ',' or '}'
        }
    }

    json_skip_whitespace(&j);
    if (j.p != j.end) return -1; // Trailing garbage

    if (!found_password) return -1; // Missing required key

    return 0;
}

int http_guard_parse_json_change(const char *json, size_t json_len, http_guard_json_change_t *out) {
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));

    json_parser_t j = {json, json + json_len};

    if (!json_match_char(&j, '{')) return -1;

    bool found_old = false;
    bool found_new = false;

    json_skip_whitespace(&j);
    if (json_match_char(&j, '}')) return -1;

    while (j.p < j.end) {
        char key_buf[32];
        size_t key_len = 0;
        if (json_parse_string(&j, key_buf, sizeof(key_buf)-1, &key_len) != 0) return -1;

        if (!json_match_char(&j, ':')) return -1;

        if (string_eq(key_buf, key_len, "old_password")) {
            if (found_old) return -1; // Duplicate key
            if (json_parse_string(&j, out->old_password, AUTH_LOGIC_MAX_PASSWORD_LEN, &out->old_password_len) != 0) return -1;
            found_old = true;
        } else if (string_eq(key_buf, key_len, "new_password")) {
            if (found_new) return -1; // Duplicate key
            if (json_parse_string(&j, out->new_password, AUTH_LOGIC_MAX_PASSWORD_LEN, &out->new_password_len) != 0) return -1;
            found_new = true;
        } else {
            return -1; // Unknown key
        }

        if (json_match_char(&j, ',')) {
            continue;
        } else if (json_match_char(&j, '}')) {
            break;
        } else {
            return -1; // Expected ',' or '}'
        }
    }

    json_skip_whitespace(&j);
    if (j.p != j.end) return -1; // Trailing garbage

    if (!found_old || !found_new) return -1; // Missing required keys

    return 0;
}

void http_guard_clear_json_setup(http_guard_json_setup_t *st) {
    if (st) {
        memset(st->password, 0, sizeof(st->password));
        st->password_len = 0;
    }
}

void http_guard_clear_json_change(http_guard_json_change_t *st) {
    if (st) {
        memset(st->old_password, 0, sizeof(st->old_password));
        memset(st->new_password, 0, sizeof(st->new_password));
        st->old_password_len = 0;
        st->new_password_len = 0;
    }
}

// --- Auth Decision ---

http_guard_action_e http_guard_decide(auth_state_e state, http_guard_method_e method, http_guard_path_e path, bool has_valid_session, bool origin_ok, bool is_json, bool host_ok) {
    if (!host_ok) return HTTP_GUARD_ACTION_403_HOST;

    if (state == AUTH_STATE_NEEDS_SETUP) {
        if (path == HTTP_GUARD_PATH_STATUS) {
            return (method == HTTP_GUARD_METHOD_GET) ? HTTP_GUARD_ACTION_ALLOW : HTTP_GUARD_ACTION_405;
        }
        if (path == HTTP_GUARD_PATH_SETUP) {
            if (method != HTTP_GUARD_METHOD_POST) return HTTP_GUARD_ACTION_405;
            if (!origin_ok) return HTTP_GUARD_ACTION_403_ORIGIN;
            if (!is_json) return HTTP_GUARD_ACTION_415;
            return HTTP_GUARD_ACTION_ALLOW;
        }
        return HTTP_GUARD_ACTION_403_SETUP_REQUIRED;
    } else {
        // state == READY
        if (path == HTTP_GUARD_PATH_STATUS) {
            return (method == HTTP_GUARD_METHOD_GET) ? HTTP_GUARD_ACTION_ALLOW : HTTP_GUARD_ACTION_405;
        }

        if (path == HTTP_GUARD_PATH_SETUP) {
            return HTTP_GUARD_ACTION_409_ALREADY_SETUP;
        }

        if (path == HTTP_GUARD_PATH_LOGIN) {
            if (method != HTTP_GUARD_METHOD_POST) return HTTP_GUARD_ACTION_405;
            if (!origin_ok) return HTTP_GUARD_ACTION_403_ORIGIN;
            if (!is_json) return HTTP_GUARD_ACTION_415;
            return HTTP_GUARD_ACTION_ALLOW;
        }

        // Everything else requires a valid session
        if (!has_valid_session) {
            return HTTP_GUARD_ACTION_401;
        }

        if (path == HTTP_GUARD_PATH_LOGOUT) {
            if (method != HTTP_GUARD_METHOD_POST) return HTTP_GUARD_ACTION_405;
            if (!origin_ok) return HTTP_GUARD_ACTION_403_ORIGIN;
            return HTTP_GUARD_ACTION_ALLOW;
        }

        if (path == HTTP_GUARD_PATH_WS_VIDEO) {
            if (method != HTTP_GUARD_METHOD_GET) return HTTP_GUARD_ACTION_405;
            if (!origin_ok) return HTTP_GUARD_ACTION_403_ORIGIN;
            return HTTP_GUARD_ACTION_ALLOW;
        }

        // Other protected resources
        if (method == HTTP_GUARD_METHOD_POST) {
            if (!origin_ok) return HTTP_GUARD_ACTION_403_ORIGIN;
            return HTTP_GUARD_ACTION_ALLOW;
        } else if (method == HTTP_GUARD_METHOD_GET) {
            return HTTP_GUARD_ACTION_ALLOW;
        }

        return HTTP_GUARD_ACTION_405; // OTHER methods
    }
}

// --- Response ---

int http_guard_format_error(const char *msg, char *out, size_t out_len) {
    if (!msg || !out || out_len == 0) return -1;
    int ret = snprintf(out, out_len, "{\"error\":\"%s\"}", msg);
    if (ret < 0 || (size_t)ret >= out_len) return -1;
    return 0;
}

int http_guard_format_status(auth_state_e state, bool authenticated, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    const char *state_str = (state == AUTH_STATE_NEEDS_SETUP) ? "needs_setup" : "ready";
    const char *auth_str = authenticated ? "true" : "false";
    int ret = snprintf(out, out_len, "{\"state\":\"%s\",\"authenticated\":%s}", state_str, auth_str);
    if (ret < 0 || (size_t)ret >= out_len) return -1;
    return 0;
}

#include "web_headers.h"
#include "web_assets.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool is_host_name_char(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '.' || c == '-';
}

static bool is_ipv6_literal_char(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F') ||
           c == ':' || c == '.';
}

static bool valid_port_suffix(const char *p) {
    if (*p != ':') return false;
    p++;
    if (*p == '\0' || (*p == '0' && p[1] != '\0')) return false;
    unsigned int port = 0;
    size_t digits = 0;
    for (; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') return false;
        port = port * 10u + (unsigned int)(*p - '0');
        digits++;
        if (digits > 5) return false;
    }
    return port >= 1u && port <= 65535u;
}

static bool extract_csp_hostname(const char *host, size_t *out_len) {
    if (!host || host[0] == '\0') return false;

    size_t name_len = 0;
    const char *rest = NULL;
    if (host[0] == '[') {
        const char *close = strchr(host + 1, ']');
        if (!close || close == host + 1) return false;
        for (const char *p = host + 1; p < close; p++) {
            if (!is_ipv6_literal_char(*p)) return false;
        }
        name_len = (size_t)(close - host) + 1u;
        rest = close + 1;
    } else {
        const char *colon = strchr(host, ':');
        const char *end = colon ? colon : host + strlen(host);
        if (end == host) return false;
        for (const char *p = host; p < end; p++) {
            if (!is_host_name_char(*p)) return false;
        }
        name_len = (size_t)(end - host);
        rest = end;
    }

    if (name_len > 255u) return false;
    if (*rest != '\0' && !valid_port_suffix(rest)) return false;

    *out_len = name_len;
    return true;
}

int web_headers_format_html_csp(const char *host, unsigned int video_port, char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    if (video_port == 0) {
        int n = snprintf(out, out_size, "%s", web_assets_csp("text/html"));
        return (n >= 0 && (size_t)n < out_size) ? 0 : -1;
    }
    if (video_port > 65535u) return -1;

    size_t name_len = 0;
    if (!extract_csp_hostname(host, &name_len)) {
        return -1;
    }

    int n = snprintf(
        out, out_size,
        "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' blob:; "
        "connect-src 'self' wss://%.*s:%u; base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
        (int)name_len, host, video_port);
    return (n >= 0 && (size_t)n < out_size) ? 0 : -1;
}

size_t web_headers_get_api(web_header_t *headers, size_t max_headers) {
    if (!headers || max_headers < 4) return 0;

    headers[0].name = "Content-Security-Policy";
    headers[0].value = "default-src 'none'; frame-ancestors 'none'";

    headers[1].name = "X-Content-Type-Options";
    headers[1].value = "nosniff";

    headers[2].name = "Cache-Control";
    headers[2].value = "no-store";

    headers[3].name = "Referrer-Policy";
    headers[3].value = "no-referrer";

    return 4;
}

const char *web_headers_allow_for_uri(const char *uri) {
    if (!uri) return NULL;

    size_t len = 0;
    const char *q = strchr(uri, '?');
    if (q) {
        len = (size_t)(q - uri);
    } else {
        len = strlen(uri);
    }

    if (len == 1 && strncmp(uri, "/", 1) == 0) return "GET";
    if (len == 7 && strncmp(uri, "/app.js", 7) == 0) return "GET";
    if (len == 8 && strncmp(uri, "/app.css", 8) == 0) return "GET";
    if (len == 19 && strncmp(uri, "/api/v1/auth/status", 19) == 0) return "GET";

    if (len == 18 && strncmp(uri, "/api/v1/auth/setup", 18) == 0) return "POST";
    if (len == 18 && strncmp(uri, "/api/v1/auth/login", 18) == 0) return "POST";
    if (len == 19 && strncmp(uri, "/api/v1/auth/logout", 19) == 0) return "POST";

    return NULL;
}

size_t web_headers_get_static(const char *content_type, web_header_t *headers, size_t max_headers) {
    if (!headers || max_headers < 4) return 0;

    headers[0].name = "Content-Security-Policy";
    headers[0].value = web_assets_csp(content_type);

    headers[1].name = "X-Content-Type-Options";
    headers[1].value = "nosniff";

    headers[2].name = "Cache-Control";
    headers[2].value = "no-store";

    headers[3].name = "Referrer-Policy";
    headers[3].value = "no-referrer";

    return 4;
}

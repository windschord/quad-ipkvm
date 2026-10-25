#include "web_headers.h"
#include "web_assets.h"
#include <string.h>

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

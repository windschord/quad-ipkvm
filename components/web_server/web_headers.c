#include "web_headers.h"
#include "web_assets.h"

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

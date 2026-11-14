#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    const char *value;
} web_header_t;

/**
 * @brief Get security headers for API responses and errors.
 *
 * @param headers Array to populate with headers.
 * @param max_headers Maximum number of headers the array can hold.
 * @return Number of headers populated.
 */
size_t web_headers_get_api(web_header_t *headers, size_t max_headers);

/**
 * @brief Get security headers for static asset responses.
 *
 * @param content_type The content type of the static asset.
 * @param headers Array to populate with headers.
 * @param max_headers Maximum number of headers the array can hold.
 * @return Number of headers populated.
 */
size_t web_headers_get_static(const char *content_type, web_header_t *headers, size_t max_headers);

/**
 * @brief Format Content-Security-Policy for the main HTML page, adding
 *        wss://<hostname>:<video_port> to connect-src when video_port != 0.
 *
 * Strips any optional :port suffix from @p host cleanly (preserving IPv6 [addr]).
 * When @p video_port == 0, writes the default HTML CSP from web_assets_csp().
 *
 * @param host Validated Host header value (required when video_port != 0).
 * @param video_port Dedicated video server port, or 0 if disabled.
 * @param out Destination buffer for the formatted CSP string.
 * @param out_size Size of @p out in bytes.
 * @return 0 on success, -1 on invalid input, disallowed host characters, or truncation.
 */
int web_headers_format_html_csp(const char *host, unsigned int video_port, char *out, size_t out_size);

/**
 * @brief Get allowed methods for a given URI.
 *
 * @param uri The request URI string.
 * @return String representing allowed methods (e.g., "GET", "POST", "GET, POST"), or NULL if URI is not registered.
 */
const char *web_headers_allow_for_uri(const char *uri);

#ifdef __cplusplus
}
#endif

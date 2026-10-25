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
 * @brief Get allowed methods for a given URI.
 *
 * @param uri The request URI string.
 * @return String representing allowed methods (e.g., "GET", "POST", "GET, POST"), or NULL if URI is not registered.
 */
const char *web_headers_allow_for_uri(const char *uri);

#ifdef __cplusplus
}
#endif

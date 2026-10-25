#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *path;
    const char *content_type;
    const unsigned char *data;
    size_t len;
} web_asset_t;

/**
 * @brief Find an asset by exact URI match.
 *
 * Removes query strings for matching. Rejects paths with fragment (#),
 * traversal (..), escaped traversal (%), backslash (\), double slash (//),
 * trailing slash (unless the path is exactly "/"), NUL byte, mismatched case,
 * or excessively long URIs.
 *
 * @param table Array of available assets.
 * @param count Number of items in the table.
 * @param uri The requested URI string.
 * @return Pointer to the matched web_asset_t, or NULL if not found or rejected.
 */
const web_asset_t *web_assets_find(const web_asset_t *table, size_t count, const char *uri);

/**
 * @brief Get the Content-Security-Policy (CSP) string based on content type.
 *
 * Provides a strict CSP for HTML content, and a restrictive CSP for other types.
 *
 * @param content_type The content type of the asset (e.g., "text/html").
 * @return The CSP string to use in the HTTP response header.
 */
const char *web_assets_csp(const char *content_type);

#ifdef __cplusplus
}
#endif

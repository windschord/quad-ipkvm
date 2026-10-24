#include "web_assets.h"
#include <string.h>

const web_asset_t *web_assets_find(const web_asset_t *table, size_t count, const char *uri) {
    if (!table || !uri || count == 0) return NULL;

    // Check for excessive length (arbitrary safe limit for paths)
    if (strlen(uri) > 256) return NULL;

    // Check for invalid characters and sequences
    if (strchr(uri, '#') != NULL) return NULL;
    if (strstr(uri, "..") != NULL) return NULL;
    if (strchr(uri, '%') != NULL) return NULL;
    if (strchr(uri, '\\') != NULL) return NULL;
    if (strstr(uri, "//") != NULL) return NULL;

    // Find the end of the path (before any query string)
    const char *query_pos = strchr(uri, '?');
    size_t path_len = query_pos ? (size_t)(query_pos - uri) : strlen(uri);

    // Reject empty path
    if (path_len == 0) return NULL;

    // Reject trailing slash unless exactly "/"
    if (path_len > 1 && uri[path_len - 1] == '/') return NULL;

    // Search table for exact match
    for (size_t i = 0; i < count; i++) {
        const char *asset_path = table[i].path;
        if (asset_path && strlen(asset_path) == path_len && strncmp(asset_path, uri, path_len) == 0) {
            return &table[i];
        }
    }

    return NULL;
}

const char *web_assets_csp(const char *content_type) {
    if (!content_type) return "default-src 'none'; frame-ancestors 'none'";

    if (strcmp(content_type, "text/html") == 0) {
        return "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' blob:; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
    } else {
        return "default-src 'none'; frame-ancestors 'none'";
    }
}

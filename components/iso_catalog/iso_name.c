#include "iso_catalog.h"

#include <string.h>

bool iso_name_valid(const char *s, size_t len) {
    if (!s || len < 1 || len > ISO_NAME_MAX_BYTES) return false;
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] == ' ' || p[len - 1] == ' ') return false;

    size_t i = 0;
    while (i < len) {
        uint8_t c = p[i];
        if (c < 0x80) {
            /* C0 制御文字 (U+0000〜001F)、DEL、'/'、'\\' */
            if (c < 0x20 || c == 0x7F || c == '/' || c == '\\') return false;
            i++;
            continue;
        }
        size_t n;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { /* 0xC0, 0xC1 は過長な表現 */
            n = 2;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0u) == 0xE0u) {
            n = 3;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) { /* 0xF5 以上は U+10FFFF を超える */
            n = 4;
            cp = c & 0x07u;
        } else {
            return false; /* 継続バイトが先頭に来た、0xF8 以上 */
        }
        if (i + n > len) return false;
        for (size_t k = 1; k < n; k++) {
            if ((p[i + k] & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (p[i + k] & 0x3Fu);
        }
        if (n == 2 && cp <= 0x9F) return false;                       /* C1 制御文字 (U+0080〜009F) */
        if (n == 3 && cp < 0x800) return false;                       /* 過長な表現 */
        if (n == 3 && cp >= 0xD800 && cp <= 0xDFFF) return false;     /* サロゲート */
        if (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return false;  /* 過長な表現、範囲外 */
        i += n;
    }
    return true;
}

bool iso_name_equal(const char *a, size_t alen, const char *b, size_t blen) {
    if (alen != blen) return false;
    if (alen == 0) return true;
    return memcmp(a, b, alen) == 0;
}

bool iso_id_valid(const char *s) {
    if (!s) return false;
    for (size_t i = 0; i < ISO_ID_LEN; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false; /* NUL もここで落ちる */
    }
    return s[ISO_ID_LEN] == '\0';
}

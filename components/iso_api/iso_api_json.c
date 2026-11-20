#include "iso_api_priv.h"

#include <string.h>

/* JSON のボディの厳密な解析(docs/ISO_MOUNT_DESIGN.md 7、docs/WEB_API.md 8)。
 *   - 値は、オブジェクト 1 つだけ。キーの重複、定義していないキー、配列・入れ子、`null`、末尾の余計な文字(空白以外)は不正。
 *   - 文字列は、`\u` エスケープを復号してから検査する(サロゲートペアは 4 バイトの UTF-8 にする。対になっていないサロゲートは不正)。
 *     生の C0 制御文字(U+0000〜001F)は、JSON として不正。生の 0x80 以上のバイトは、そのままコピーする(UTF-8 としての検査は iso_name_valid)。
 *   - 数は、10 進の整数だけ(符号、小数、指数、先頭ゼロは不正)。 */

typedef struct {
    const char *s;
    size_t len;
    size_t i;
} cur_t;

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static void skip_ws(cur_t *c) {
    while (c->i < c->len && is_ws(c->s[c->i])) c->i++;
}

static bool eat(cur_t *c, char ch) {
    if (c->i < c->len && c->s[c->i] == ch) {
        c->i++;
        return true;
    }
    return false;
}

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* `\u` の後ろの 4 桁の 16 進(大文字小文字を受け付ける) */
static bool read_u4(cur_t *c, uint32_t *out) {
    if (c->len - c->i < 4) return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexval(c->s[c->i + (size_t)k]);
        if (h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    c->i += 4;
    *out = v;
    return true;
}

/* cp(サロゲートでない。U+10FFFF 以下)を UTF-8 にする。書いたバイト数を返す。 */
static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* c->i が '"' を指すところから、文字列を復号して out(cap バイトまで。NUL は付けない)に入れる。 */
static bool parse_string(cur_t *c, char *out, size_t cap, size_t *out_len) {
    if (!eat(c, '"')) return false;
    size_t n = 0;
    for (;;) {
        if (c->i >= c->len) return false;
        unsigned char ch = (unsigned char)c->s[c->i++];
        if (ch == '"') break;
        if (ch < 0x20u) return false;
        if (ch != '\\') {
            if (n >= cap) return false;
            out[n++] = (char)ch;
            continue;
        }
        if (c->i >= c->len) return false;
        char e = c->s[c->i++];
        uint32_t cp = 0;
        switch (e) {
        case '"': cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;
        case 'b': cp = 0x08; break;
        case 'f': cp = 0x0C; break;
        case 'n': cp = 0x0A; break;
        case 'r': cp = 0x0D; break;
        case 't': cp = 0x09; break;
        case 'u':
            if (!read_u4(c, &cp)) return false;
            if (cp >= 0xDC00u && cp <= 0xDFFFu) return false; /* 下位サロゲートだけ */
            if (cp >= 0xD800u && cp <= 0xDBFFu) {              /* 上位サロゲート: 続く `\u` の下位サロゲートと対になる */
                if (c->len - c->i < 2 || c->s[c->i] != '\\' || c->s[c->i + 1] != 'u') return false;
                c->i += 2;
                uint32_t lo = 0;
                if (!read_u4(c, &lo)) return false;
                if (lo < 0xDC00u || lo > 0xDFFFu) return false;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            }
            break;
        default: return false;
        }
        char tmp[4];
        size_t k = utf8_encode(cp, tmp);
        if (k > cap - n) return false;
        memcpy(out + n, tmp, k);
        n += k;
    }
    *out_len = n;
    return true;
}

int iso_api_parse_u64(const char *s, size_t len, uint64_t *out) {
    if (!s || !out || len == 0 || len > 20) return -1;
    if (len > 1 && s[0] == '0') return -1;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10u) return -1;
        v = v * 10u + d;
    }
    *out = v;
    return 0;
}

static bool parse_uint(cur_t *c, uint64_t *out) {
    size_t start = c->i;
    while (c->i < c->len && c->s[c->i] >= '0' && c->s[c->i] <= '9') c->i++;
    return iso_api_parse_u64(c->s + start, c->i - start, out) == 0;
}

static bool parse_literal(cur_t *c, const char *lit) {
    size_t n = strlen(lit);
    if (c->len - c->i < n || memcmp(c->s + c->i, lit, n) != 0) return false;
    c->i += n;
    return true;
}

static bool parse_bool(cur_t *c, bool *out) {
    if (parse_literal(c, "true")) {
        *out = true;
        return true;
    }
    if (parse_literal(c, "false")) {
        *out = false;
        return true;
    }
    return false;
}

enum {
    K_NAME = 1u << 0,
    K_SIZE = 1u << 1,
    K_SHA = 1u << 2,
    K_ID = 1u << 3,
    K_FORCE = 1u << 4,
    K_CURSOR = 1u << 5
};

static bool key_is(const char *key, size_t klen, const char *lit) {
    return klen == strlen(lit) && memcmp(key, lit, klen) == 0;
}

/* schema で許すキーの印(許さなければ 0) */
static unsigned key_bit(iso_api_schema_t schema, const char *key, size_t klen) {
    switch (schema) {
    case ISO_API_SCHEMA_BEGIN:
        if (key_is(key, klen, "name")) return K_NAME;
        if (key_is(key, klen, "size")) return K_SIZE;
        if (key_is(key, klen, "sha256")) return K_SHA;
        return 0;
    case ISO_API_SCHEMA_MOUNT:
        if (key_is(key, klen, "id")) return K_ID;
        if (key_is(key, klen, "force")) return K_FORCE;
        return 0;
    case ISO_API_SCHEMA_EJECT:
        if (key_is(key, klen, "force")) return K_FORCE;
        return 0;
    case ISO_API_SCHEMA_RESCAN:
        if (key_is(key, klen, "cursor")) return K_CURSOR;
        return 0;
    default: return 0;
    }
}

static bool parse_value(cur_t *c, unsigned bit, iso_api_body_t *out) {
    switch (bit) {
    case K_NAME:
        if (!parse_string(c, out->name, ISO_NAME_MAX_BYTES, &out->name_len)) return false;
        out->name[out->name_len] = '\0';
        return iso_name_valid(out->name, out->name_len);
    case K_SIZE:
        if (!parse_uint(c, &out->size)) return false;
        return out->size >= ISO_IMAGE_BYTES_MIN && out->size <= ISO_IMAGE_BYTES_MAX;
    case K_SHA: {
        char hex[2 * ISO_SHA256_LEN];
        size_t n = 0;
        if (!parse_string(c, hex, sizeof(hex), &n) || n != sizeof(hex)) return false;
        for (size_t i = 0; i < ISO_SHA256_LEN; i++) {
            int hi = hexval(hex[2 * i]);
            int lo = hexval(hex[2 * i + 1]);
            if (hi < 0 || lo < 0) return false;
            out->sha[i] = (uint8_t)((hi << 4) | lo);
        }
        out->has_sha = true;
        return true;
    }
    case K_ID: {
        size_t n = 0;
        if (!parse_string(c, out->id, ISO_ID_LEN, &n) || n != ISO_ID_LEN) return false;
        out->id[ISO_ID_LEN] = '\0';
        return iso_id_valid(out->id);
    }
    case K_FORCE: return parse_bool(c, &out->force);
    case K_CURSOR: {
        uint64_t v = 0;
        if (!parse_uint(c, &v) || v > 65535u) return false;
        out->cursor = (uint32_t)v;
        return true;
    }
    default: return false;
    }
}

int iso_api_parse_body(iso_api_schema_t schema, const char *body, size_t len, iso_api_body_t *out) {
    memset(out, 0, sizeof(*out));
    if (len == 0) return (schema == ISO_API_SCHEMA_BEGIN || schema == ISO_API_SCHEMA_MOUNT) ? -1 : 0;
    if (!body || len > ISO_API_BODY_MAX) return -1;

    cur_t c = {body, len, 0};
    skip_ws(&c);
    if (!eat(&c, '{')) return -1;
    skip_ws(&c);
    unsigned seen = 0;
    if (!eat(&c, '}')) {
        for (;;) {
            skip_ws(&c);
            char key[16];
            size_t klen = 0;
            if (!parse_string(&c, key, sizeof(key), &klen)) return -1;
            unsigned bit = key_bit(schema, key, klen);
            if (bit == 0 || (seen & bit) != 0) return -1; /* 定義していないキー、重複 */
            seen |= bit;
            skip_ws(&c);
            if (!eat(&c, ':')) return -1;
            skip_ws(&c);
            if (!parse_value(&c, bit, out)) return -1;
            skip_ws(&c);
            if (eat(&c, ',')) continue;
            if (eat(&c, '}')) break;
            return -1;
        }
    }
    skip_ws(&c);
    if (c.i != c.len) return -1; /* '}' の後に空白以外がある */

    if (schema == ISO_API_SCHEMA_BEGIN && (seen & (K_NAME | K_SIZE)) != (K_NAME | K_SIZE)) return -1;
    if (schema == ISO_API_SCHEMA_MOUNT && (seen & K_ID) == 0) return -1;
    return 0;
}

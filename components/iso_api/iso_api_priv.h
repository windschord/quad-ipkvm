#ifndef ISO_API_PRIV_H
#define ISO_API_PRIV_H

/* iso_api の内部(iso_api.c / iso_api_json.c / iso_api_fmt.c の間だけ。公開しない) */

#include "iso_api.h"

/* ---- JSON のボディの解析(iso_api_json.c) ------------------------------------------------- */

typedef enum {
    ISO_API_SCHEMA_BEGIN = 0, /* {"name":"...","size":N[,"sha256":"<64 桁の 16 進>"]} */
    ISO_API_SCHEMA_MOUNT,     /* {"id":"<8 桁の小文字の 16 進>"[,"force":true|false]} */
    ISO_API_SCHEMA_EJECT,     /* {} / {"force":true|false} */
    ISO_API_SCHEMA_RESCAN,    /* {} / {"cursor":0..65535} */
    ISO_API_SCHEMA_FINISH     /* {} */
} iso_api_schema_t;

typedef struct {
    char name[ISO_NAME_MAX_BYTES + 1]; /* BEGIN: 名前の規則に合うもの(NUL 終端) */
    size_t name_len;
    uint64_t size;                     /* BEGIN: ISO_IMAGE_BYTES_MIN..ISO_IMAGE_BYTES_MAX */
    bool has_sha;
    uint8_t sha[ISO_SHA256_LEN];       /* BEGIN: 16 進(大文字小文字を受け付ける)を復号したもの */
    char id[ISO_ID_LEN + 1];           /* MOUNT */
    bool force;                        /* MOUNT / EJECT(省略は false) */
    uint32_t cursor;                   /* RESCAN(省略は 0) */
} iso_api_body_t;

/* body(len バイト。NUL を含んでもよい)を、schema に従って厳密に解析する。0 = 成功、-1 = 不正(400)。out は、失敗のとき、不定。
 * len == 0 は、begin / mount では不正、ほかでは `{}`(空の本体)として成功する。 */
int iso_api_parse_body(iso_api_schema_t schema, const char *body, size_t len, iso_api_body_t *out);

/* ---- 応答の書き出し(iso_api_fmt.c) -------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ok;
} iso_w_t;

void iso_w_init(iso_w_t *w, char *buf, size_t cap);
void iso_w_str(iso_w_t *w, const char *s);
void iso_w_u64(iso_w_t *w, uint64_t v);
void iso_w_bool(iso_w_t *w, bool v);
/* 引用符つきの JSON 文字列(`"` と C0 制御文字をエスケープ。ほかのバイトは、そのまま) */
void iso_w_json_str(iso_w_t *w, const char *s, size_t len);
/* 小文字の 16 進(引用符なし) */
void iso_w_hex(iso_w_t *w, const uint8_t *p, size_t n);
/* 失敗(溢れ)なら出力を空にして -1。成功なら *out_len に長さを入れて 0。 */
int iso_w_finish(iso_w_t *w, size_t *out_len);

/* GET /api/v1/iso の本文。media は、state_lock の下で取ったもの。0 = 成功、-1 = バッファ不足。 */
int iso_api_format_list(const iso_api_snapshot_t *snap, const iso_media_status_t *media, char *out, size_t cap, size_t *out_len);

#endif /* ISO_API_PRIV_H */

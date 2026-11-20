#ifndef API_COMMON_H
#define API_COMMON_H

/*
 * iso_api のテスト共通の部品(実行ファイルごとに別のコピー)。core_common.h(モックの fs、偽の時計、iso_core、ワーカーのループの代わりの drive())に、
 * iso_media、公開ビューのコピー(iso_api_snapshot_t)、ロックの数え上げ(順序の違反を見つける)、要求を作る関数を足す。
 * 期待値は手書き(実装から計算しない)。
 */

#include "core_common.h"
#include "iso_api.h"
#include "iso_api_priv.h"

static iso_media_t MEDIA;
static iso_api_snapshot_t SNAP;
static iso_api_env_t ENV;

/* ロックの数え上げ。op_trylock を state_lock の中で呼ぶ、state_lock の入れ子、釣り合わない unlock は、違反 */
static int SL_DEPTH, SL_CALLS, OL_DEPTH, OL_CALLS, OL_TRY_CALLS;
static bool OL_FAIL;                 /* true なら、op_trylock は取れない(503 busy) */
static bool LOCK_VIOLATION;
static uint32_t OL_LAST_WAIT;
static void (*OL_HOOK)(void);        /* op_trylock が成功した直後に呼ぶ(判定と処理の間の、競合の注入) */
static void (*SL_HOOK)(void);        /* SL_CALLS が SL_HOOK_AT になった state_lock の中(ロックを取った直後)で呼ぶ */
static int SL_HOOK_AT;

static void t_state_lock(void *c) {
    (void)c;
    SL_DEPTH++;
    SL_CALLS++;
    if (SL_DEPTH > 1) LOCK_VIOLATION = true;
    if (SL_HOOK && SL_CALLS == SL_HOOK_AT) SL_HOOK();
}
static void t_state_unlock(void *c) {
    (void)c;
    SL_DEPTH--;
    if (SL_DEPTH < 0) LOCK_VIOLATION = true;
}
static bool t_op_try(void *c, uint32_t w) {
    (void)c;
    OL_TRY_CALLS++;
    OL_LAST_WAIT = w;
    if (SL_DEPTH != 0) LOCK_VIOLATION = true; /* iso_state_mutex を持ったまま iso_op_mutex を取らない */
    if (OL_FAIL) return false;
    OL_DEPTH++;
    OL_CALLS++;
    if (OL_DEPTH > 1) LOCK_VIOLATION = true;
    if (OL_HOOK) OL_HOOK();
    return true;
}
static void t_op_unlock(void *c) {
    (void)c;
    OL_DEPTH--;
    if (OL_DEPTH < 0) LOCK_VIOLATION = true;
}

/* iso_core の hooks: ビューの更新で、公開ビューのコピーを更新する(グルーの実装と同じ。iso_state_mutex の下) */
static void api_view_cb(void *ctx, const iso_view_t *v) {
    (void)ctx;
    VIEW_COPY = *v;
    VIEW_UPDATES++;
    t_state_lock(NULL);
    iso_api_snapshot_update(&SNAP, v, iso_core_upload(&CORE));
    t_state_unlock(NULL);
}

static bool api_mounted_cb(void *ctx, const char *id) {
    (void)ctx;
    uint32_t v = 0;
    if (!iso_api_id_to_u32(id, &v)) return false;
    t_state_lock(NULL);
    iso_media_status_t s;
    iso_media_get_status(&MEDIA, &s);
    t_state_unlock(NULL);
    return s.mounted && s.id == v;
}

static inline void fx_api_setup(void) {
    fx_core_setup(0, 0);
    CORE.cfg.hooks.view_update = api_view_cb;
    CORE.cfg.hooks.is_mounted = api_mounted_cb;
    iso_media_init(&MEDIA, NULL, NULL);
    memset(&SNAP, 0, sizeof(SNAP));
    memset(&ENV, 0, sizeof(ENV));
    ENV.ctx = NULL;
    ENV.state_lock = t_state_lock;
    ENV.state_unlock = t_state_unlock;
    ENV.op_trylock = t_op_try;
    ENV.op_unlock = t_op_unlock;
    ENV.snapshot = &SNAP;
    ENV.media = &MEDIA;
    ENV.core = &CORE;
    SL_DEPTH = SL_CALLS = OL_DEPTH = OL_CALLS = OL_TRY_CALLS = 0;
    OL_FAIL = false;
    LOCK_VIOLATION = false;
    OL_LAST_WAIT = 0;
    OL_HOOK = NULL;
    SL_HOOK = NULL;
    SL_HOOK_AT = 0;
    iso_core_publish(&CORE);
}

static inline void fx_api_teardown(void) {
    TEST_ASSERT_FALSE_MESSAGE(LOCK_VIOLATION, "lock order / balance violation");
    TEST_ASSERT_EQUAL_INT(0, SL_DEPTH);
    TEST_ASSERT_EQUAL_INT(0, OL_DEPTH);
    fx_core_teardown();
}

/* ---- イメージの準備(スキャンの前に、ディスクに置く) ---------------------------------------------------- */

#define SZ2 (2ull * PART_SMALL) /* 69632 バイト: 17 ブロックのパーツ 2 つ */

/* ready のイメージ(全パーツがディスクにある)。api_boot() の後、drive() でエクステントの表ができる */
static inline void api_add_ready(const char *id, const char *name, uint64_t size) {
    make_image(id, name, size, PB_SMALL, ISO_META_READY, size);
}
/* incomplete のイメージ(先頭の have バイトがある) */
static inline void api_add_incomplete(const char *id, const char *name, uint64_t size, uint64_t have) {
    make_image(id, name, size, PB_SMALL, ISO_META_INCOMPLETE, have);
}
/* invalid のイメージ(理由 reason: 1..4) */
static inline void api_add_invalid(const char *id, const char *name, uint64_t size, uint8_t reason) {
    iso_meta_t m = mk_meta(name, size, PB_SMALL, ISO_META_INVALID, 5);
    m.invalid_reason = reason;
    put_meta(id, &m);
    put_parts_data(id, PB_SMALL, size);
}

/* スキャン(起動)して、INDEX を最後まで流す */
static inline void api_boot(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    (void)drive(0, 1000);
    iso_core_publish(&CORE);
}
/* スキャンだけ(INDEX を流さない。ready でも、エクステントの表がない = 表示は finalizing) */
static inline void api_boot_no_index(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    iso_core_publish(&CORE);
}

static inline iso_upload_t *api_up(void) { return iso_core_upload(&CORE); }
static inline iso_catalog_entry_t *api_find(const char *id) { return iso_catalog_find(&CAT, id); }

/* 公開ビューの sd.state を変える(ビューを作り直さずに、コピーだけ) */
static inline void api_set_sd(iso_sd_state_t s) {
    SD_STATE = s;
    iso_core_publish(&CORE);
}

/* ---- 要求と応答 ------------------------------------------------------------------------------ */

static char RBUF[ISO_API_RESPONSE_MAX];
static iso_api_response_t RSP;

static inline void api_rq(iso_api_request_t *rq, iso_api_method_t m, const char *path, const char *ct, const char *body) {
    memset(rq, 0, sizeof(*rq));
    rq->method = m;
    rq->path = path;
    rq->content_type = ct;
    if (body) {
        rq->has_content_length = true;
        rq->content_length = strlen(body);
        rq->body = body;
        rq->body_len = strlen(body);
    }
}

static inline void api_call_rq(const iso_api_env_t *env, const iso_api_request_t *rq) {
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_handle(env, rq, &RSP);
}

static inline void api_call(iso_api_method_t m, const char *path, const char *ct, const char *body) {
    iso_api_request_t rq;
    api_rq(&rq, m, path, ct, body);
    api_call_rq(&ENV, &rq);
}

#define JSON_CT "application/json"
#define OCTET_CT "application/octet-stream"

static inline void api_get(void) { api_call(ISO_API_M_GET, "/api/v1/iso", NULL, NULL); }
static inline void api_post_json(const char *path, const char *body) { api_call(ISO_API_M_POST, path, JSON_CT, body); }
static inline void api_post_empty(const char *path) { api_call(ISO_API_M_POST, path, NULL, NULL); }
static inline void api_delete(const char *id) {
    char path[64];
    snprintf(path, sizeof(path), "/api/v1/iso/%s", id);
    api_call(ISO_API_M_DELETE, path, NULL, NULL);
}

/* 本文の内容と、body_len の整合を確かめる */
static inline void expect_rsp(int status, const char *body) {
    TEST_ASSERT_EQUAL_INT(status, RSP.status);
    TEST_ASSERT_EQUAL_STRING(body, RSP.body);
    TEST_ASSERT_EQUAL_UINT(strlen(body), RSP.body_len);
}

/* {"error":"<code>"} */
static inline void expect_err(int status, const char *code) {
    char b[160];
    snprintf(b, sizeof(b), "{\"error\":\"%s\"}", code);
    expect_rsp(status, b);
}

/* PUT の、本体を読む前の判定。off が NULL なら X-Upload-Offset なし */
static inline bool api_put_pre(const iso_api_env_t *env, const char *id, const char *ct, bool has_cl, uint64_t cl, const char *off) {
    char path[64];
    snprintf(path, sizeof(path), "/api/v1/iso/%s", id);
    iso_api_request_t rq;
    api_rq(&rq, ISO_API_M_PUT, path, ct, NULL);
    rq.has_content_length = has_cl;
    rq.content_length = cl;
    rq.upload_offset = off;
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    return iso_api_put_precheck(env, &rq, &RSP);
}

/* 画像の [from, from + n) を 1 つのチャンクとして、precheck → commit(応答は RSP)。precheck が拒否なら、その応答 */
static inline void api_put_chunk(const char *id, uint64_t from, size_t n) {
    static uint8_t buf[1024 * 1024];
    char off[24];
    snprintf(off, sizeof(off), "%llu", (unsigned long long)from);
    if (!api_put_pre(&ENV, id, OCTET_CT, true, n, off)) return;
    gen_fill(buf, from, n);
    iso_api_put_t put = RSP.put;
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &put, buf, n, &RSP);
}

#endif /* API_COMMON_H */

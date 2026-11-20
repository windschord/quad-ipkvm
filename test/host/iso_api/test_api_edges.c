/*
 * iso_api の境界と、防御の分岐: 応答の初期化(汚れた応答を渡しても、前の内容が残らない)、書き出しの部品の境界、引数の不正、
 * 応答の置き場が小さいとき、公開ビューが満杯(16 個)のときの探索、空き容量の推定(途中でないイメージを数えない)、
 * 処理の側が、判定の後に変わった状態(SD が消えた、すでにマウントされた、表がない)を見ること。期待値は手書き。
 */
#include "api_common.h"

void setUp(void) { fx_api_setup(); }
void tearDown(void) { fx_api_teardown(); }

#define A "aaaaaaa1"
#define B "bbbbbbb2"

static void snap_sd_gone(void) { SNAP.view.sd_state = ISO_SD_NO_CARD; }
static void hook_sd_gone(void) {
    SD_STATE = ISO_SD_NO_CARD;
    iso_core_publish(&CORE);
}
static void hook_mount_a(void) { (void)iso_media_mount(&MEDIA, 0xaaaaaaa1u, false); }

/* ---- 応答の初期化 ---------------------------------------------------------------------- */

void test_response_init_clears_the_response_and_the_first_byte(void) {
    char buf[16];
    memset(buf, 'X', sizeof(buf));
    iso_api_response_t r;
    memset(&r, 0xCC, sizeof(r));
    iso_api_response_init(&r, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, r.status);
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL_UINT(0, r.body_len);
    TEST_ASSERT_EQUAL_PTR(buf, r.body);
    TEST_ASSERT_EQUAL_UINT(sizeof(buf), r.body_cap);
    TEST_ASSERT_FALSE(r.close_connection);
    TEST_ASSERT_NULL(r.allow);
    TEST_ASSERT_FALSE(r.has_retry_after);
    TEST_ASSERT_FALSE(r.log);
    TEST_ASSERT_FALSE(r.put.accepted);
    TEST_ASSERT_EQUAL_STRING("", r.log_id);
    /* cap 1: NUL だけ */
    memset(buf, 'X', sizeof(buf));
    iso_api_response_init(&r, buf, 1);
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)buf[0]);
    TEST_ASSERT_EQUAL_UINT8('X', (uint8_t)buf[1]);
    /* cap 0: 書かない */
    memset(buf, 'X', sizeof(buf));
    iso_api_response_init(&r, buf, 0);
    TEST_ASSERT_EQUAL_UINT8('X', (uint8_t)buf[0]);
    /* NULL の置き場: 何も書かない(落ちない) */
    iso_api_response_init(&r, NULL, 10);
    TEST_ASSERT_NULL(r.body);
    iso_api_response_init(&r, NULL, 0);
}

/* ---- 書き出しの部品の境界 -------------------------------------------------------------------- */

void test_writer_edges(void) {
    char b[8];
    size_t n = 99;
    iso_w_t w;
    /* cap 1: 何も書かなければ成功(空の文字列) */
    memset(b, 'X', sizeof(b));
    iso_w_init(&w, b, 1);
    TEST_ASSERT_TRUE(w.ok);
    TEST_ASSERT_EQUAL_INT(0, iso_w_finish(&w, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)b[0]);
    TEST_ASSERT_EQUAL_UINT8('X', (uint8_t)b[1]);
    /* cap 1 で 1 文字書くと失敗。出力は空(NUL) */
    memset(b, 'X', sizeof(b));
    iso_w_init(&w, b, 1);
    iso_w_str(&w, "a");
    TEST_ASSERT_EQUAL_INT(-1, iso_w_finish(&w, &n));
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)b[0]);
    TEST_ASSERT_EQUAL_UINT8('X', (uint8_t)b[1]);
    /* cap 0(バッファはあるが、書けない): 何も書かない */
    memset(b, 'X', sizeof(b));
    iso_w_init(&w, b, 0);
    TEST_ASSERT_FALSE(w.ok);
    iso_w_str(&w, "abc");
    TEST_ASSERT_EQUAL_INT(-1, iso_w_finish(&w, &n));
    TEST_ASSERT_EQUAL_UINT8('X', (uint8_t)b[0]);
    /* 初期化は、先頭の 1 バイトを NUL にする */
    memset(b, 'X', sizeof(b));
    iso_w_init(&w, b, sizeof(b));
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)b[0]);
    TEST_ASSERT_EQUAL_UINT8('X', (uint8_t)b[1]);
    /* 失敗した書き出しの後は、何も書かない(NULL のバッファでも落ちない) */
    iso_w_init(&w, NULL, 10);
    iso_w_str(&w, "abc");
    iso_w_u64(&w, 5);
    iso_w_json_str(&w, "x", 1);
    iso_w_hex(&w, (const uint8_t *)"ab", 2);
    TEST_ASSERT_EQUAL_INT(-1, iso_w_finish(&w, NULL));
    /* 一度溢れたら、その後は、短い文字列も書かない */
    iso_w_init(&w, b, 4);
    iso_w_str(&w, "abcd"); /* 溢れる */
    iso_w_str(&w, "");
    iso_w_str(&w, "z");
    TEST_ASSERT_EQUAL_INT(-1, iso_w_finish(&w, &n));
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)b[0]);
}

/* ---- 汚れた応答を渡しても、前の内容が残らない ----------------------------------------------------- */

static void dirty(iso_api_response_t *r) {
    iso_api_response_init(r, RBUF, sizeof(RBUF));
    snprintf(RBUF, sizeof(RBUF), "junk");
    r->body_len = 4;
    r->status = 999;
    r->allow = "X";
    r->has_retry_after = true;
    r->retry_after_s = 77;
    r->close_connection = true;
    r->log = true;
    r->log_kind = "k";
    r->log_reason = "r";
    snprintf(r->log_id, sizeof(r->log_id), "zzzzzzzz");
    r->log_has_size = true;
    r->log_size = 5;
    r->put.accepted = true;
    r->put.len = 9;
}

static void expect_no_leftovers(const iso_api_response_t *r) {
    TEST_ASSERT_NULL(r->allow);
    TEST_ASSERT_FALSE(r->has_retry_after);
    TEST_ASSERT_EQUAL_UINT32(0, r->retry_after_s);
    TEST_ASSERT_FALSE(r->close_connection);
    TEST_ASSERT_FALSE(r->log_has_size);
    TEST_ASSERT_EQUAL_UINT64(0, r->log_size);
    TEST_ASSERT_EQUAL_UINT32(0, r->put.len);
}

void test_entry_points_discard_what_the_response_held_before(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_incomplete(B, "beta", SZ2, PART_SMALL);
    api_boot();
    iso_api_request_t rq;
    /* headers_check */
    api_rq(&rq, ISO_API_M_GET, "/api/v1/iso", NULL, NULL);
    dirty(&RSP);
    TEST_ASSERT_TRUE(iso_api_headers_check(&rq, &RSP));
    TEST_ASSERT_EQUAL_INT(0, RSP.status);
    TEST_ASSERT_EQUAL_STRING("", RSP.body);
    TEST_ASSERT_EQUAL_UINT(0, RSP.body_len);
    TEST_ASSERT_FALSE(RSP.log);
    TEST_ASSERT_NULL(RSP.log_kind);
    TEST_ASSERT_NULL(RSP.log_reason);
    TEST_ASSERT_EQUAL_STRING("", RSP.log_id);
    TEST_ASSERT_FALSE(RSP.put.accepted);
    expect_no_leftovers(&RSP);
    /* handle(GET)、handle(POST eject) */
    dirty(&RSP);
    iso_api_handle(&ENV, &rq, &RSP);
    TEST_ASSERT_EQUAL_INT(200, RSP.status);
    TEST_ASSERT_FALSE(RSP.log);
    TEST_ASSERT_EQUAL_STRING("", RSP.log_id);
    TEST_ASSERT_FALSE(RSP.put.accepted);
    expect_no_leftovers(&RSP);
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso/eject", NULL, NULL);
    dirty(&RSP);
    iso_api_handle(&ENV, &rq, &RSP);
    expect_rsp(200, "{\"result\":\"not_mounted\"}");
    TEST_ASSERT_TRUE(RSP.log);
    TEST_ASSERT_EQUAL_STRING("", RSP.log_id);
    TEST_ASSERT_FALSE(RSP.put.accepted);
    expect_no_leftovers(&RSP);
    /* put_precheck(受理) */
    api_rq(&rq, ISO_API_M_PUT, "/api/v1/iso/" B, OCTET_CT, NULL);
    rq.has_content_length = true;
    rq.content_length = 100;
    rq.upload_offset = "34816";
    dirty(&RSP);
    TEST_ASSERT_TRUE(iso_api_put_precheck(&ENV, &rq, &RSP));
    TEST_ASSERT_EQUAL_INT(0, RSP.status);
    TEST_ASSERT_EQUAL_STRING("", RSP.body);
    TEST_ASSERT_TRUE(RSP.put.accepted);
    TEST_ASSERT_EQUAL_STRING(B, RSP.put.id);
    TEST_ASSERT_EQUAL_UINT32(100, RSP.put.len);
    TEST_ASSERT_FALSE(RSP.log);
    TEST_ASSERT_NULL(RSP.log_kind);
    TEST_ASSERT_EQUAL_STRING(B, RSP.log_id); /* PUT は記録しない(log = false)が、id は入る */
    TEST_ASSERT_FALSE(RSP.close_connection);
    TEST_ASSERT_NULL(RSP.allow);
    TEST_ASSERT_FALSE(RSP.has_retry_after);
    /* handle(PUT): 受理は status 0 */
    iso_api_put_t put = RSP.put;
    dirty(&RSP);
    iso_api_handle(&ENV, &rq, &RSP);
    TEST_ASSERT_EQUAL_INT(0, RSP.status);
    TEST_ASSERT_TRUE(RSP.put.accepted);
    /* put_commit */
    uint8_t data[100];
    gen_fill(data, 34816, sizeof(data));
    dirty(&RSP);
    iso_api_put_commit(&ENV, &put, data, sizeof(data), &RSP);
    expect_rsp(200, "{\"received\":34916}");
    TEST_ASSERT_FALSE(RSP.log);
    TEST_ASSERT_NULL(RSP.log_kind);
    TEST_ASSERT_EQUAL_STRING("", RSP.log_id);
    TEST_ASSERT_FALSE(RSP.put.accepted);
    expect_no_leftovers(&RSP);
}

void test_headers_check_directly(void) {
    iso_api_request_t rq;
    /* NULL の応答は、何もしない */
    api_rq(&rq, ISO_API_M_GET, "/api/v1/iso", NULL, NULL);
    TEST_ASSERT_FALSE(iso_api_headers_check(&rq, NULL));
    /* 要求の不正 */
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    TEST_ASSERT_FALSE(iso_api_headers_check(NULL, &RSP));
    expect_err(500, "internal_error");
    api_rq(&rq, ISO_API_M_GET, NULL, NULL, NULL);
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    TEST_ASSERT_FALSE(iso_api_headers_check(&rq, &RSP));
    expect_err(500, "internal_error");
    /* PUT の拒否は、接続を閉じる。PUT でない拒否は、閉じない */
    api_rq(&rq, ISO_API_M_PUT, "/api/v1/iso/" A, JSON_CT, NULL);
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    TEST_ASSERT_FALSE(iso_api_headers_check(&rq, &RSP));
    expect_err(415, "unsupported_media_type");
    TEST_ASSERT_TRUE(RSP.close_connection);
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso", "text/plain", "{}");
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    TEST_ASSERT_FALSE(iso_api_headers_check(&rq, &RSP));
    expect_err(415, "unsupported_media_type");
    TEST_ASSERT_FALSE(RSP.close_connection);
    /* 通る */
    api_rq(&rq, ISO_API_M_PUT, "/api/v1/iso/" A, OCTET_CT, NULL);
    rq.has_content_length = true;
    rq.content_length = 5;
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    TEST_ASSERT_TRUE(iso_api_headers_check(&rq, &RSP));
    TEST_ASSERT_EQUAL_INT(0, RSP.status);
    TEST_ASSERT_FALSE(RSP.close_connection);
    /* 経路の 404 / 405 */
    api_rq(&rq, ISO_API_M_PUT, "/api/v1/iso", OCTET_CT, NULL);
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    TEST_ASSERT_FALSE(iso_api_headers_check(&rq, &RSP));
    expect_err(405, "method_not_allowed");
    TEST_ASSERT_TRUE(RSP.close_connection);
}

void test_put_precheck_and_commit_argument_errors(void) {
    iso_api_request_t rq;
    api_rq(&rq, ISO_API_M_PUT, "/api/v1/iso/" A, OCTET_CT, NULL);
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    TEST_ASSERT_FALSE(iso_api_put_precheck(&ENV, NULL, &RSP));
    expect_err(500, "internal_error");
    TEST_ASSERT_TRUE(RSP.close_connection);
    api_rq(&rq, ISO_API_M_PUT, NULL, OCTET_CT, NULL);
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    TEST_ASSERT_FALSE(iso_api_put_precheck(&ENV, &rq, &RSP));
    expect_err(500, "internal_error");
    TEST_ASSERT_TRUE(RSP.close_connection);
    /* NULL の応答は、何もしない */
    iso_api_put_t put;
    memset(&put, 0, sizeof(put));
    iso_api_put_commit(&ENV, &put, "x", 1, NULL);
    iso_api_handle(&ENV, &rq, NULL);
    /* handle の要求の不正 */
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_handle(&ENV, NULL, &RSP);
    expect_err(500, "internal_error");
    api_rq(&rq, ISO_API_M_GET, NULL, NULL, NULL);
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_handle(&ENV, &rq, &RSP);
    expect_err(500, "internal_error");
}

/* ---- 応答の置き場が小さいとき -------------------------------------------------------------------- */

void test_a_body_that_does_not_fit_becomes_the_short_internal_error(void) {
    char small[30]; /* 415 の本体(34 バイト)は入らず、internal_error(26 バイト)は入る */
    iso_api_request_t rq;
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso", "text/plain", "{}");
    iso_api_response_init(&RSP, small, sizeof(small));
    iso_api_handle(&ENV, &rq, &RSP);
    TEST_ASSERT_EQUAL_INT(500, RSP.status);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"internal_error\"}", RSP.body);
    TEST_ASSERT_EQUAL_UINT(26, RSP.body_len);
    TEST_ASSERT_EQUAL_STRING("internal_error", RSP.log_reason);
    /* ちょうど入る大きさ(本体 34 バイト + NUL)なら、そのまま */
    char fit[35];
    iso_api_response_init(&RSP, fit, sizeof(fit));
    iso_api_handle(&ENV, &rq, &RSP);
    expect_err(415, "unsupported_media_type");
    char one_less[34];
    iso_api_response_init(&RSP, one_less, sizeof(one_less));
    iso_api_handle(&ENV, &rq, &RSP);
    TEST_ASSERT_EQUAL_INT(500, RSP.status);
    /* 置き場が小さすぎて、internal_error も入らない: 空の本体 */
    char tiny[8];
    iso_api_response_init(&RSP, tiny, sizeof(tiny));
    iso_api_handle(&ENV, &rq, &RSP);
    TEST_ASSERT_EQUAL_INT(500, RSP.status);
    TEST_ASSERT_EQUAL_UINT(0, RSP.body_len);
    TEST_ASSERT_EQUAL_STRING("", RSP.body);
    TEST_ASSERT_EQUAL_STRING("internal_error", RSP.log_reason);
}

void test_an_unknown_invalid_reason_in_the_view_is_a_500(void) {
    api_add_invalid(A, "alpha", SZ2, ISO_INVALID_NOT_ISO);
    api_boot();
    SNAP.view.images[0].invalid_reason = 0;
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_err(500, "internal_error");
    SNAP.view.images[0].invalid_reason = 77;
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_err(500, "internal_error");
    SNAP.view.images[0].invalid_reason = ISO_INVALID_META_CORRUPT;
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_err(422, "meta_corrupt");
}

/* ---- 公開ビューが満杯(16 個)のとき ------------------------------------------------------------- */

static void full_catalog(void) {
    for (unsigned i = 0; i < ISO_MAX_IMAGES; i++) {
        char id[9], name[16];
        snprintf(id, sizeof(id), "%08x", 0x100u + i);
        snprintf(name, sizeof(name), "img%02u", i);
        if (i == 7) {
            api_add_incomplete(id, name, SZ2, PART_SMALL);
        } else {
            api_add_ready(id, name, SZ2);
        }
    }
    api_boot();
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, SNAP.view.count);
}

void test_lookups_in_a_full_view_do_not_read_past_the_array(void) {
    full_catalog();
    api_post_json("/api/v1/iso/mount", "{\"id\":\"cccccccc\"}");
    expect_err(404, "not_found");
    api_delete("cccccccc");
    expect_err(404, "not_found");
    api_post_empty("/api/v1/iso/cccccccc/finish");
    expect_err(404, "not_found");
    TEST_ASSERT_FALSE(api_put_pre(&ENV, "cccccccc", OCTET_CT, true, 10, "0"));
    expect_err(404, "not_found");
    /* 最後の要素は見つかる */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"0000010f\"}");
    expect_rsp(200, "{\"result\":\"mounted\",\"id\":\"0000010f\"}");
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"mounted\":{\"id\":\"0000010f\",\"name\":\"img15\"}"));
    api_post_json("/api/v1/iso", "{\"name\":\"no-such-name\",\"size\":34816}"); /* name_exists が 16 個を調べ終えて、too_many_images */
    expect_err(409, "too_many_images");
}

void test_resume_estimate_in_a_full_view_counts_every_image_once(void) {
    full_catalog(); /* img07 だけ incomplete(残り 34816) */
    const uint64_t reserve = 64ull * 1048576ull;
    iso_upload_set_free(api_up(), 64 * GIB, 34816 + reserve);
    iso_core_publish(&CORE);
    TEST_ASSERT_TRUE(api_put_pre(&ENV, "00000107", OCTET_CT, true, 100, "34816"));
    iso_upload_set_free(api_up(), 64 * GIB, 34816 + reserve - 1);
    iso_core_publish(&CORE);
    TEST_ASSERT_FALSE(api_put_pre(&ENV, "00000107", OCTET_CT, true, 100, "34816"));
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":67108863}");
}

/* ---- 空き容量の推定 ------------------------------------------------------------------------ */

void test_estimate_ignores_images_that_are_not_being_uploaded(void) {
    /* invalid(途中までしかパーツがない)と deleting は、「これから使う分」に数えない */
    iso_meta_t m = mk_meta("partial", SZ2, PB_SMALL, ISO_META_INVALID, 5);
    m.invalid_reason = ISO_INVALID_PART_MISMATCH;
    put_meta(A, &m);
    put_parts_data(A, PB_SMALL, PART_SMALL);
    api_add_ready(B, "beta", SZ2);
    api_boot();
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, api_find(A)->state);
    TEST_ASSERT_EQUAL_UINT64(PART_SMALL, api_find(A)->received); /* received < size */
    const uint64_t reserve = 64ull * 1048576ull;
    iso_upload_set_free(api_up(), 64 * GIB, 40000 + reserve);
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso", "{\"name\":\"fits\",\"size\":40000}");
    TEST_ASSERT_EQUAL_INT(201, RSP.status); /* invalid の残りは引かない */
}

void test_estimate_is_zero_when_the_used_bytes_exceed_the_free_space(void) {
    api_boot();
    iso_upload_set_free(api_up(), 64 * GIB, 1000);
    api_up()->consumed_since = 5000; /* 採取後に、空きより多く書いた */
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso", "{\"name\":\"x\",\"size\":34816}");
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":0}");
    api_up()->consumed_since = 1000; /* ちょうど */
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso", "{\"name\":\"x\",\"size\":34816}");
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":0}");
    api_up()->consumed_since = 999;
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso", "{\"name\":\"x\",\"size\":34816}");
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":1}");
}

void test_begin_larger_than_the_card_is_decided_from_the_view_without_the_op_lock(void) {
    api_boot();
    iso_upload_set_free(api_up(), 50000, 10 * GIB);
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso", "{\"name\":\"fits\",\"size\":50001}");
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":10737418240}");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

/* ---- 処理の側が、判定の後の状態を見る ------------------------------------------------------------ */

void test_every_changing_operation_rechecks_the_sd_after_taking_the_op_lock(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_incomplete(B, "beta", SZ2, SZ2);
    api_boot();
    struct {
        iso_api_method_t m;
        const char *path;
        const char *ct;
        const char *body;
    } T[] = {
        {ISO_API_M_POST, "/api/v1/iso", JSON_CT, "{\"name\":\"new\",\"size\":34816}"},
        {ISO_API_M_POST, "/api/v1/iso/" B "/finish", NULL, NULL},
        {ISO_API_M_DELETE, "/api/v1/iso/" A, NULL, NULL},
        {ISO_API_M_POST, "/api/v1/iso/rescan", NULL, NULL},
        {ISO_API_M_POST, "/api/v1/iso/mount", JSON_CT, "{\"id\":\"" A "\"}"},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        SD_STATE = ISO_SD_READY;
        iso_core_publish(&CORE);
        OL_HOOK = hook_sd_gone;
        api_call(T[i].m, T[i].path, T[i].ct, T[i].body);
        expect_err(503, "not_ready");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, OL_DEPTH, T[i].path);
    }
    TEST_ASSERT_EQUAL_UINT(2, CAT.count);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, api_find(A)->state);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, api_find(B)->state);
    TEST_ASSERT_FALSE(iso_media_is_loaded(&MEDIA));
}

void test_mount_checks_the_sd_again_inside_the_state_lock(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    SL_CALLS = 0;
    SL_HOOK = snap_sd_gone; /* state_lock: 1 = 判定、2 = op_enter の再検査、3 = マウントの直前 */
    SL_HOOK_AT = 3;
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    expect_err(503, "not_ready");
    TEST_ASSERT_FALSE(iso_media_is_loaded(&MEDIA));
    TEST_ASSERT_EQUAL_INT(0, SL_DEPTH);
    TEST_ASSERT_EQUAL_INT(0, OL_DEPTH);
}

void test_mount_that_finds_the_image_already_mounted_reports_it(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    OL_HOOK = hook_mount_a; /* 判定の後に、同じイメージがマウントされた */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    expect_rsp(200, "{\"result\":\"already_mounted\",\"id\":\"aaaaaaa1\"}");
    TEST_ASSERT_EQUAL_STRING("already_mounted", RSP.log_reason);
}

void test_mount_of_an_image_without_a_table_is_refused_even_if_the_view_says_ready(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot_no_index(); /* 表がない */
    EPOCH = 0;           /* 表がなくても、epoch の比較(表の epoch は 0 として読まれる)では、拒否されない値にする */
    SNAP.view.images[0].state = ISO_STATE_READY; /* 古いビュー */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    expect_err(409, "image_not_ready");
    TEST_ASSERT_FALSE(iso_media_is_loaded(&MEDIA));
}

void test_finish_that_finds_the_image_ready_reports_the_computed_sha_only(void) {
    /* ready で、SHA-256(計算値)がある */
    iso_meta_t m = mk_meta("withsha", SZ2, PB_SMALL, ISO_META_READY, 5);
    m.sha_flag = ISO_SHA_COMPUTED;
    for (int i = 0; i < 32; i++) m.sha256[i] = (uint8_t)((i % 16) * 0x11);
    put_meta(A, &m);
    put_parts_data(A, PB_SMALL, SZ2);
    /* ready で、宣言値だけ(計算していない)と、SHA-256 なし */
    iso_meta_t d = mk_meta("declared", SZ2, PB_SMALL, ISO_META_READY, 5);
    d.sha_flag = ISO_SHA_DECLARED;
    memset(d.sha256, 0x77, sizeof(d.sha256));
    put_meta(B, &d);
    put_parts_data(B, PB_SMALL, SZ2);
    api_add_ready("ccccccc3", "nosha", SZ2);
    api_boot();
    TEST_ASSERT_EQUAL_UINT(3, SNAP.view.count);
    for (size_t i = 0; i < 3; i++) SNAP.view.images[i].state = ISO_STATE_FINALIZING; /* 古いビュー: 処理へ進む */
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_rsp(200, "{\"state\":\"ready\",\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}");
    api_post_empty("/api/v1/iso/" B "/finish");
    expect_rsp(200, "{\"state\":\"ready\",\"sha256\":null}");
    api_post_empty("/api/v1/iso/ccccccc3/finish");
    expect_rsp(200, "{\"state\":\"ready\",\"sha256\":null}");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_response_init_clears_the_response_and_the_first_byte);
    RUN_TEST(test_writer_edges);
    RUN_TEST(test_entry_points_discard_what_the_response_held_before);
    RUN_TEST(test_headers_check_directly);
    RUN_TEST(test_put_precheck_and_commit_argument_errors);
    RUN_TEST(test_a_body_that_does_not_fit_becomes_the_short_internal_error);
    RUN_TEST(test_an_unknown_invalid_reason_in_the_view_is_a_500);
    RUN_TEST(test_lookups_in_a_full_view_do_not_read_past_the_array);
    RUN_TEST(test_resume_estimate_in_a_full_view_counts_every_image_once);
    RUN_TEST(test_estimate_ignores_images_that_are_not_being_uploaded);
    RUN_TEST(test_estimate_is_zero_when_the_used_bytes_exceed_the_free_space);
    RUN_TEST(test_begin_larger_than_the_card_is_decided_from_the_view_without_the_op_lock);
    RUN_TEST(test_every_changing_operation_rechecks_the_sd_after_taking_the_op_lock);
    RUN_TEST(test_mount_checks_the_sd_again_inside_the_state_lock);
    RUN_TEST(test_mount_that_finds_the_image_already_mounted_reports_it);
    RUN_TEST(test_mount_of_an_image_without_a_table_is_refused_even_if_the_view_says_ready);
    RUN_TEST(test_finish_that_finds_the_image_ready_reports_the_computed_sha_only);
    return UNITY_END();
}

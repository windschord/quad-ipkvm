/*
 * 判定の順序(WEB_API.md 8 章の 13 段のうち、この層の 3〜13)。複数の条件が同時に当てはまるとき、前の段のエラーが先に出ること。
 *   3 パスの id の形式不正(404) → 4 Content-Type(415) → 5 PUT の Content-Length の欠落(411)、JSON のボディの大きさ(413)、chunk_size 超過(413)
 *   → 6 形式(400) → 7 SD が使えない(503 not_ready。eject は 200 not_mounted) → 8 そのイメージがない(404) → 9 状態の矛盾(409。並びの順)
 *   → 10 offset_mismatch(409) → 11 チャンクが size を超える(400) → 12 空き容量(507) → 13 処理(503 busy、または受理)
 * 段 1・2(認証、Origin)は、http_guard(test_api_guard.c)。期待値は手書き。
 */
#include "api_common.h"

void setUp(void) { fx_api_setup(); }
void tearDown(void) { fx_api_teardown(); }

#define A "aaaaaaa1"
#define B "bbbbbbb2"
#define C "ccccccc3"

/* 要求を作って、PUT の precheck を呼ぶ(offset 文字列つき)。長さ n の本体を、off から */
static bool put_pre(const char *id, const char *ct, bool has_cl, uint64_t cl, const char *off) { return api_put_pre(&ENV, id, ct, has_cl, cl, off); }

static void boot_ab(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_ready(B, "beta", SZ2);
    api_boot();
}

/* ---- 段 3 → 段 4 以降 ---------------------------------------------------------------------- */

void test_step3_an_invalid_id_in_the_path_is_404_before_everything_else(void) {
    api_set_sd(ISO_SD_NO_CARD); /* 段 7 より前 */
    TEST_ASSERT_FALSE(put_pre("ZZZZZZZZ", "text/plain", false, 0, NULL)); /* 段 4(415)、段 5(411)、段 6(400)より前 */
    expect_err(404, "not_found");
    TEST_ASSERT_TRUE(RSP.close_connection);
    api_call(ISO_API_M_DELETE, "/api/v1/iso/zz", NULL, NULL);
    expect_err(404, "not_found");
    /* finish: 本体があって Content-Type が不正でも、先に 404 */
    iso_api_request_t rq;
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso/zz/finish", "text/plain", "{}");
    api_call_rq(&ENV, &rq);
    expect_err(404, "not_found");
    api_call(ISO_API_M_POST, "/api/v1/iso/A1B2C3D4/finish", "text/plain", "xxx");
    expect_err(404, "not_found");
    api_call(ISO_API_M_DELETE, "/api/v1/iso/mount1", NULL, NULL);
    expect_err(404, "not_found");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

void test_unknown_paths_and_methods_are_404_and_405(void) {
    api_call(ISO_API_M_GET, "/api/v1/iso/a1b2c3d4", NULL, NULL); /* 単体の GET は定義しない */
    expect_err(405, "method_not_allowed");
    TEST_ASSERT_EQUAL_STRING("PUT, DELETE", RSP.allow);
    api_call(ISO_API_M_GET, "/api/v1/iso/a1b2c3d4/finish", NULL, NULL);
    expect_err(405, "method_not_allowed");
    TEST_ASSERT_EQUAL_STRING("POST", RSP.allow);
    api_call(ISO_API_M_PUT, "/api/v1/iso", OCTET_CT, NULL);
    expect_err(405, "method_not_allowed");
    TEST_ASSERT_EQUAL_STRING("GET, POST", RSP.allow);
    TEST_ASSERT_TRUE(RSP.close_connection); /* PUT の拒否は、本体を読まずに閉じる */
    api_call(ISO_API_M_DELETE, "/api/v1/iso", NULL, NULL);
    expect_err(405, "method_not_allowed");
    TEST_ASSERT_FALSE(RSP.close_connection);
    api_call(ISO_API_M_GET, "/api/v1/iso/mount", NULL, NULL);
    expect_err(405, "method_not_allowed");
    TEST_ASSERT_EQUAL_STRING("POST", RSP.allow);
    api_call(ISO_API_M_GET, "/api/v1/iso/other/path/here", NULL, NULL);
    expect_err(404, "not_found");
    TEST_ASSERT_NULL(RSP.allow);
    api_call(ISO_API_M_PUT, "/api/v1/iso/a1b2c3d4/finish", OCTET_CT, NULL);
    expect_err(405, "method_not_allowed");
    TEST_ASSERT_TRUE(RSP.close_connection);
    api_call(ISO_API_M_PUT, "/nothing", OCTET_CT, NULL);
    expect_err(404, "not_found");
    TEST_ASSERT_TRUE(RSP.close_connection);
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

/* ---- 段 4 → 段 5 ------------------------------------------------------------------------- */

void test_step4_content_type_is_415_before_the_length_checks(void) {
    iso_api_request_t rq;
    /* PUT: Content-Length がなくても、Content-Type が違えば 415(411 ではない) */
    TEST_ASSERT_FALSE(put_pre(A, JSON_CT, false, 0, NULL));
    expect_err(415, "unsupported_media_type");
    TEST_ASSERT_TRUE(RSP.close_connection);
    TEST_ASSERT_FALSE(put_pre(A, NULL, true, 10, "0"));
    expect_err(415, "unsupported_media_type");
    TEST_ASSERT_FALSE(put_pre(A, "application/octet-streams", true, 10, "0"));
    expect_err(415, "unsupported_media_type");
    TEST_ASSERT_FALSE(put_pre(A, "text/plain", true, 10000000, "0")); /* 413 ではない */
    expect_err(415, "unsupported_media_type");
    /* begin: 大きすぎる本体でも、Content-Type が違えば 415 */
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso", "text/plain", "x");
    rq.content_length = 1000;
    api_call_rq(&ENV, &rq);
    expect_err(415, "unsupported_media_type");
    /* begin と mount は、Content-Type が必須(ヘッダーなしも 415) */
    api_call(ISO_API_M_POST, "/api/v1/iso", NULL, "{\"name\":\"a\",\"size\":34816}");
    expect_err(415, "unsupported_media_type");
    api_call(ISO_API_M_POST, "/api/v1/iso/mount", "application/octet-stream", "{\"id\":\"a1b2c3d4\"}");
    expect_err(415, "unsupported_media_type");
    /* 本体があるときの finish / eject / rescan */
    api_call(ISO_API_M_POST, "/api/v1/iso/" A "/finish", "text/plain", "{}");
    expect_err(415, "unsupported_media_type");
    api_call(ISO_API_M_POST, "/api/v1/iso/eject", NULL, "{}");
    expect_err(415, "unsupported_media_type");
    api_call(ISO_API_M_POST, "/api/v1/iso/rescan", OCTET_CT, "{}");
    expect_err(415, "unsupported_media_type");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

void test_step4_is_not_applied_to_requests_without_a_body_or_to_delete(void) {
    boot_ab();
    /* 本文のない POST(Content-Length なし、または 0)には、Content-Type の検査をしない */
    api_call(ISO_API_M_POST, "/api/v1/iso/eject", "text/plain", NULL);
    expect_rsp(200, "{\"result\":\"not_mounted\"}");
    iso_api_request_t rq;
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso/eject", "text/plain", NULL);
    rq.has_content_length = true;
    rq.content_length = 0;
    api_call_rq(&ENV, &rq);
    expect_rsp(200, "{\"result\":\"not_mounted\"}");
    api_call(ISO_API_M_POST, "/api/v1/iso/" A "/finish", "text/plain", NULL);
    expect_rsp(200, "{\"state\":\"ready\",\"sha256\":null}");
    api_call(ISO_API_M_POST, "/api/v1/iso/rescan", NULL, NULL);
    expect_rsp(200, "{\"imported\":0,\"skipped\":0,\"next_cursor\":null}");
    /* DELETE には適用しない(本文があっても) */
    api_rq(&rq, ISO_API_M_DELETE, "/api/v1/iso/" C, "text/plain", "junk");
    api_call_rq(&ENV, &rq);
    expect_err(404, "not_found");
    /* 本文があり、Content-Type が JSON(パラメーターつき)なら通る */
    api_call(ISO_API_M_POST, "/api/v1/iso/eject", "application/json; charset=utf-8", "{}");
    expect_rsp(200, "{\"result\":\"not_mounted\"}");
}

/* ---- 段 5 ----------------------------------------------------------------------------- */

void test_step5_put_needs_a_content_length(void) {
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, false, 0, "0"));
    expect_err(411, "length_required");
    TEST_ASSERT_TRUE(RSP.close_connection);
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, false, 0, NULL)); /* 411 が先(offset の欠落は 6) */
    expect_err(411, "length_required");
}

void test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed(void) {
    iso_api_request_t rq;
    /* 513 バイト: 本体が不正でも、先に 413 */
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso", JSON_CT, "garbage");
    rq.content_length = 513;
    api_call_rq(&ENV, &rq);
    expect_err(413, "payload_too_large");
    rq.content_length = 512; /* 512 は通る(本体の解析で 400) */
    api_call_rq(&ENV, &rq);
    expect_err(400, "invalid_request");
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso/mount", JSON_CT, "x");
    rq.content_length = 513;
    api_call_rq(&ENV, &rq);
    expect_err(413, "payload_too_large");
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso/" A "/finish", JSON_CT, "x");
    rq.content_length = 513;
    api_call_rq(&ENV, &rq);
    expect_err(413, "payload_too_large");
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso/eject", JSON_CT, "x");
    rq.content_length = 513;
    api_call_rq(&ENV, &rq);
    expect_err(413, "payload_too_large");
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso/rescan", JSON_CT, "x");
    rq.content_length = 513;
    api_call_rq(&ENV, &rq);
    expect_err(413, "payload_too_large");
    /* SD が使えなくても、413 が先 */
    api_set_sd(ISO_SD_NO_CARD);
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso", JSON_CT, "garbage");
    rq.content_length = 513;
    api_call_rq(&ENV, &rq);
    expect_err(413, "payload_too_large");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

void test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks(void) {
    /* chunk_size = 131072(公開ビューの値)。ちょうどは通り、1 つ多いと 413。offset の欠落(400)より前 */
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 131073, NULL));
    expect_err(413, "payload_too_large");
    TEST_ASSERT_TRUE(RSP.close_connection);
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 131072, NULL));
    expect_err(400, "invalid_request");
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 1, NULL));
    expect_err(400, "invalid_request");
    /* SD が使えなくても、413 が先 */
    api_set_sd(ISO_SD_ERROR);
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 131073, "0"));
    expect_err(413, "payload_too_large");
    /* 別の chunk_size */
    api_set_sd(ISO_SD_READY);
    SNAP.view.chunk_bytes = 1000;
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 1001, "0"));
    expect_err(413, "payload_too_large");
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 1000, "0"));
    expect_err(404, "not_found"); /* 段 5 を通り、段 8 まで進む */
    SNAP.view.chunk_bytes = 0; /* 未設定(サービスの起動前)は、既定の 128 KiB */
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 131073, "0"));
    expect_err(413, "payload_too_large");
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 131072, "0"));
    expect_err(404, "not_found");
}

/* ---- 段 6 → 段 7 ----------------------------------------------------------------------- */

void test_step6_format_errors_are_400_before_the_sd_check(void) {
    api_set_sd(ISO_SD_NO_CARD);
    api_post_json("/api/v1/iso", "{\"name\":\"a\"}");
    expect_err(400, "invalid_request");
    api_post_json("/api/v1/iso", "");
    expect_err(400, "invalid_request");
    api_post_json("/api/v1/iso/mount", "{\"id\":\"XYZ\"}");
    expect_err(400, "invalid_request");
    api_post_json("/api/v1/iso/mount", "{}");
    expect_err(400, "invalid_request");
    api_post_json("/api/v1/iso/rescan", "{\"cursor\":-1}");
    expect_err(400, "invalid_request");
    api_post_json("/api/v1/iso/eject", "{\"force\":1}"); /* eject の例外(200)より前 */
    expect_err(400, "invalid_request");
    api_post_json("/api/v1/iso/" A "/finish", "{\"x\":1}");
    expect_err(400, "invalid_request");
    /* PUT: offset の欠落・不正、Content-Length 0 */
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 10, NULL));
    expect_err(400, "invalid_request");
    TEST_ASSERT_TRUE(RSP.close_connection);
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 0, "0"));
    expect_err(400, "invalid_request");
    static const char *BAD_OFF[] = {"", "abc", "-1", "+1", "01", " 5", "5 ", "1.5", "0x10", "18446744073709551616", "99999999999999999999999"};
    for (size_t i = 0; i < sizeof(BAD_OFF) / sizeof(BAD_OFF[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(put_pre(A, OCTET_CT, true, 10, BAD_OFF[i]), BAD_OFF[i]);
        expect_err(400, "invalid_request");
    }
    /* 形式が正しければ、次の段(7)の 503 */
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 10, "0"));
    expect_err(503, "not_ready");
    TEST_ASSERT_TRUE(RSP.close_connection);
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

/* ---- 段 7 → 段 8 ----------------------------------------------------------------------- */

void test_step7_sd_not_ready_is_503_before_not_found(void) {
    static const iso_sd_state_t BAD[] = {ISO_SD_NO_CARD, ISO_SD_UNSUPPORTED_FS, ISO_SD_ERROR};
    for (size_t k = 0; k < 3; k++) {
        api_set_sd(BAD[k]);
        api_post_json("/api/v1/iso", "{\"name\":\"a\",\"size\":34816}");
        expect_err(503, "not_ready");
        api_post_json("/api/v1/iso/mount", "{\"id\":\"cccccccc\"}");
        expect_err(503, "not_ready");
        api_post_json("/api/v1/iso/rescan", "{}");
        expect_err(503, "not_ready");
        api_post_empty("/api/v1/iso/" C "/finish");
        expect_err(503, "not_ready");
        api_delete(C);
        expect_err(503, "not_ready");
        TEST_ASSERT_FALSE(put_pre(C, OCTET_CT, true, 10, "0"));
        expect_err(503, "not_ready");
        TEST_ASSERT_TRUE(RSP.close_connection);
    }
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
    /* GET は 200 */
    api_get();
    TEST_ASSERT_EQUAL_INT(200, RSP.status);
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

void test_step7_eject_is_the_exception_it_answers_200_not_mounted(void) {
    api_set_sd(ISO_SD_NO_CARD);
    api_post_json("/api/v1/iso/eject", "{}");
    expect_rsp(200, "{\"result\":\"not_mounted\"}");
    api_post_empty("/api/v1/iso/eject");
    expect_rsp(200, "{\"result\":\"not_mounted\"}");
    api_post_json("/api/v1/iso/eject", "{\"force\":true}");
    expect_rsp(200, "{\"result\":\"not_mounted\"}");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS); /* SD がなければ、何もしない */
    TEST_ASSERT_TRUE(RSP.log);
    TEST_ASSERT_EQUAL_STRING("eject", RSP.log_kind);
}

/* ---- 段 8 → 段 9 ----------------------------------------------------------------------- */

void test_step8_not_found_is_404_before_the_state_conflicts(void) {
    boot_ab();
    /* B がマウント中で、ターゲットが防止している */
    uint32_t b32 = 0;
    TEST_ASSERT_TRUE(iso_api_id_to_u32(B, &b32));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, b32, false));
    TEST_ASSERT_TRUE(iso_media_set_prevent(&MEDIA, true));
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" C "\"}"); /* 存在しない id: locked_by_target ではなく 404 */
    expect_err(404, "not_found");
    api_post_empty("/api/v1/iso/" C "/finish");
    expect_err(404, "not_found");
    api_delete(C);
    expect_err(404, "not_found");
    TEST_ASSERT_FALSE(put_pre(C, OCTET_CT, true, 10, "0"));
    expect_err(404, "not_found");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

void test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish(void) {
    boot_ab();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, A)); /* deleting(ジョブはまだ) */
    iso_core_publish(&CORE);
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 10, "0"));
    expect_err(404, "not_found");
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    expect_err(404, "not_found");
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_err(404, "not_found");
    /* DELETE は冪等(202) */
    api_delete(A);
    expect_rsp(202, "{\"state\":\"deleting\"}");
}

/* ---- 段 9: begin ------------------------------------------------------------------------ */

static void begin_upload(const char *name, uint64_t size, char id[ISO_ID_LEN + 1]) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(api_up(), name, strlen(name), size, NULL, id));
}

void test_step9_begin_conflicts_in_the_documented_order(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    char id[9];
    begin_upload("alpha2", 100000, id); /* 枠を使っている */
    /* 枠が使用中 と 同名 が同時: upload_in_progress */
    api_post_json("/api/v1/iso", "{\"name\":\"alpha\",\"size\":34816}");
    expect_err(409, "upload_in_progress");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
    /* 枠が使用中 と 507 が同時: upload_in_progress */
    api_post_json("/api/v1/iso", "{\"name\":\"zzz\",\"size\":17179869184}");
    iso_upload_set_free(api_up(), 1 * MIB, 1 * MIB);
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso", "{\"name\":\"zzz\",\"size\":34816}");
    expect_err(409, "upload_in_progress");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

void test_step9_begin_name_exists_then_too_many_images_then_507(void) {
    /* 16 個のイメージ(上限) */
    for (unsigned i = 0; i < ISO_MAX_IMAGES; i++) {
        char id[9], name[16];
        snprintf(id, sizeof(id), "%08x", 0x100u + i);
        snprintf(name, sizeof(name), "img%02u", i);
        api_add_ready(id, name, SZ2);
    }
    api_boot();
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, SNAP.view.count);
    /* 同名と上限が同時: name_exists が先 */
    api_post_json("/api/v1/iso", "{\"name\":\"img05\",\"size\":34816}");
    expect_err(409, "name_exists");
    /* 大文字小文字は別の名前(完全一致)なので、同名ではない → too_many_images */
    api_post_json("/api/v1/iso", "{\"name\":\"IMG05\",\"size\":34816}");
    expect_err(409, "too_many_images");
    /* 上限と 507 が同時: too_many_images が先 */
    iso_upload_set_free(api_up(), 1 * MIB, 1 * MIB);
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso", "{\"name\":\"fresh\",\"size\":34816}");
    expect_err(409, "too_many_images");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

void test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, A));
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso", "{\"name\":\"alpha\",\"size\":34816}"); /* deleting の名前は見えない */
    TEST_ASSERT_EQUAL_INT(201, RSP.status);
}

void test_step12_begin_fits_exactly_with_the_reserve(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    const uint64_t reserve = 64ull * 1048576ull; /* ISO_FREE_RESERVE_MIB = 64 MiB = 67108864 */
    iso_upload_set_free(api_up(), 10 * GIB, 40000 + reserve);
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso", "{\"name\":\"fits\",\"size\":40000}");
    TEST_ASSERT_EQUAL_INT(201, RSP.status);
}

void test_step12_begin_one_byte_short_is_507(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    const uint64_t reserve = 64ull * 1048576ull;
    iso_upload_set_free(api_up(), 10 * GIB, 40000 + reserve - 1);
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso", "{\"name\":\"fits\",\"size\":40000}");
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":67148863}"); /* 67108864 + 40000 - 1 */
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
    TEST_ASSERT_EQUAL_UINT(1, CAT.count); /* 何も作らない */
}

void test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space(void) {
    api_boot();
    iso_upload_set_free(api_up(), 50000, 10 * GIB);
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso", "{\"name\":\"fits\",\"size\":50001}");
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":10737418240}");
    /* size == total_bytes は、この検査では通る(空きの検査へ) */
    api_post_json("/api/v1/iso", "{\"name\":\"fits\",\"size\":50000}");
    TEST_ASSERT_EQUAL_INT(201, RSP.status);
}

void test_step12_begin_without_a_sampled_free_size_is_507_with_zero(void) {
    CORE.up.free_known = false;
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso", "{\"name\":\"x\",\"size\":34816}");
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":0}");
}

void test_step12_the_estimate_subtracts_the_remaining_of_unfinished_images_and_consumed_bytes(void) {
    /* 空き 10 GiB。途中のイメージ(uploading 100000 バイト、received 0)の残り 100000 と、採取後に書いた 5000 を引く */
    api_boot();
    char id[9];
    begin_upload("up", 100000, id);
    iso_upload_set_free(api_up(), 20 * GIB, 10 * GIB);
    api_up()->consumed_since = 5000;
    /* 途中のイメージは、枠が使用中なので begin できない。incomplete にしてから */
    iso_upload_idle_close(api_up(), id);
    iso_core_publish(&CORE);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, api_find(id)->state);
    api_post_json("/api/v1/iso", "{\"name\":\"big\",\"size\":10737418240}"); /* 10 GiB: 引いた後の空きに入らない */
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":10737313240}"); /* 10737418240 - 5000 - 100000 */
}

/* ---- 段 9: mount ------------------------------------------------------------------------- */

void test_step9_mount_image_not_ready_before_locked_by_target(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_incomplete(B, "beta", SZ2, PART_SMALL);
    api_boot();
    uint32_t a32 = 0;
    TEST_ASSERT_TRUE(iso_api_id_to_u32(A, &a32));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, a32, false));
    TEST_ASSERT_TRUE(iso_media_set_prevent(&MEDIA, true));
    /* B は incomplete かつ、A がロックされている: image_not_ready が先 */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" B "\"}");
    expect_err(409, "image_not_ready");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

void test_step9_mount_already_mounted_wins_over_the_lock(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_ready(B, "beta", SZ2);
    api_boot();
    uint32_t a32 = 0;
    TEST_ASSERT_TRUE(iso_api_id_to_u32(A, &a32));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, a32, false));
    TEST_ASSERT_TRUE(iso_media_set_prevent(&MEDIA, true));
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    expect_rsp(200, "{\"result\":\"already_mounted\",\"id\":\"aaaaaaa1\"}");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS); /* 状態だけで決まる。何も変えない */
    /* 別のイメージは、locked_by_target(force なし) */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" B "\"}");
    expect_err(409, "locked_by_target");
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" B "\",\"force\":false}");
    expect_err(409, "locked_by_target");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
    /* force なら差し替える */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" B "\",\"force\":true}");
    expect_rsp(200, "{\"result\":\"mounted\",\"id\":\"bbbbbbb2\"}");
}

void test_step9_mount_of_a_ready_image_without_a_table_is_image_not_ready(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot_no_index(); /* INDEX を流さない: 表示は finalizing */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, SNAP.view.images[0].state);
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    expect_err(409, "image_not_ready");
}

/* ---- 段 9: finish ------------------------------------------------------------------------ */

void test_step9_finish_size_mismatch_before_upload_in_progress(void) {
    api_add_incomplete(A, "alpha", SZ2, PART_SMALL); /* received 34816 < 69632 */
    api_add_incomplete(B, "beta", SZ2, SZ2);         /* received == size */
    api_boot();
    char id[9];
    begin_upload("live", 100000, id); /* 枠を使う */
    /* A: size_mismatch(received と size を返す) */
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_rsp(409, "{\"error\":\"size_mismatch\",\"received\":34816,\"size\":69632}");
    /* B: 全部あるが、別のイメージが枠を使っている */
    api_post_empty("/api/v1/iso/" B "/finish");
    expect_err(409, "upload_in_progress");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
    /* uploading のイメージ自身: received < size は size_mismatch */
    char path[64];
    snprintf(path, sizeof(path), "/api/v1/iso/%s/finish", id);
    api_post_empty(path);
    expect_rsp(409, "{\"error\":\"size_mismatch\",\"received\":0,\"size\":100000}");
}

void test_step9_finish_ready_and_invalid_are_answered_from_the_state(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_invalid(B, "beta", SZ2, ISO_INVALID_NOT_ISO);
    api_boot();
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_rsp(200, "{\"state\":\"ready\",\"sha256\":null}");
    api_post_empty("/api/v1/iso/" B "/finish");
    expect_err(422, "not_iso");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS); /* ロックなしで答える(busy にならない) */
    OL_FAIL = true;
    api_post_empty("/api/v1/iso/" A "/finish");
    TEST_ASSERT_EQUAL_INT(200, RSP.status);
    api_post_empty("/api/v1/iso/" B "/finish");
    TEST_ASSERT_EQUAL_INT(422, RSP.status);
}

/* ---- 段 9: PUT ------------------------------------------------------------------------- */

void test_step9_put_to_images_that_cannot_receive_is_not_uploading(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_invalid(B, "beta", SZ2, ISO_INVALID_CHECKSUM_MISMATCH);
    api_boot();
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 10, "69632")); /* offset が合っていても */
    expect_err(409, "not_uploading");
    TEST_ASSERT_TRUE(RSP.close_connection);
    TEST_ASSERT_FALSE(put_pre(B, OCTET_CT, true, 10, "0"));
    expect_err(409, "not_uploading");
    /* finalizing(ready で表がない) */
    TEST_ASSERT_EQUAL_INT(0, iso_core_boot(&CORE));
    iso_core_publish(&CORE);
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 10, "0"));
    expect_err(409, "not_uploading");
}

void test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch(void) {
    api_add_incomplete(A, "alpha", SZ2, PART_SMALL);
    api_boot();
    char id[9];
    begin_upload("live", 100000, id);
    /* A は incomplete で再開したいが、別のイメージが枠を使っている。offset も合っていない */
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 10, "5"));
    expect_err(409, "upload_in_progress");
    TEST_ASSERT_TRUE(RSP.close_connection);
    /* 枠の持ち主は、offset の判定へ */
    TEST_ASSERT_FALSE(put_pre(id, OCTET_CT, true, 10, "5"));
    expect_rsp(409, "{\"error\":\"offset_mismatch\",\"received\":0}");
}

void test_step10_offset_mismatch_before_exceeds_size_and_storage(void) {
    api_boot();
    char id[9];
    begin_upload("live", 100000, id);
    /* offset が合わず、チャンクが size を超える: offset_mismatch */
    TEST_ASSERT_FALSE(put_pre(id, OCTET_CT, true, 100000, "50000"));
    expect_rsp(409, "{\"error\":\"offset_mismatch\",\"received\":0}");
    TEST_ASSERT_FALSE(put_pre(id, OCTET_CT, true, 10, "1"));
    expect_rsp(409, "{\"error\":\"offset_mismatch\",\"received\":0}");
    put_all(id, 0, 1000, 1000);
    iso_core_publish(&CORE);
    TEST_ASSERT_FALSE(put_pre(id, OCTET_CT, true, 10, "0"));
    expect_rsp(409, "{\"error\":\"offset_mismatch\",\"received\":1000}");
    TEST_ASSERT_TRUE(put_pre(id, OCTET_CT, true, 10, "1000"));
    TEST_ASSERT_EQUAL_INT(0, RSP.status);
    TEST_ASSERT_EQUAL_STRING(id, RSP.put.id);
    TEST_ASSERT_EQUAL_UINT64(1000, RSP.put.offset);
    TEST_ASSERT_EQUAL_UINT32(10, RSP.put.len);
    TEST_ASSERT_FALSE(RSP.close_connection);
}

void test_step11_chunk_beyond_the_size_is_400_before_the_storage_check(void) {
    api_add_incomplete(A, "alpha", SZ2, PART_SMALL); /* received 34816。size 69632。残り 34816 */
    api_boot();
    iso_upload_set_free(api_up(), 1 * MIB, 1 * MIB); /* 空きが足りない(507 になりうる) */
    iso_core_publish(&CORE);
    /* offset == received、len が残りを 1 つ超える: 400。ちょうどなら、507(段 12) */
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 34817, "34816"));
    expect_err(400, "invalid_request");
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 34816, "34816"));
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":1013760}"); /* 1048576 - 34816 */
    TEST_ASSERT_TRUE(RSP.close_connection);
}

void test_step12_put_resume_storage_boundary(void) {
    api_add_incomplete(A, "alpha", SZ2, PART_SMALL);
    api_boot();
    const uint64_t remaining = SZ2 - PART_SMALL; /* 34816 */
    const uint64_t reserve = 64ull * 1048576ull;
    iso_upload_set_free(api_up(), 64 * GIB, remaining + reserve); /* ちょうど */
    iso_core_publish(&CORE);
    TEST_ASSERT_TRUE(put_pre(A, OCTET_CT, true, 100, "34816"));
    TEST_ASSERT_EQUAL_INT(0, RSP.status);
    iso_upload_set_free(api_up(), 64 * GIB, remaining + reserve - 1); /* 1 バイト足りない */
    iso_core_publish(&CORE);
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 100, "34816"));
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":67108863}"); /* 推定: (残り + 予備 - 1) - 残り */
    TEST_ASSERT_TRUE(RSP.close_connection);
    /* consumed_since も引く */
    iso_upload_set_free(api_up(), 64 * GIB, remaining + reserve);
    api_up()->consumed_since = 1;
    iso_core_publish(&CORE);
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 100, "34816"));
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":67108863}");
    /* 空きが未採取なら、0 */
    CORE.up.free_known = false;
    iso_core_publish(&CORE);
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 100, "34816"));
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":0}");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

void test_step12_put_to_an_uploading_image_never_checks_the_storage(void) {
    api_boot();
    char id[9];
    begin_upload("live", 100000, id);
    iso_upload_set_free(api_up(), 1 * MIB, 0); /* 空きなし(begin のときに確保済みとして数えている) */
    iso_core_publish(&CORE);
    TEST_ASSERT_TRUE(put_pre(id, OCTET_CT, true, 100, "0"));
    TEST_ASSERT_EQUAL_INT(0, RSP.status);
}

void test_the_resume_check_does_not_count_the_image_itself_twice(void) {
    /* A(incomplete、残り 34816)と B(incomplete、残り 34816)。空き = 残りの合計 + 予備 のとき、A の再開は、B の分を引いた後で足りる */
    api_add_incomplete(A, "alpha", SZ2, PART_SMALL);
    api_add_incomplete(B, "beta", SZ2, PART_SMALL);
    api_boot();
    const uint64_t reserve = 64ull * 1048576ull;
    iso_upload_set_free(api_up(), 64 * GIB, 2 * 34816ull + reserve);
    iso_core_publish(&CORE);
    TEST_ASSERT_TRUE(put_pre(A, OCTET_CT, true, 100, "34816"));
    iso_upload_set_free(api_up(), 64 * GIB, 2 * 34816ull + reserve - 1);
    iso_core_publish(&CORE);
    TEST_ASSERT_FALSE(put_pre(A, OCTET_CT, true, 100, "34816"));
    TEST_ASSERT_EQUAL_INT(507, RSP.status);
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":67108863}"); /* (2 * 34816 + 予備 - 1) - 2 * 34816 */
}

/* ---- 段 9: delete / rescan ---------------------------------------------------------------------- */

void test_step9_delete_of_the_mounted_image_is_409(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_ready(B, "beta", SZ2);
    api_boot();
    uint32_t a32 = 0;
    TEST_ASSERT_TRUE(iso_api_id_to_u32(A, &a32));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, a32, false));
    api_delete(A);
    expect_err(409, "mounted");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, api_find(A)->state);
    api_delete(B);
    expect_rsp(202, "{\"state\":\"deleting\"}");
}

void test_step9_mounted_is_reported_before_crosslinked(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    /* import/ に、A/p000 と同じ先頭クラスタのファイル(rename の途中の電源断) */
    uint32_t cl = mock_fs_first_cluster(&FS, "/iso/" A "/p000");
    TEST_ASSERT_TRUE(cl != 0);
    static const uint8_t junk[4] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/x.iso", junk, sizeof(junk), cl));
    uint32_t a32 = 0;
    TEST_ASSERT_TRUE(iso_api_id_to_u32(A, &a32));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, a32, false));
    api_delete(A);
    expect_err(409, "mounted");
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&MEDIA, false));
    api_delete(A);
    expect_err(409, "crosslinked");
}

void test_step9_rescan_during_an_upload_is_409(void) {
    api_boot();
    char id[9];
    begin_upload("live", 100000, id);
    api_post_json("/api/v1/iso/rescan", "{}");
    expect_err(409, "upload_in_progress");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

/* ---- 段 12 → 段 13 ---------------------------------------------------------------------- */

void test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    OL_FAIL = true; /* 取れない状態でも、拒否は 503 busy ではなく、本来のエラー */
    api_post_json("/api/v1/iso", "{\"name\":\"alpha\",\"size\":34816}");
    expect_err(409, "name_exists");
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" C "\"}");
    expect_err(404, "not_found");
    api_delete(C);
    expect_err(404, "not_found");
    api_post_empty("/api/v1/iso/" C "/finish");
    expect_err(404, "not_found");
    TEST_ASSERT_FALSE(put_pre(C, OCTET_CT, true, 10, "0"));
    expect_err(404, "not_found");
    api_post_json("/api/v1/iso/rescan", "{\"cursor\":-1}");
    expect_err(400, "invalid_request");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
}

void test_step13_busy_when_the_op_lock_cannot_be_taken(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_ready(B, "beta", SZ2);
    api_boot();
    OL_FAIL = true;
    struct {
        iso_api_method_t m;
        const char *path;
        const char *ct;
        const char *body;
    } T[] = {
        {ISO_API_M_POST, "/api/v1/iso", JSON_CT, "{\"name\":\"new\",\"size\":34816}"},
        {ISO_API_M_POST, "/api/v1/iso/mount", JSON_CT, "{\"id\":\"" A "\"}"},
        {ISO_API_M_POST, "/api/v1/iso/eject", NULL, NULL},
        {ISO_API_M_POST, "/api/v1/iso/rescan", NULL, NULL},
        {ISO_API_M_DELETE, "/api/v1/iso/" B, NULL, NULL},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        OL_TRY_CALLS = 0;
        api_call(T[i].m, T[i].path, T[i].ct, T[i].body);
        expect_err(503, "busy");
        TEST_ASSERT_TRUE_MESSAGE(RSP.has_retry_after, T[i].path);
        TEST_ASSERT_EQUAL_UINT32(1, RSP.retry_after_s);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, OL_TRY_CALLS, T[i].path);
        TEST_ASSERT_EQUAL_UINT32(ISO_OP_LOCK_WAIT_MS, OL_LAST_WAIT);
    }
    /* 何も変わっていない */
    TEST_ASSERT_EQUAL_UINT(2, CAT.count);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, api_find(B)->state);
    TEST_ASSERT_FALSE(iso_media_is_loaded(&MEDIA));
}

void test_step13_busy_for_finish_and_put_commit(void) {
    api_add_incomplete(A, "alpha", SZ2, SZ2);
    api_boot();
    OL_FAIL = true;
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_err(503, "busy");
    TEST_ASSERT_TRUE(RSP.has_retry_after);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, api_find(A)->state);
    /* PUT の commit */
    OL_FAIL = false;
    char id[9];
    begin_upload("live", 100000, id);
    char off[8] = "0";
    TEST_ASSERT_TRUE(put_pre(id, OCTET_CT, true, 10, off));
    iso_api_put_t put = RSP.put;
    OL_FAIL = true;
    uint8_t data[10] = {0};
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &put, data, 10, &RSP);
    expect_err(503, "busy");
    TEST_ASSERT_TRUE(RSP.has_retry_after);
    TEST_ASSERT_EQUAL_UINT32(1, RSP.retry_after_s);
    TEST_ASSERT_EQUAL_UINT64(0, api_find(id)->received);
}

void test_the_op_lock_wait_can_be_configured(void) {
    api_boot();
    ENV.op_lock_wait_ms = 7;
    api_post_json("/api/v1/iso/rescan", "{}");
    TEST_ASSERT_EQUAL_UINT32(7, OL_LAST_WAIT);
    ENV.op_lock_wait_ms = 0; /* 既定 */
    api_post_json("/api/v1/iso/rescan", "{}");
    TEST_ASSERT_EQUAL_UINT32(ISO_OP_LOCK_WAIT_MS, OL_LAST_WAIT);
    TEST_ASSERT_EQUAL_UINT32(20, ISO_OP_LOCK_WAIT_MS);
}

void test_get_and_put_precheck_do_not_take_the_op_lock(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    api_get();
    TEST_ASSERT_EQUAL_INT(200, RSP.status);
    TEST_ASSERT_TRUE(SL_CALLS > 0);
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
    OL_FAIL = true;
    api_get(); /* busy にならない */
    TEST_ASSERT_EQUAL_INT(200, RSP.status);
    TEST_ASSERT_EQUAL_INT(0, SL_DEPTH);
}

/* ---- 起動していないサービス ------------------------------------------------------------------ */

void test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks(void) {
    iso_api_request_t rq;
    iso_api_response_t rsp;
    char buf[ISO_API_RESPONSE_MAX];
    struct {
        iso_api_method_t m;
        const char *path;
        const char *ct;
        const char *body;
        int status;
        const char *code;
    } T[] = {
        {ISO_API_M_POST, "/api/v1/iso", JSON_CT, "{\"name\":\"a\",\"size\":34816}", 503, "not_ready"},
        {ISO_API_M_POST, "/api/v1/iso", JSON_CT, "{\"name\":\"a\"}", 400, "invalid_request"},
        {ISO_API_M_POST, "/api/v1/iso/mount", JSON_CT, "{\"id\":\"a1b2c3d4\"}", 503, "not_ready"},
        {ISO_API_M_POST, "/api/v1/iso/mount", JSON_CT, "{\"id\":\"a1b2c3d\"}", 400, "invalid_request"},
        {ISO_API_M_POST, "/api/v1/iso/rescan", NULL, NULL, 503, "not_ready"},
        {ISO_API_M_POST, "/api/v1/iso/a1b2c3d4/finish", NULL, NULL, 503, "not_ready"},
        {ISO_API_M_DELETE, "/api/v1/iso/a1b2c3d4", NULL, NULL, 503, "not_ready"},
        {ISO_API_M_DELETE, "/api/v1/iso/zz", NULL, NULL, 404, "not_found"},
        {ISO_API_M_GET, "/api/v1/iso/a1b2c3d4", NULL, NULL, 405, "method_not_allowed"},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        api_rq(&rq, T[i].m, T[i].path, T[i].ct, T[i].body);
        iso_api_response_init(&rsp, buf, sizeof(buf));
        iso_api_handle(NULL, &rq, &rsp);
        char want[64];
        snprintf(want, sizeof(want), "{\"error\":\"%s\"}", T[i].code);
        TEST_ASSERT_EQUAL_INT_MESSAGE(T[i].status, rsp.status, T[i].path);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want, rsp.body, T[i].path);
    }
    /* eject は 200 not_mounted、PUT は 503 not_ready(本体を読まない) */
    api_rq(&rq, ISO_API_M_POST, "/api/v1/iso/eject", NULL, NULL);
    iso_api_response_init(&rsp, buf, sizeof(buf));
    iso_api_handle(NULL, &rq, &rsp);
    TEST_ASSERT_EQUAL_INT(200, rsp.status);
    TEST_ASSERT_EQUAL_STRING("{\"result\":\"not_mounted\"}", rsp.body);
    api_rq(&rq, ISO_API_M_PUT, "/api/v1/iso/a1b2c3d4", OCTET_CT, NULL);
    rq.has_content_length = true;
    rq.content_length = 5;
    rq.upload_offset = "0";
    iso_api_response_init(&rsp, buf, sizeof(buf));
    TEST_ASSERT_FALSE(iso_api_put_precheck(NULL, &rq, &rsp));
    TEST_ASSERT_EQUAL_INT(503, rsp.status);
    TEST_ASSERT_TRUE(rsp.close_connection);
    /* commit は not_ready */
    iso_api_put_t put;
    memset(&put, 0, sizeof(put));
    put.accepted = true;
    put.len = 1;
    iso_api_response_init(&rsp, buf, sizeof(buf));
    iso_api_put_commit(NULL, &put, "x", 1, &rsp);
    TEST_ASSERT_EQUAL_INT(503, rsp.status);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"not_ready\"}", rsp.body);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_step3_an_invalid_id_in_the_path_is_404_before_everything_else);
    RUN_TEST(test_unknown_paths_and_methods_are_404_and_405);
    RUN_TEST(test_step4_content_type_is_415_before_the_length_checks);
    RUN_TEST(test_step4_is_not_applied_to_requests_without_a_body_or_to_delete);
    RUN_TEST(test_step5_put_needs_a_content_length);
    RUN_TEST(test_step5_json_body_is_at_most_512_bytes_before_the_body_is_parsed);
    RUN_TEST(test_step5_put_content_length_over_chunk_size_is_413_before_the_format_checks);
    RUN_TEST(test_step6_format_errors_are_400_before_the_sd_check);
    RUN_TEST(test_step7_sd_not_ready_is_503_before_not_found);
    RUN_TEST(test_step7_eject_is_the_exception_it_answers_200_not_mounted);
    RUN_TEST(test_step8_not_found_is_404_before_the_state_conflicts);
    RUN_TEST(test_step8_a_deleting_image_does_not_exist_for_put_mount_and_finish);
    RUN_TEST(test_step9_begin_conflicts_in_the_documented_order);
    RUN_TEST(test_step9_begin_name_exists_then_too_many_images_then_507);
    RUN_TEST(test_step9_deleting_images_do_not_hold_their_name_but_do_count_toward_the_limit);
    RUN_TEST(test_step12_begin_fits_exactly_with_the_reserve);
    RUN_TEST(test_step12_begin_one_byte_short_is_507);
    RUN_TEST(test_step12_begin_larger_than_the_card_is_507_even_with_enough_free_space);
    RUN_TEST(test_step12_begin_without_a_sampled_free_size_is_507_with_zero);
    RUN_TEST(test_step12_the_estimate_subtracts_the_remaining_of_unfinished_images_and_consumed_bytes);
    RUN_TEST(test_step9_mount_image_not_ready_before_locked_by_target);
    RUN_TEST(test_step9_mount_already_mounted_wins_over_the_lock);
    RUN_TEST(test_step9_mount_of_a_ready_image_without_a_table_is_image_not_ready);
    RUN_TEST(test_step9_finish_size_mismatch_before_upload_in_progress);
    RUN_TEST(test_step9_finish_ready_and_invalid_are_answered_from_the_state);
    RUN_TEST(test_step9_put_to_images_that_cannot_receive_is_not_uploading);
    RUN_TEST(test_step9_put_resume_blocked_by_the_slot_before_offset_mismatch);
    RUN_TEST(test_step10_offset_mismatch_before_exceeds_size_and_storage);
    RUN_TEST(test_step11_chunk_beyond_the_size_is_400_before_the_storage_check);
    RUN_TEST(test_step12_put_resume_storage_boundary);
    RUN_TEST(test_step12_put_to_an_uploading_image_never_checks_the_storage);
    RUN_TEST(test_the_resume_check_does_not_count_the_image_itself_twice);
    RUN_TEST(test_step9_delete_of_the_mounted_image_is_409);
    RUN_TEST(test_step9_mounted_is_reported_before_crosslinked);
    RUN_TEST(test_step9_rescan_during_an_upload_is_409);
    RUN_TEST(test_a_rejection_in_steps_3_to_12_never_asks_for_the_op_lock);
    RUN_TEST(test_step13_busy_when_the_op_lock_cannot_be_taken);
    RUN_TEST(test_step13_busy_for_finish_and_put_commit);
    RUN_TEST(test_the_op_lock_wait_can_be_configured);
    RUN_TEST(test_get_and_put_precheck_do_not_take_the_op_lock);
    RUN_TEST(test_a_service_that_is_not_started_answers_not_ready_after_the_format_checks);
    return UNITY_END();
}

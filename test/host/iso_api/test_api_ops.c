/*
 * 段 13(処理)と、結果の対応表(WEB_API.md 8.2〜8.8)。実物の iso_core / iso_upload / iso_catalog / iso_media と、モックの fs を使う。
 * 判定(段 7〜12)は公開ビューから行うので、判定と処理の間の競合(ビューが古い)は、古いビューのコピー(use_stale)と OL_HOOK で再現する。
 * 期待値は手書き。
 */
#include "api_common.h"

void setUp(void) { fx_api_setup(); }
void tearDown(void) { fx_api_teardown(); }

#define A "aaaaaaa1"
#define B "bbbbbbb2"

static iso_api_snapshot_t STALE;
static void use_stale(void) {
    STALE = SNAP;
    ENV.snapshot = &STALE;
}
static void use_live(void) { ENV.snapshot = &SNAP; }

static void hexof(const uint8_t sha[32], char out[65]) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = H[sha[i] >> 4];
        out[2 * i + 1] = H[sha[i] & 15];
    }
    out[64] = '\0';
}

static void begin_ok(const char *name, uint64_t size, char id[ISO_ID_LEN + 1]) {
    char body[160];
    snprintf(body, sizeof(body), "{\"name\":\"%s\",\"size\":%llu}", name, (unsigned long long)size);
    api_post_json("/api/v1/iso", body);
    TEST_ASSERT_EQUAL_INT(201, RSP.status);
    memcpy(id, RSP.log_id, ISO_ID_LEN + 1);
}

/* ---- begin ----------------------------------------------------------------------------- */

void test_begin_accepted(void) {
    api_post_json("/api/v1/iso", "{\"name\":\"ubuntu\",\"size\":100000}");
    expect_rsp(201, "{\"id\":\"11110001\",\"chunk_size\":131072}");
    TEST_ASSERT_TRUE(RSP.log);
    TEST_ASSERT_EQUAL_STRING("begin", RSP.log_kind);
    TEST_ASSERT_EQUAL_STRING("ok", RSP.log_reason);
    TEST_ASSERT_EQUAL_STRING("11110001", RSP.log_id);
    TEST_ASSERT_TRUE(RSP.log_has_size);
    TEST_ASSERT_EQUAL_UINT64(100000, RSP.log_size);
    TEST_ASSERT_EQUAL_INT(1, OL_CALLS);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, api_find("11110001")->state);
    TEST_ASSERT_EQUAL_STRING("ubuntu", api_find("11110001")->meta.name);
    /* GET に出る(アップロードの枠も) */
    api_get();
    expect_rsp(200,
               "{\"sd\":{\"state\":\"ready\",\"total_bytes\":68719476736,\"free_bytes\":64424509440},\"images\":["
               "{\"id\":\"11110001\",\"name\":\"ubuntu\",\"size\":100000,\"state\":\"uploading\",\"sha256\":null,\"received\":0,\"invalid_reason\":null}],"
               "\"mounted\":null,\"target\":{\"locked\":false,\"ejected_by_target\":false},\"chunk_size\":131072,\"hidden_images\":0,"
               "\"upload\":{\"id\":\"11110001\",\"state\":\"receiving\"}}");
    TEST_ASSERT_FALSE(RSP.log);
}

void test_begin_keeps_the_declared_sha256_in_lower_case_bytes(void) {
    api_post_json("/api/v1/iso", "{\"name\":\"x\",\"size\":34816,\"sha256\":\"00112233445566778899AABBCCDDEEFF00112233445566778899aabbccddeeff\"}");
    TEST_ASSERT_EQUAL_INT(201, RSP.status);
    const iso_catalog_entry_t *e = api_find("11110001");
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_DECLARED, e->meta.sha_flag);
    for (int i = 0; i < 32; i++) TEST_ASSERT_EQUAL_UINT8((uint8_t)((i % 16) * 0x11), e->meta.sha256[i]);
}

void test_begin_without_sha256_has_no_declared_value(void) {
    api_post_json("/api/v1/iso", "{\"name\":\"x\",\"size\":34816}");
    TEST_ASSERT_EQUAL_INT(201, RSP.status);
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_NONE, api_find("11110001")->meta.sha_flag);
}

void test_begin_accepts_utf8_names_and_unicode_escapes(void) {
    api_post_json("/api/v1/iso", "{\"name\":\"caf\\u00e9 \\ud83d\\ude00\",\"size\":34816}");
    TEST_ASSERT_EQUAL_INT(201, RSP.status);
    TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9 \xF0\x9F\x98\x80", api_find("11110001")->meta.name);
    /* 同じ名前を、生の UTF-8 で: 同名(バイト列の完全一致) */
    iso_upload_abort(api_up(), "11110001");
    api_post_json("/api/v1/iso", "{\"name\":\"caf\xC3\xA9 \xF0\x9F\x98\x80\",\"size\":34816}");
    expect_err(409, "name_exists");
    /* 正規化はしない: e + 結合文字は、別の名前 */
    api_post_json("/api/v1/iso", "{\"name\":\"cafe\xCC\x81 \xF0\x9F\x98\x80\",\"size\":34816}");
    TEST_ASSERT_EQUAL_INT(201, RSP.status);
}

void test_begin_name_matches_are_case_sensitive(void) {
    api_add_ready(A, "Alpha", SZ2);
    api_boot();
    api_post_json("/api/v1/iso", "{\"name\":\"alpha\",\"size\":34816}");
    TEST_ASSERT_EQUAL_INT(201, RSP.status);
}

/* 判定(ビュー)が古いときの、処理の側の対応 */
void test_begin_exec_mapping_when_the_view_is_stale(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    use_stale(); /* 古い: 枠は空き、名前は alpha だけ、空きは十分 */
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(api_up(), "other", 5, 100000, NULL, id));
    api_post_json("/api/v1/iso", "{\"name\":\"third\",\"size\":34816}");
    expect_err(409, "upload_in_progress");
    TEST_ASSERT_EQUAL_STRING("begin", RSP.log_kind);
    iso_upload_abort(api_up(), id);
    api_post_json("/api/v1/iso", "{\"name\":\"alpha\",\"size\":34816}");
    expect_err(409, "name_exists");
    api_post_json("/api/v1/iso", "{\"name\":\"other\",\"size\":34816}"); /* 途中のイメージの名前も */
    expect_err(409, "name_exists");
    /* 空きのキャッシュが小さくなった */
    iso_upload_set_free(api_up(), 64 * GIB, 1000);
    api_post_json("/api/v1/iso", "{\"name\":\"fresh\",\"size\":34816}");
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":0}");
}

void test_begin_exec_too_many_images_when_the_view_is_stale(void) {
    for (unsigned i = 0; i < ISO_MAX_IMAGES; i++) {
        char id[9], name[16];
        snprintf(id, sizeof(id), "%08x", 0x100u + i);
        snprintf(name, sizeof(name), "img%02u", i);
        api_add_ready(id, name, SZ2);
    }
    api_boot();
    SNAP.view.count = ISO_MAX_IMAGES - 1; /* 古いビュー */
    api_post_json("/api/v1/iso", "{\"name\":\"fresh\",\"size\":34816}");
    expect_err(409, "too_many_images");
}

void test_begin_io_errors(void) {
    mock_fs_error_at(&FS, 1, ISO_ERR_IO); /* mkdir */
    api_post_json("/api/v1/iso", "{\"name\":\"x\",\"size\":34816}");
    expect_err(503, "sd_unavailable");
    TEST_ASSERT_FALSE(RSP.has_retry_after);
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    mock_fs_reset_counter(&FS);
    mock_fs_error_at(&FS, 2, ISO_ERR_NO_SPACE); /* meta.a の書き込みで、空きなし */
    api_post_json("/api/v1/iso", "{\"name\":\"x\",\"size\":34816}");
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":64424509440}");
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_EQUAL_INT(0, OL_DEPTH);
}

void test_begin_exhausted_ids_are_a_retryable_503(void) {
    static const uint32_t SAME[1] = {0x11110001u};
    mock_rnd_set(&RND, SAME, 1);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/11110001")); /* 同じ id のディレクトリがすでにある */
    api_post_json("/api/v1/iso", "{\"name\":\"x\",\"size\":34816}");
    expect_err(503, "busy");
    TEST_ASSERT_TRUE(RSP.has_retry_after);
    TEST_ASSERT_EQUAL_UINT32(1, RSP.retry_after_s);
}

/* ---- PUT ------------------------------------------------------------------------------- */

/* パーツ p000 の、メモリ上(未同期を含む)の中身が、画像の先頭 n バイトと一致する(パーツは 1 つ) */
static void expect_mem_prefix(const char *id, uint64_t n) {
    char path[ISO_PATH_MAX];
    snprintf(path, sizeof(path), "/iso/%s/p000", id);
    TEST_ASSERT_EQUAL_INT64((int64_t)n, mock_fs_mem_size(&FS, path));
    size_t len = 0;
    const uint8_t *d = mock_fs_data(&FS, path, &len);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT64(n, len);
    for (uint64_t i = 0; i < n; i++) {
        if (d[i] != gen_byte(i)) TEST_FAIL_MESSAGE("part content differs from the source");
    }
}

void test_put_flow_two_chunks(void) {
    char id[9];
    begin_ok("img", 100000, id);
    api_put_chunk(id, 0, 50000);
    expect_rsp(200, "{\"received\":50000}");
    TEST_ASSERT_FALSE(RSP.log); /* チャンクごとのログは出さない */
    api_put_chunk(id, 50000, 50000);
    expect_rsp(200, "{\"received\":100000}");
    TEST_ASSERT_EQUAL_UINT64(100000, api_find(id)->received);
    expect_mem_prefix(id, 100000);
    /* 古い offset は 409(何も書かない) */
    api_put_chunk(id, 50000, 100);
    expect_rsp(409, "{\"error\":\"offset_mismatch\",\"received\":100000}");
    TEST_ASSERT_TRUE(RSP.close_connection);
    /* size を超えるチャンクは 400(offset が合っていても) */
    api_put_chunk(id, 100000, 1);
    expect_err(400, "invalid_request");
    expect_mem_prefix(id, 100000);
}

void test_put_resume_of_an_incomplete_image(void) {
    api_add_incomplete(A, "alpha", SZ2, PART_SMALL);
    api_boot();
    api_put_chunk(A, PART_SMALL, 1000);
    expect_rsp(200, "{\"received\":35816}");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, api_find(A)->state); /* incomplete -> uploading */
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"upload\":{\"id\":\"aaaaaaa1\",\"state\":\"receiving\"}"));
}

void test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck(void) {
    char id[9];
    begin_ok("img", 34816, id);
    uint8_t data[100];
    memset(data, 7, sizeof(data));
    char off[8] = "0";
    /* offset_mismatch: 別のチャンクが先に入った */
    TEST_ASSERT_TRUE(api_put_pre(&ENV, id, OCTET_CT, true, 100, off));
    iso_api_put_t put = RSP.put;
    put_all(id, 0, 500, 500);
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &put, data, 100, &RSP);
    expect_rsp(409, "{\"error\":\"offset_mismatch\",\"received\":500}");
    /* not_uploading: finish が先に受理された */
    put_all(id, 500, 34816, 20000);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL));
    put.offset = 34816;
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &put, data, 100, &RSP);
    expect_err(409, "not_uploading");
    /* not_found: 削除が先に受理された */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, id));
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &put, data, 100, &RSP);
    expect_err(404, "not_found");
}

void test_commit_upload_in_progress_for_a_resume_when_another_upload_started(void) {
    api_add_incomplete(A, "alpha", SZ2, PART_SMALL);
    api_boot();
    char off[8] = "34816";
    TEST_ASSERT_TRUE(api_put_pre(&ENV, A, OCTET_CT, true, 100, off));
    iso_api_put_t put = RSP.put;
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(api_up(), "live", 4, 100000, NULL, id));
    uint8_t data[100] = {0};
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &put, data, 100, &RSP);
    expect_err(409, "upload_in_progress");
}

void test_commit_with_a_fabricated_put_maps_the_remaining_results(void) {
    char id[9];
    begin_ok("img", 34816, id);
    static uint8_t big[131073];
    memset(big, 1, sizeof(big));
    iso_api_put_t put;
    memset(&put, 0, sizeof(put));
    put.accepted = true;
    memcpy(put.id, id, sizeof(put.id));
    /* 413: チャンクが chunk_size を超える */
    put.len = 131073;
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &put, big, 131073, &RSP);
    expect_err(413, "payload_too_large");
    /* 400: チャンクが size を超える */
    put.len = 34817;
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &put, big, 34817, &RSP);
    expect_err(400, "invalid_request");
    TEST_ASSERT_EQUAL_UINT64(0, api_find(id)->received);
}

void test_commit_507_when_the_resume_no_longer_fits(void) {
    api_add_incomplete(A, "alpha", SZ2, PART_SMALL);
    api_boot();
    char off[8] = "34816";
    TEST_ASSERT_TRUE(api_put_pre(&ENV, A, OCTET_CT, true, 100, off));
    iso_api_put_t put = RSP.put;
    iso_upload_set_free(api_up(), 64 * GIB, 1000); /* 中断中に、別の何かが空きを使った */
    uint8_t data[100] = {0};
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &put, data, 100, &RSP);
    expect_rsp(507, "{\"error\":\"insufficient_storage\",\"free_bytes\":0}");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, api_find(A)->state); /* 何も変わらない */
}

void test_commit_write_failures(void) {
    char id[9];
    begin_ok("img", 100000, id);
    mock_fs_fail_write(&FS, 1, 0, ISO_ERR_IO);
    api_put_chunk(id, 0, 1000);
    expect_err(503, "sd_unavailable");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, api_find(id)->state); /* 取り消されて、再開できる */
    TEST_ASSERT_EQUAL_UINT64(0, api_find(id)->received);
    mock_fs_fail_write(&FS, 0, 0, 0);
    api_put_chunk(id, 0, 1000); /* 再開 */
    expect_rsp(200, "{\"received\":1000}");
    mock_fs_fail_write(&FS, 1, 0, ISO_ERR_NO_SPACE); /* 書き込み中に「空きなし」 */
    api_put_chunk(id, 1000, 1000);
    TEST_ASSERT_EQUAL_INT(507, RSP.status);
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"error\":\"insufficient_storage\",\"free_bytes\":"));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, api_find(id)->state);
}

static void hook_sd_gone(void) {
    SD_STATE = ISO_SD_NO_CARD;
    iso_core_publish(&CORE);
}

void test_commit_rechecks_the_sd_after_taking_the_op_lock(void) {
    char id[9];
    begin_ok("img", 100000, id);
    char off[8] = "0";
    TEST_ASSERT_TRUE(api_put_pre(&ENV, id, OCTET_CT, true, 100, off));
    iso_api_put_t put = RSP.put;
    OL_HOOK = hook_sd_gone;
    uint8_t data[100] = {0};
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &put, data, 100, &RSP);
    expect_err(503, "not_ready");
    TEST_ASSERT_EQUAL_UINT64(0, api_find(id)->received); /* 何も書かない */
    TEST_ASSERT_EQUAL_INT(0, OL_DEPTH);                  /* ロックを放した */
}

void test_commit_rejects_bad_arguments(void) {
    char id[9];
    begin_ok("img", 100000, id);
    OL_TRY_CALLS = 0;
    uint8_t data[10] = {0};
    iso_api_put_t ok;
    memset(&ok, 0, sizeof(ok));
    ok.accepted = true;
    memcpy(ok.id, id, sizeof(ok.id));
    ok.len = 10;
    iso_api_put_t no = ok;
    no.accepted = false;
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, NULL, data, 10, &RSP);
    expect_err(400, "invalid_request");
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &no, data, 10, &RSP);
    expect_err(400, "invalid_request");
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &ok, NULL, 10, &RSP);
    expect_err(400, "invalid_request");
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &ok, data, 9, &RSP); /* put->len と違う */
    expect_err(400, "invalid_request");
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &ok, data, 11, &RSP);
    expect_err(400, "invalid_request");
    iso_api_put_t zero = ok;
    zero.len = 0;
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    iso_api_put_commit(&ENV, &zero, data, 0, &RSP);
    expect_err(400, "invalid_request");
    TEST_ASSERT_EQUAL_INT(0, OL_TRY_CALLS);
    TEST_ASSERT_EQUAL_UINT64(0, api_find(id)->received);
}

void test_put_precheck_rejects_a_request_that_is_not_a_put(void) {
    iso_api_request_t rq;
    api_rq(&rq, ISO_API_M_DELETE, "/api/v1/iso/" A, NULL, NULL);
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    TEST_ASSERT_FALSE(iso_api_put_precheck(&ENV, &rq, &RSP));
    expect_err(500, "internal_error");
    TEST_ASSERT_TRUE(RSP.close_connection);
    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
    TEST_ASSERT_FALSE(iso_api_put_precheck(&ENV, NULL, &RSP));
    expect_err(500, "internal_error");
    TEST_ASSERT_FALSE(iso_api_put_precheck(&ENV, &rq, NULL));
}

/* ---- finish ----------------------------------------------------------------------------- */

static void upload_all(const char *id, uint64_t size) {
    for (uint64_t off = 0; off < size;) {
        size_t n = (size - off < 30000) ? (size_t)(size - off) : 30000;
        api_put_chunk(id, off, n);
        TEST_ASSERT_EQUAL_INT(200, RSP.status);
        off += n;
    }
}

void test_finish_flow_with_a_declared_sha256(void) {
    uint8_t sha[32];
    char hex[65];
    gen_sha(SZ2, sha);
    hexof(sha, hex);
    char body[256];
    snprintf(body, sizeof(body), "{\"name\":\"win\",\"size\":%llu,\"sha256\":\"%s\"}", (unsigned long long)SZ2, hex);
    api_post_json("/api/v1/iso", body);
    TEST_ASSERT_EQUAL_INT(201, RSP.status);
    const char *id = "11110001";
    upload_all(id, SZ2);
    /* finish: 受理 */
    api_post_empty("/api/v1/iso/11110001/finish");
    expect_rsp(202, "{\"state\":\"finalizing\"}");
    TEST_ASSERT_EQUAL_STRING("finish", RSP.log_kind);
    TEST_ASSERT_EQUAL_STRING("11110001", RSP.log_id);
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
    /* 冪等(何度呼んでもよい。ジョブは 1 つ) */
    api_post_json("/api/v1/iso/11110001/finish", "{}");
    expect_rsp(202, "{\"state\":\"finalizing\"}");
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
    /* GET: finalizing、枠は finalizing */
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"state\":\"finalizing\",\"sha256\":null,\"received\":69632"));
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"upload\":{\"id\":\"11110001\",\"state\":\"finalizing\"}"));
    /* PUT は拒否 */
    TEST_ASSERT_FALSE(api_put_pre(&ENV, id, OCTET_CT, true, 10, "69632"));
    expect_err(409, "not_uploading");
    /* ワーカーが終わらせる */
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, api_find(id)->state);
    iso_core_publish(&CORE);
    api_get();
    char want[64 + 80];
    snprintf(want, sizeof(want), "\"state\":\"ready\",\"sha256\":\"%s\",\"received\":69632", hex);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(RSP.body, want), RSP.body);
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"upload\":null"));
    /* ready: 200 + sha256 */
    api_post_empty("/api/v1/iso/11110001/finish");
    char want2[160];
    snprintf(want2, sizeof(want2), "{\"state\":\"ready\",\"sha256\":\"%s\"}", hex);
    expect_rsp(200, want2);
    TEST_ASSERT_EQUAL_STRING("ready", RSP.log_reason);
}

void test_finish_with_a_wrong_declared_sha256_ends_invalid(void) {
    api_post_json("/api/v1/iso",
                  "{\"name\":\"win\",\"size\":69632,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}");
    upload_all("11110001", SZ2);
    api_post_empty("/api/v1/iso/11110001/finish");
    expect_rsp(202, "{\"state\":\"finalizing\"}");
    (void)drive(0, 1000);
    iso_core_publish(&CORE);
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"state\":\"invalid\",\"sha256\":null,\"received\":69632,\"invalid_reason\":\"checksum_mismatch\""));
    api_post_empty("/api/v1/iso/11110001/finish");
    expect_err(422, "checksum_mismatch");
    TEST_ASSERT_EQUAL_STRING("checksum_mismatch", RSP.log_reason);
    /* invalid のイメージは、PUT も mount もできない。削除はできる */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"11110001\"}");
    expect_err(409, "image_not_ready");
    api_delete("11110001");
    expect_rsp(202, "{\"state\":\"deleting\"}");
}

void test_finish_without_the_iso_signature_ends_not_iso(void) {
    SIG = "XXXXX";
    api_post_json("/api/v1/iso", "{\"name\":\"junk\",\"size\":69632}");
    upload_all("11110001", SZ2);
    api_post_empty("/api/v1/iso/11110001/finish");
    (void)drive(0, 1000);
    iso_core_publish(&CORE);
    api_post_empty("/api/v1/iso/11110001/finish");
    expect_err(422, "not_iso");
}

void test_finish_of_an_incomplete_image_resumes_the_slot(void) {
    api_add_incomplete(A, "alpha", SZ2, SZ2); /* 全部ある(再起動で finalizing が失われた) */
    api_boot();
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_rsp(202, "{\"state\":\"finalizing\"}");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, api_find(A)->state);
    (void)drive(0, 1000);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, api_find(A)->state);
}

void test_finish_of_a_ready_image_without_a_table_is_still_finalizing(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot_no_index();
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_rsp(202, "{\"state\":\"finalizing\"}"); /* INDEX 待ち・中。表示は finalizing */
    (void)drive(0, 1000);
    iso_core_publish(&CORE);
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_rsp(200, "{\"state\":\"ready\",\"sha256\":null}");
}

void test_finish_of_a_meta_corrupt_image_is_422_meta_corrupt(void) {
    api_add_ready(A, "alpha", SZ2);
    TEST_ASSERT_TRUE(mock_fs_poke(&FS, "/iso/" A "/meta.a", 100, 0x55)); /* CRC が合わない */
    api_boot();
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_err(422, "meta_corrupt");
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "{\"id\":\"aaaaaaa1\",\"name\":\"aaaaaaa1\",\"size\":null,\"state\":\"invalid\",\"sha256\":null,\"received\":null,\"invalid_reason\":\"meta_corrupt\"}"));
}

void test_finish_exec_mapping_when_the_view_is_stale(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_incomplete(B, "beta", SZ2, PART_SMALL);
    api_boot();
    /* ビューは、B = incomplete で全部ある、と言っている(古い)。実際は received < size → size_mismatch(実際の received と size) */
    use_stale();
    STALE.view.images[1].received = SZ2;
    api_post_empty("/api/v1/iso/" B "/finish");
    expect_rsp(409, "{\"error\":\"size_mismatch\",\"received\":34816,\"size\":69632}");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, api_find(B)->state);
    /* A: ビューは finalizing(処理へ進む)。実際は invalid(理由 checksum_mismatch) */
    use_live();
    iso_catalog_entry_t *a = api_find(A);
    a->state = ISO_STATE_INVALID;
    a->invalid_reason = ISO_INVALID_CHECKSUM_MISMATCH;
    use_stale();
    STALE.view.images[0].state = ISO_STATE_FINALIZING;
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_err(422, "checksum_mismatch");
    /* A: 実際は消えた */
    iso_catalog_remove(&CAT, A);
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_err(404, "not_found");
}

void test_finish_exec_upload_in_progress_when_the_view_is_stale(void) {
    api_add_incomplete(A, "alpha", SZ2, SZ2);
    api_boot();
    use_stale(); /* 枠は空き */
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(api_up(), "live", 4, 100000, NULL, id));
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_err(409, "upload_in_progress");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, api_find(A)->state);
}

void test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing(void) {
    api_add_incomplete(A, "alpha", SZ2, SZ2);
    for (unsigned i = 0; i < ISO_WORKER_QUEUE; i++) {
        char id[9], nm[16];
        snprintf(id, sizeof(id), "%08x", 0x200u + i);
        snprintf(nm, sizeof(nm), "f%u", i);
        api_add_ready(id, nm, SZ2);
    }
    api_boot();
    for (unsigned i = 0; i < ISO_WORKER_QUEUE; i++) {
        char id[9];
        snprintf(id, sizeof(id), "%08x", 0x200u + i);
        TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, id));
    }
    TEST_ASSERT_EQUAL_UINT(ISO_WORKER_QUEUE, iso_core_queue_len(&CORE));
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_err(503, "busy");
    TEST_ASSERT_TRUE(RSP.has_retry_after);
    TEST_ASSERT_EQUAL_UINT32(1, RSP.retry_after_s);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, api_find(A)->state); /* 受付は取り消された */
    /* DELETE も(待っているジョブのあるイメージの DELETE は、そのジョブを置き換えるので、あふれない。ないイメージは 503) */
    api_delete(A);
    expect_err(503, "busy");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, api_find(A)->state);
    api_delete("00000200");
    expect_rsp(202, "{\"state\":\"deleting\"}");
}

void test_finish_io_error_while_reloading_received_is_503(void) {
    api_add_incomplete(A, "alpha", SZ2, SZ2);
    api_boot();
    mock_fs_fail_path(&FS, "/iso/" A "/p000");
    api_post_empty("/api/v1/iso/" A "/finish");
    expect_err(503, "sd_unavailable");
    mock_fs_fail_path(&FS, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, api_find(A)->state);
}

/* ---- mount / eject ---------------------------------------------------------------------- */

void test_mount_flow(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_ready(B, "beta", SZ2);
    api_boot();
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    expect_rsp(200, "{\"result\":\"mounted\",\"id\":\"aaaaaaa1\"}");
    TEST_ASSERT_TRUE(RSP.log);
    TEST_ASSERT_EQUAL_STRING("mount", RSP.log_kind);
    TEST_ASSERT_EQUAL_STRING("mounted", RSP.log_reason);
    TEST_ASSERT_EQUAL_STRING(A, RSP.log_id);
    uint32_t id = 0;
    TEST_ASSERT_TRUE(iso_media_get_id(&MEDIA, &id));
    TEST_ASSERT_EQUAL_HEX32(0xaaaaaaa1u, id);
    TEST_ASSERT_TRUE(iso_media_unit_attention_pending(&MEDIA));
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"mounted\":{\"id\":\"aaaaaaa1\",\"name\":\"alpha\"}"));
    /* 同じイメージ */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    expect_rsp(200, "{\"result\":\"already_mounted\",\"id\":\"aaaaaaa1\"}");
    TEST_ASSERT_EQUAL_STRING("already_mounted", RSP.log_reason);
    /* 差し替え */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" B "\"}");
    expect_rsp(200, "{\"result\":\"mounted\",\"id\":\"bbbbbbb2\"}");
    TEST_ASSERT_TRUE(iso_media_get_id(&MEDIA, &id));
    TEST_ASSERT_EQUAL_HEX32(0xbbbbbbb2u, id);
    /* 削除できない */
    api_delete(B);
    expect_err(409, "mounted");
    api_delete(A);
    expect_rsp(202, "{\"state\":\"deleting\"}");
}

void test_eject_flow_and_target_eject(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    api_post_empty("/api/v1/iso/eject");
    expect_rsp(200, "{\"result\":\"not_mounted\"}");
    TEST_ASSERT_EQUAL_STRING("eject", RSP.log_kind);
    TEST_ASSERT_EQUAL_STRING("not_mounted", RSP.log_reason);
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    api_post_json("/api/v1/iso/eject", "{}");
    expect_rsp(200, "{\"result\":\"ejected\"}");
    TEST_ASSERT_EQUAL_STRING("ejected", RSP.log_reason);
    TEST_ASSERT_FALSE(iso_media_is_loaded(&MEDIA));
    /* ターゲットが取り外した: ejected_by_target は、次の mount / eject まで残る */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_target_eject(&MEDIA));
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"mounted\":null,\"target\":{\"locked\":false,\"ejected_by_target\":true}"));
    api_get(); /* GET では消えない */
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"ejected_by_target\":true"));
    api_post_empty("/api/v1/iso/eject");
    expect_rsp(200, "{\"result\":\"not_mounted\"}");
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"ejected_by_target\":false"));
    /* mount も戻す */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_target_eject(&MEDIA));
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    expect_rsp(200, "{\"result\":\"mounted\",\"id\":\"aaaaaaa1\"}");
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"ejected_by_target\":false"));
}

void test_eject_is_refused_while_the_target_prevents_removal_unless_forced(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    TEST_ASSERT_TRUE(iso_media_set_prevent(&MEDIA, true));
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"target\":{\"locked\":true,"));
    api_post_empty("/api/v1/iso/eject");
    expect_err(409, "locked_by_target");
    TEST_ASSERT_EQUAL_STRING("locked_by_target", RSP.log_reason);
    api_post_json("/api/v1/iso/eject", "{\"force\":false}");
    expect_err(409, "locked_by_target");
    TEST_ASSERT_TRUE(iso_media_is_loaded(&MEDIA));
    api_post_json("/api/v1/iso/eject", "{\"force\":true}");
    expect_rsp(200, "{\"result\":\"ejected\"}");
    TEST_ASSERT_FALSE(iso_media_is_loaded(&MEDIA));
    TEST_ASSERT_FALSE(iso_media_prevent_removal(&MEDIA)); /* 取り外すと解除される */
}

static void hook_prevent(void) { (void)iso_media_set_prevent(&MEDIA, true); }

void test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_ready(B, "beta", SZ2);
    api_boot();
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    OL_HOOK = hook_prevent; /* 判定の後、処理の前に、ターゲットが防止した */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" B "\"}");
    expect_err(409, "locked_by_target");
    TEST_ASSERT_EQUAL_STRING("locked_by_target", RSP.log_reason);
    uint32_t id = 0;
    TEST_ASSERT_TRUE(iso_media_get_id(&MEDIA, &id));
    TEST_ASSERT_EQUAL_HEX32(0xaaaaaaa1u, id);
    TEST_ASSERT_EQUAL_INT(0, OL_DEPTH);
    OL_HOOK = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&MEDIA, true));
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    OL_HOOK = hook_prevent;
    api_post_empty("/api/v1/iso/eject");
    expect_err(409, "locked_by_target");
    TEST_ASSERT_TRUE(iso_media_is_loaded(&MEDIA));
}

void test_mount_exec_requires_a_table_of_the_current_card(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    EPOCH++; /* カードが替わった(表は、前のカードのもの) */
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    expect_err(409, "image_not_ready");
    TEST_ASSERT_FALSE(iso_media_is_loaded(&MEDIA));
    EPOCH--;
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    TEST_ASSERT_EQUAL_INT(200, RSP.status);
}

void test_mount_exec_when_the_catalog_changed_after_the_check(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_ready(B, "beta", SZ2);
    api_boot();
    /* A: カタログから消えた */
    iso_catalog_remove(&CAT, A);
    use_stale();
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    expect_err(404, "not_found");
    /* B: 削除が始まった */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, B));
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" B "\"}");
    expect_err(404, "not_found");
    /* ready でなくなった */
    api_add_incomplete("cccccccc", "gamma", SZ2, PART_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_catalog_entry_t *c = api_find("cccccccc");
    TEST_ASSERT_NOT_NULL(c);
    STALE.view.images[0].state = ISO_STATE_READY;
    snprintf(STALE.view.images[0].id, sizeof(STALE.view.images[0].id), "%s", "cccccccc");
    STALE.view.count = 1;
    api_post_json("/api/v1/iso/mount", "{\"id\":\"cccccccc\"}");
    expect_err(409, "image_not_ready");
    TEST_ASSERT_FALSE(iso_media_is_loaded(&MEDIA));
}

void test_mount_and_eject_exec_when_the_sd_went_away_after_the_check(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    OL_HOOK = hook_sd_gone;
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    expect_err(503, "not_ready");
    TEST_ASSERT_FALSE(iso_media_is_loaded(&MEDIA));
    TEST_ASSERT_EQUAL_INT(0, OL_DEPTH);
    TEST_ASSERT_EQUAL_INT(0, SL_DEPTH);
    /* eject の例外は、処理のときも */
    SD_STATE = ISO_SD_READY;
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}"); /* OL_HOOK がまだ有効: SD が消える */
    OL_HOOK = NULL;
    SD_STATE = ISO_SD_READY;
    iso_core_publish(&CORE);
    api_post_json("/api/v1/iso/mount", "{\"id\":\"" A "\"}");
    TEST_ASSERT_EQUAL_INT(200, RSP.status);
    OL_HOOK = hook_sd_gone;
    api_post_empty("/api/v1/iso/eject");
    expect_rsp(200, "{\"result\":\"not_mounted\"}");
    TEST_ASSERT_TRUE(iso_media_is_loaded(&MEDIA)); /* メディアには触らない */
    TEST_ASSERT_EQUAL_INT(0, OL_DEPTH);
}

/* ---- delete ----------------------------------------------------------------------------- */

void test_delete_flow(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    api_delete(A);
    expect_rsp(202, "{\"state\":\"deleting\"}");
    TEST_ASSERT_TRUE(RSP.log);
    TEST_ASSERT_EQUAL_STRING("delete", RSP.log_kind);
    TEST_ASSERT_EQUAL_STRING(A, RSP.log_id);
    TEST_ASSERT_EQUAL_STRING("deleting", RSP.log_reason);
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"state\":\"deleting\""));
    api_delete(A); /* 冪等 */
    expect_rsp(202, "{\"state\":\"deleting\"}");
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
    (void)drive(0, 1000);
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/" A));
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"images\":[]"));
    api_delete(A);
    expect_err(404, "not_found");
}

void test_delete_aborts_an_upload_and_frees_the_slot(void) {
    char id[9];
    begin_ok("img", 100000, id);
    api_put_chunk(id, 0, 1000);
    char path[32];
    snprintf(path, sizeof(path), "/api/v1/iso/%s", id);
    api_call(ISO_API_M_DELETE, path, NULL, NULL);
    expect_rsp(202, "{\"state\":\"deleting\"}");
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"upload\":null"));
    api_put_chunk(id, 1000, 1000); /* 次の PUT は 404 */
    expect_err(404, "not_found");
    (void)drive(0, 1000);
    api_post_json("/api/v1/iso", "{\"name\":\"again\",\"size\":34816}"); /* 枠が空いた */
    TEST_ASSERT_EQUAL_INT(201, RSP.status);
}

static void hook_mount_a(void) { (void)iso_media_mount(&MEDIA, 0xaaaaaaa1u, false); }

void test_delete_exec_mapping(void) {
    api_add_ready(A, "alpha", SZ2);
    api_add_ready(B, "beta", SZ2);
    api_boot();
    /* 判定の後に、マウントされた */
    OL_HOOK = hook_mount_a;
    api_delete(A);
    expect_err(409, "mounted");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, api_find(A)->state);
    OL_HOOK = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&MEDIA, true));
    /* 判定の後に、カタログから消えた */
    use_stale();
    iso_catalog_remove(&CAT, B);
    api_delete(B);
    expect_err(404, "not_found");
    /* crosslinked の照合の I/O エラー */
    use_live();
    iso_core_publish(&CORE);
    mock_fs_fail_path(&FS, "/iso/import");
    api_delete(A);
    expect_err(503, "sd_unavailable");
    mock_fs_fail_path(&FS, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, api_find(A)->state);
}

void test_delete_with_a_crosslinked_import_file_is_409_and_changes_nothing(void) {
    api_add_ready(A, "alpha", SZ2);
    api_boot();
    uint32_t cl = mock_fs_first_cluster(&FS, "/iso/" A "/p000");
    static const uint8_t junk[4] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/x.iso", junk, sizeof(junk), cl));
    api_delete(A);
    expect_err(409, "crosslinked");
    TEST_ASSERT_EQUAL_STRING("crosslinked", RSP.log_reason);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, api_find(A)->state);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
}

/* ---- rescan ----------------------------------------------------------------------------- */

static void add_import(const char *name) {
    static uint8_t buf[PART_SMALL];
    gen_fill(buf, 0, sizeof(buf));
    char path[ISO_PATH_MAX];
    snprintf(path, sizeof(path), "/iso/import/%s", name);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, path, buf, sizeof(buf), 0));
}

void test_rescan_imports_and_reports(void) {
    api_boot();
    add_import("one.iso");
    add_import("two.ISO");
    static uint8_t junk[PART_SMALL]; /* 署名がない */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/bad.iso", junk, sizeof(junk), 0));
    api_post_empty("/api/v1/iso/rescan");
    expect_rsp(200, "{\"imported\":2,\"skipped\":1,\"next_cursor\":null}");
    TEST_ASSERT_EQUAL_STRING("rescan", RSP.log_kind);
    TEST_ASSERT_EQUAL_STRING("ok", RSP.log_reason);
    TEST_ASSERT_EQUAL_UINT(2, CAT.count);
    TEST_ASSERT_EQUAL_UINT(2, iso_core_index_pending(&CORE)); /* 取り込んだものは、エクステントの表の作成へ */
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"name\":\"one\",\"size\":34816,\"state\":\"finalizing\""));
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"name\":\"two\",\"size\":34816,\"state\":\"finalizing\""));
    (void)drive(0, 1000);
    iso_core_publish(&CORE);
    api_get();
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"name\":\"one\",\"size\":34816,\"state\":\"ready\",\"sha256\":null"));
}

void test_rescan_continues_with_the_cursor(void) {
    api_boot();
    const char *names[] = {"a1.iso", "a2.iso", "a3.iso", "a4.iso", "a5.iso", "a6.iso"};
    for (int i = 0; i < 6; i++) add_import(names[i]);
    api_post_empty("/api/v1/iso/rescan");
    expect_rsp(200, "{\"imported\":4,\"skipped\":0,\"next_cursor\":0}"); /* 取り込んだものはディレクトリから消える。位置は進まない */
    api_post_json("/api/v1/iso/rescan", "{\"cursor\":0}");
    expect_rsp(200, "{\"imported\":2,\"skipped\":0,\"next_cursor\":null}");
    TEST_ASSERT_EQUAL_UINT(6, CAT.count);
}

void test_rescan_io_error_is_503_but_keeps_what_was_imported(void) {
    api_boot();
    add_import("one.iso");
    mock_fs_fail_dir_next_after(&FS, "/iso/import", 1);
    api_post_empty("/api/v1/iso/rescan");
    expect_err(503, "sd_unavailable");
    TEST_ASSERT_EQUAL_INT(0, OL_DEPTH);
}

/* ---- 応答の置き場が小さいとき ------------------------------------------------------------------- */

void test_a_small_response_buffer_gives_a_500_that_fits_or_is_empty(void) {
    char small[40];
    iso_api_request_t rq;
    api_rq(&rq, ISO_API_M_GET, "/api/v1/iso", NULL, NULL);
    iso_api_response_init(&RSP, small, sizeof(small));
    iso_api_handle(&ENV, &rq, &RSP);
    TEST_ASSERT_EQUAL_INT(500, RSP.status);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"internal_error\"}", RSP.body);
    TEST_ASSERT_EQUAL_UINT(26, RSP.body_len);
    char tiny[10];
    iso_api_response_init(&RSP, tiny, sizeof(tiny));
    iso_api_handle(&ENV, &rq, &RSP);
    TEST_ASSERT_EQUAL_INT(500, RSP.status);
    TEST_ASSERT_EQUAL_STRING("", RSP.body);
    TEST_ASSERT_EQUAL_UINT(0, RSP.body_len);
    /* エラーの本体が入らないとき */
    api_rq(&rq, ISO_API_M_DELETE, "/api/v1/iso/" A, NULL, NULL);
    iso_api_response_init(&RSP, tiny, sizeof(tiny));
    iso_api_handle(&ENV, &rq, &RSP);
    TEST_ASSERT_EQUAL_INT(500, RSP.status);
    TEST_ASSERT_EQUAL_UINT(0, RSP.body_len);
    iso_api_handle(&ENV, &rq, NULL); /* NULL は何もしない */
    iso_api_handle(&ENV, NULL, &RSP);
    TEST_ASSERT_EQUAL_INT(500, RSP.status);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_begin_accepted);
    RUN_TEST(test_begin_keeps_the_declared_sha256_in_lower_case_bytes);
    RUN_TEST(test_begin_without_sha256_has_no_declared_value);
    RUN_TEST(test_begin_accepts_utf8_names_and_unicode_escapes);
    RUN_TEST(test_begin_name_matches_are_case_sensitive);
    RUN_TEST(test_begin_exec_mapping_when_the_view_is_stale);
    RUN_TEST(test_begin_exec_too_many_images_when_the_view_is_stale);
    RUN_TEST(test_begin_io_errors);
    RUN_TEST(test_begin_exhausted_ids_are_a_retryable_503);
    RUN_TEST(test_put_flow_two_chunks);
    RUN_TEST(test_put_resume_of_an_incomplete_image);
    RUN_TEST(test_commit_maps_the_upload_results_when_the_state_changed_after_the_precheck);
    RUN_TEST(test_commit_upload_in_progress_for_a_resume_when_another_upload_started);
    RUN_TEST(test_commit_with_a_fabricated_put_maps_the_remaining_results);
    RUN_TEST(test_commit_507_when_the_resume_no_longer_fits);
    RUN_TEST(test_commit_write_failures);
    RUN_TEST(test_commit_rechecks_the_sd_after_taking_the_op_lock);
    RUN_TEST(test_commit_rejects_bad_arguments);
    RUN_TEST(test_put_precheck_rejects_a_request_that_is_not_a_put);
    RUN_TEST(test_finish_flow_with_a_declared_sha256);
    RUN_TEST(test_finish_with_a_wrong_declared_sha256_ends_invalid);
    RUN_TEST(test_finish_without_the_iso_signature_ends_not_iso);
    RUN_TEST(test_finish_of_an_incomplete_image_resumes_the_slot);
    RUN_TEST(test_finish_of_a_ready_image_without_a_table_is_still_finalizing);
    RUN_TEST(test_finish_of_a_meta_corrupt_image_is_422_meta_corrupt);
    RUN_TEST(test_finish_exec_mapping_when_the_view_is_stale);
    RUN_TEST(test_finish_exec_upload_in_progress_when_the_view_is_stale);
    RUN_TEST(test_finish_with_a_full_worker_queue_is_503_busy_and_changes_nothing);
    RUN_TEST(test_finish_io_error_while_reloading_received_is_503);
    RUN_TEST(test_mount_flow);
    RUN_TEST(test_eject_flow_and_target_eject);
    RUN_TEST(test_eject_is_refused_while_the_target_prevents_removal_unless_forced);
    RUN_TEST(test_mount_and_eject_exec_see_a_lock_that_appeared_after_the_check);
    RUN_TEST(test_mount_exec_requires_a_table_of_the_current_card);
    RUN_TEST(test_mount_exec_when_the_catalog_changed_after_the_check);
    RUN_TEST(test_mount_and_eject_exec_when_the_sd_went_away_after_the_check);
    RUN_TEST(test_delete_flow);
    RUN_TEST(test_delete_aborts_an_upload_and_frees_the_slot);
    RUN_TEST(test_delete_exec_mapping);
    RUN_TEST(test_delete_with_a_crosslinked_import_file_is_409_and_changes_nothing);
    RUN_TEST(test_rescan_imports_and_reports);
    RUN_TEST(test_rescan_continues_with_the_cursor);
    RUN_TEST(test_rescan_io_error_is_503_but_keeps_what_was_imported);
    RUN_TEST(test_a_small_response_buffer_gives_a_500_that_fits_or_is_empty);
    return UNITY_END();
}

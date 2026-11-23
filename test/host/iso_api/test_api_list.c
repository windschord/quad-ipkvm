/*
 * GET /api/v1/iso の応答(WEB_API.md 8.1。iso_api_format_list と、iso_api_handle の GET)と、公開ビューのコピー(iso_api_snapshot_update)。
 * 期待値は、仕様の JSON を手で書いたもの(実装から計算しない)。サービスが起動していないときは、sd.state = no_card。
 */
#include "unity.h"
#include "iso_api.h"
#include "iso_api_priv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static iso_api_snapshot_t S;
static iso_media_status_t M;
static char OUT[ISO_API_RESPONSE_MAX];

static const char SHA_HEX[] = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

static void fill_sha(uint8_t sha[32]) {
    for (int i = 0; i < 32; i++) sha[i] = (uint8_t)((i % 16) * 0x11);
}

static void reset(void) {
    memset(&S, 0, sizeof(S));
    memset(&M, 0, sizeof(M));
    S.view.sd_state = ISO_SD_READY;
    S.view.free_known = true;
    S.view.total_bytes = 31914983424ull;
    S.view.free_bytes = 22000000000ull;
    S.view.chunk_bytes = 131072;
}

static iso_view_image_t *add(const char *id, const char *name, uint64_t size, iso_state_t st, uint64_t received) {
    iso_view_image_t *im = &S.view.images[S.view.count++];
    memset(im, 0, sizeof(*im));
    snprintf(im->id, sizeof(im->id), "%s", id);
    im->state = st;
    im->meta_valid = true;
    im->name_len = (uint8_t)strlen(name);
    memcpy(im->name, name, im->name_len);
    im->size_bytes = size;
    im->received = received;
    return im;
}

static void expect_list(const char *want) {
    size_t n = 12345;
    memset(OUT, 'X', sizeof(OUT));
    TEST_ASSERT_EQUAL_INT(0, iso_api_format_list(&S, &M, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_STRING(want, OUT);
    TEST_ASSERT_EQUAL_UINT(strlen(want), n);
}

/* ---- 仕様の例 -------------------------------------------------------------------------- */

void test_the_example_of_the_spec(void) {
    reset();
    iso_view_image_t *a = add("a1b2c3d4", "ubuntu-24.04", 6126805504ull, ISO_STATE_READY, 6126805504ull);
    a->has_sha = true;
    fill_sha(a->sha256);
    add("0f9e8d7c", "win11", 5500000000ull, ISO_STATE_INCOMPLETE, 2147483648ull);
    M.mounted = true;
    M.id = 0xa1b2c3d4u;
    expect_list(
        "{\"sd\":{\"state\":\"ready\",\"total_bytes\":31914983424,\"free_bytes\":22000000000},"
        "\"images\":["
        "{\"id\":\"a1b2c3d4\",\"name\":\"ubuntu-24.04\",\"size\":6126805504,\"state\":\"ready\","
        "\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\","
        "\"received\":6126805504,\"invalid_reason\":null},"
        "{\"id\":\"0f9e8d7c\",\"name\":\"win11\",\"size\":5500000000,\"state\":\"incomplete\",\"sha256\":null,"
        "\"received\":2147483648,\"invalid_reason\":null}],"
        "\"mounted\":{\"id\":\"a1b2c3d4\",\"name\":\"ubuntu-24.04\"},"
        "\"target\":{\"locked\":false,\"ejected_by_target\":false},"
        "\"chunk_size\":131072,\"hidden_images\":0,\"upload\":null}");
}

void test_an_empty_ready_card(void) {
    reset();
    expect_list(
        "{\"sd\":{\"state\":\"ready\",\"total_bytes\":31914983424,\"free_bytes\":22000000000},\"images\":[],\"mounted\":null,"
        "\"target\":{\"locked\":false,\"ejected_by_target\":false},\"chunk_size\":131072,\"hidden_images\":0,\"upload\":null}");
}

/* ---- sd ----------------------------------------------------------------------------- */

void test_sd_states_other_than_ready(void) {
    static const struct {
        iso_sd_state_t s;
        const char *name;
    } T[] = {{ISO_SD_NO_CARD, "no_card"}, {ISO_SD_UNSUPPORTED_FS, "unsupported_fs"}, {ISO_SD_ERROR, "error"}};
    for (size_t i = 0; i < 3; i++) {
        reset();
        add("a1b2c3d4", "x", 34816, ISO_STATE_READY, 34816);
        S.view.hidden = 3;
        S.view.upload_active = true;
        snprintf(S.view.upload_id, sizeof(S.view.upload_id), "%s", "a1b2c3d4");
        S.view.upload_state = ISO_UP_ACT_RECEIVING;
        S.view.sd_state = T[i].s;
        char want[400];
        snprintf(want, sizeof(want),
                 "{\"sd\":{\"state\":\"%s\",\"total_bytes\":null,\"free_bytes\":null},\"images\":[],\"mounted\":null,"
                 "\"target\":{\"locked\":false,\"ejected_by_target\":false},\"chunk_size\":131072,\"hidden_images\":0,\"upload\":null}",
                 T[i].name);
        expect_list(want); /* images は空、hidden は 0、upload は null(カタログは、SD がなければ意味がない) */
    }
}

void test_an_unknown_sd_state_is_reported_as_error(void) {
    reset();
    S.view.sd_state = (iso_sd_state_t)77;
    expect_list(
        "{\"sd\":{\"state\":\"error\",\"total_bytes\":null,\"free_bytes\":null},\"images\":[],\"mounted\":null,"
        "\"target\":{\"locked\":false,\"ejected_by_target\":false},\"chunk_size\":131072,\"hidden_images\":0,\"upload\":null}");
}

void test_total_and_free_are_null_until_sampled(void) {
    reset();
    S.view.free_known = false;
    expect_list(
        "{\"sd\":{\"state\":\"ready\",\"total_bytes\":null,\"free_bytes\":null},\"images\":[],\"mounted\":null,"
        "\"target\":{\"locked\":false,\"ejected_by_target\":false},\"chunk_size\":131072,\"hidden_images\":0,\"upload\":null}");
}

void test_chunk_size_and_hidden_images(void) {
    reset();
    S.view.chunk_bytes = 65536;
    S.view.hidden = 5;
    expect_list(
        "{\"sd\":{\"state\":\"ready\",\"total_bytes\":31914983424,\"free_bytes\":22000000000},\"images\":[],\"mounted\":null,"
        "\"target\":{\"locked\":false,\"ejected_by_target\":false},\"chunk_size\":65536,\"hidden_images\":5,\"upload\":null}");
    S.view.chunk_bytes = 0; /* 未設定は、既定(128 KiB) */
    expect_list(
        "{\"sd\":{\"state\":\"ready\",\"total_bytes\":31914983424,\"free_bytes\":22000000000},\"images\":[],\"mounted\":null,"
        "\"target\":{\"locked\":false,\"ejected_by_target\":false},\"chunk_size\":131072,\"hidden_images\":5,\"upload\":null}");
}

/* ---- イメージの状態 ----------------------------------------------------------------------- */

void test_every_image_state_and_invalid_reason(void) {
    reset();
    add("00000001", "u", 100000, ISO_STATE_UPLOADING, 40000);
    add("00000002", "f", 100000, ISO_STATE_FINALIZING, 100000);
    add("00000003", "d", 100000, ISO_STATE_DELETING, 7);
    iso_view_image_t *i1 = add("00000004", "i1", 100000, ISO_STATE_INVALID, 100000);
    i1->invalid_reason = ISO_INVALID_CHECKSUM_MISMATCH;
    iso_view_image_t *i2 = add("00000005", "i2", 100000, ISO_STATE_INVALID, 100000);
    i2->invalid_reason = ISO_INVALID_NOT_ISO;
    iso_view_image_t *i3 = add("00000006", "i3", 100000, ISO_STATE_INVALID, 100000);
    i3->invalid_reason = ISO_INVALID_TOO_FRAGMENTED;
    iso_view_image_t *i4 = add("00000007", "i4", 100000, ISO_STATE_INVALID, 100000);
    i4->invalid_reason = ISO_INVALID_PART_MISMATCH;
    iso_view_image_t *i0 = add("00000008", "i0", 100000, ISO_STATE_INVALID, 100000);
    i0->invalid_reason = 0; /* 理由なし: null */
    iso_view_image_t *i9 = add("00000009", "i9", 100000, ISO_STATE_INVALID, 100000);
    i9->invalid_reason = 77; /* 範囲外: null */
    add("0000000a", "n", 100000, ISO_STATE_NONE, 0); /* NONE は出さない */
    iso_view_image_t *r = add("0000000b", "r", 100000, ISO_STATE_READY, 0); /* ready の received は size */
    r->invalid_reason = ISO_INVALID_NOT_ISO;                                /* ready では理由を出さない */
    expect_list(
        "{\"sd\":{\"state\":\"ready\",\"total_bytes\":31914983424,\"free_bytes\":22000000000},\"images\":["
        "{\"id\":\"00000001\",\"name\":\"u\",\"size\":100000,\"state\":\"uploading\",\"sha256\":null,\"received\":40000,\"invalid_reason\":null},"
        "{\"id\":\"00000002\",\"name\":\"f\",\"size\":100000,\"state\":\"finalizing\",\"sha256\":null,\"received\":100000,\"invalid_reason\":null},"
        "{\"id\":\"00000003\",\"name\":\"d\",\"size\":100000,\"state\":\"deleting\",\"sha256\":null,\"received\":7,\"invalid_reason\":null},"
        "{\"id\":\"00000004\",\"name\":\"i1\",\"size\":100000,\"state\":\"invalid\",\"sha256\":null,\"received\":100000,\"invalid_reason\":\"checksum_mismatch\"},"
        "{\"id\":\"00000005\",\"name\":\"i2\",\"size\":100000,\"state\":\"invalid\",\"sha256\":null,\"received\":100000,\"invalid_reason\":\"not_iso\"},"
        "{\"id\":\"00000006\",\"name\":\"i3\",\"size\":100000,\"state\":\"invalid\",\"sha256\":null,\"received\":100000,\"invalid_reason\":\"too_fragmented\"},"
        "{\"id\":\"00000007\",\"name\":\"i4\",\"size\":100000,\"state\":\"invalid\",\"sha256\":null,\"received\":100000,\"invalid_reason\":\"part_mismatch\"},"
        "{\"id\":\"00000008\",\"name\":\"i0\",\"size\":100000,\"state\":\"invalid\",\"sha256\":null,\"received\":100000,\"invalid_reason\":null},"
        "{\"id\":\"00000009\",\"name\":\"i9\",\"size\":100000,\"state\":\"invalid\",\"sha256\":null,\"received\":100000,\"invalid_reason\":null},"
        "{\"id\":\"0000000b\",\"name\":\"r\",\"size\":100000,\"state\":\"ready\",\"sha256\":null,\"received\":100000,\"invalid_reason\":null}],"
        "\"mounted\":null,\"target\":{\"locked\":false,\"ejected_by_target\":false},\"chunk_size\":131072,\"hidden_images\":0,\"upload\":null}");
}

void test_meta_corrupt_image_has_the_id_as_its_name_and_null_sizes(void) {
    reset();
    iso_view_image_t *im = add("deadbeef", "ignored", 123, ISO_STATE_INVALID, 456);
    im->meta_valid = false;
    im->invalid_reason = ISO_INVALID_META_CORRUPT;
    expect_list(
        "{\"sd\":{\"state\":\"ready\",\"total_bytes\":31914983424,\"free_bytes\":22000000000},\"images\":["
        "{\"id\":\"deadbeef\",\"name\":\"deadbeef\",\"size\":null,\"state\":\"invalid\",\"sha256\":null,\"received\":null,"
        "\"invalid_reason\":\"meta_corrupt\"}],"
        "\"mounted\":null,\"target\":{\"locked\":false,\"ejected_by_target\":false},\"chunk_size\":131072,\"hidden_images\":0,\"upload\":null}");
}

/* ---- 名前のエスケープ -------------------------------------------------------------------- */

void test_names_are_json_escaped(void) {
    reset();
    add("00000001", "say \"hi\"", 34816, ISO_STATE_INCOMPLETE, 0);
    add("00000002", "\xE6\x97\xA5\xE6\x9C\xAC", 34816, ISO_STATE_INCOMPLETE, 0);               /* 日本: そのまま */
    add("00000003", "\xF0\x9F\x98\x80<b>&'", 34816, ISO_STATE_INCOMPLETE, 0);                  /* 4 バイトの UTF-8、< > & ' はそのまま */
    iso_view_image_t *c = add("00000004", "a_b", 34816, ISO_STATE_INCOMPLETE, 0);              /* 制御文字(通常は名前の規則で入らない)は、\u00XX */
    c->name[1] = '\x1f';
    iso_view_image_t *d = add("00000005", "a_b", 34816, ISO_STATE_INCOMPLETE, 0);
    d->name[1] = '\x01';
    iso_view_image_t *e = add("00000006", "a_b", 34816, ISO_STATE_INCOMPLETE, 0);
    e->name[1] = '\\';
    iso_view_image_t *f = add("00000007", "a_b", 34816, ISO_STATE_INCOMPLETE, 0);
    f->name[1] = ' '; /* 0x20 はそのまま */
    iso_view_image_t *g = add("00000008", "a_b", 34816, ISO_STATE_INCOMPLETE, 0);
    g->name[1] = '\x7f'; /* DEL は、JSON ではそのまま */
    iso_view_image_t *h = add("00000009", "a_b", 34816, ISO_STATE_INCOMPLETE, 0);
    h->name[1] = '\n';
    char want[2048];
    snprintf(want, sizeof(want),
             "{\"sd\":{\"state\":\"ready\",\"total_bytes\":31914983424,\"free_bytes\":22000000000},\"images\":["
             "{\"id\":\"00000001\",\"name\":\"say \\\"hi\\\"\",\"size\":34816,\"state\":\"incomplete\",\"sha256\":null,\"received\":0,\"invalid_reason\":null},"
             "{\"id\":\"00000002\",\"name\":\"\xE6\x97\xA5\xE6\x9C\xAC\",\"size\":34816,\"state\":\"incomplete\",\"sha256\":null,\"received\":0,\"invalid_reason\":null},"
             "{\"id\":\"00000003\",\"name\":\"\xF0\x9F\x98\x80<b>&'\",\"size\":34816,\"state\":\"incomplete\",\"sha256\":null,\"received\":0,\"invalid_reason\":null},"
             "{\"id\":\"00000004\",\"name\":\"a\\u001fb\",\"size\":34816,\"state\":\"incomplete\",\"sha256\":null,\"received\":0,\"invalid_reason\":null},"
             "{\"id\":\"00000005\",\"name\":\"a\\u0001b\",\"size\":34816,\"state\":\"incomplete\",\"sha256\":null,\"received\":0,\"invalid_reason\":null},"
             "{\"id\":\"00000006\",\"name\":\"a\\\\b\",\"size\":34816,\"state\":\"incomplete\",\"sha256\":null,\"received\":0,\"invalid_reason\":null},"
             "{\"id\":\"00000007\",\"name\":\"a b\",\"size\":34816,\"state\":\"incomplete\",\"sha256\":null,\"received\":0,\"invalid_reason\":null},"
             "{\"id\":\"00000008\",\"name\":\"a\x7f" "b\",\"size\":34816,\"state\":\"incomplete\",\"sha256\":null,\"received\":0,\"invalid_reason\":null},"
             "{\"id\":\"00000009\",\"name\":\"a\\u000ab\",\"size\":34816,\"state\":\"incomplete\",\"sha256\":null,\"received\":0,\"invalid_reason\":null}],"
             "\"mounted\":null,\"target\":{\"locked\":false,\"ejected_by_target\":false},\"chunk_size\":131072,\"hidden_images\":0,\"upload\":null}");
    expect_list(want);
}

/* ---- mounted、target、upload -------------------------------------------------------------- */

void test_mounted_target_flags(void) {
    reset();
    add("a1b2c3d4", "disc \"1\"", 34816, ISO_STATE_READY, 34816);
    M.mounted = true;
    M.id = 0xa1b2c3d4u;
    M.locked = true;
    M.ejected_by_target = false;
    expect_list(
        "{\"sd\":{\"state\":\"ready\",\"total_bytes\":31914983424,\"free_bytes\":22000000000},\"images\":["
        "{\"id\":\"a1b2c3d4\",\"name\":\"disc \\\"1\\\"\",\"size\":34816,\"state\":\"ready\",\"sha256\":null,\"received\":34816,\"invalid_reason\":null}],"
        "\"mounted\":{\"id\":\"a1b2c3d4\",\"name\":\"disc \\\"1\\\"\"},\"target\":{\"locked\":true,\"ejected_by_target\":false},"
        "\"chunk_size\":131072,\"hidden_images\":0,\"upload\":null}");
    /* ターゲットが取り外した */
    M.mounted = false;
    M.locked = false;
    M.ejected_by_target = true;
    expect_list(
        "{\"sd\":{\"state\":\"ready\",\"total_bytes\":31914983424,\"free_bytes\":22000000000},\"images\":["
        "{\"id\":\"a1b2c3d4\",\"name\":\"disc \\\"1\\\"\",\"size\":34816,\"state\":\"ready\",\"sha256\":null,\"received\":34816,\"invalid_reason\":null}],"
        "\"mounted\":null,\"target\":{\"locked\":false,\"ejected_by_target\":true},\"chunk_size\":131072,\"hidden_images\":0,\"upload\":null}");
}

void test_mounted_image_missing_from_the_view_is_named_by_its_id(void) {
    reset();
    M.mounted = true;
    M.id = 0x0000abcdu;
    expect_list(
        "{\"sd\":{\"state\":\"ready\",\"total_bytes\":31914983424,\"free_bytes\":22000000000},\"images\":[],"
        "\"mounted\":{\"id\":\"0000abcd\",\"name\":\"0000abcd\"},\"target\":{\"locked\":false,\"ejected_by_target\":false},"
        "\"chunk_size\":131072,\"hidden_images\":0,\"upload\":null}");
    /* meta_corrupt のイメージがマウント中でも、名前は id */
    iso_view_image_t *im = add("0000abcd", "zzz", 34816, ISO_STATE_INVALID, 0);
    im->meta_valid = false;
    im->invalid_reason = ISO_INVALID_META_CORRUPT;
    char out[ISO_API_RESPONSE_MAX];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, iso_api_format_list(&S, &M, out, sizeof(out), &n));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"mounted\":{\"id\":\"0000abcd\",\"name\":\"0000abcd\"}"));
}

void test_mounted_name_is_found_among_several_images(void) {
    reset();
    add("00000001", "first", 34816, ISO_STATE_READY, 34816);
    add("00000002", "second", 34816, ISO_STATE_READY, 34816);
    add("00000003", "third", 34816, ISO_STATE_READY, 34816);
    M.mounted = true;
    M.id = 3;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, iso_api_format_list(&S, &M, OUT, sizeof(OUT), &n));
    TEST_ASSERT_NOT_NULL(strstr(OUT, "\"mounted\":{\"id\":\"00000003\",\"name\":\"third\"}"));
    M.id = 1;
    TEST_ASSERT_EQUAL_INT(0, iso_api_format_list(&S, &M, OUT, sizeof(OUT), &n));
    TEST_ASSERT_NOT_NULL(strstr(OUT, "\"mounted\":{\"id\":\"00000001\",\"name\":\"first\"}"));
}

void test_upload_slot_states(void) {
    static const struct {
        iso_upload_activity_t a;
        const char *s;
    } T[] = {{ISO_UP_ACT_RECEIVING, "receiving"}, {ISO_UP_ACT_IDLE, "idle"}, {ISO_UP_ACT_FINALIZING, "finalizing"}, {ISO_UP_ACT_NONE, "idle"}};
    for (size_t i = 0; i < 4; i++) {
        reset();
        S.view.upload_active = true;
        snprintf(S.view.upload_id, sizeof(S.view.upload_id), "%s", "0f9e8d7c");
        S.view.upload_state = T[i].a;
        char want[400];
        snprintf(want, sizeof(want),
                 "{\"sd\":{\"state\":\"ready\",\"total_bytes\":31914983424,\"free_bytes\":22000000000},\"images\":[],\"mounted\":null,"
                 "\"target\":{\"locked\":false,\"ejected_by_target\":false},\"chunk_size\":131072,\"hidden_images\":0,"
                 "\"upload\":{\"id\":\"0f9e8d7c\",\"state\":\"%s\"}}",
                 T[i].s);
        expect_list(want);
    }
    reset();
    S.view.upload_active = false; /* 枠を使っていなければ、状態が RECEIVING でも null */
    S.view.upload_state = ISO_UP_ACT_RECEIVING;
    snprintf(S.view.upload_id, sizeof(S.view.upload_id), "%s", "0f9e8d7c");
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, iso_api_format_list(&S, &M, OUT, sizeof(OUT), &n));
    TEST_ASSERT_NOT_NULL(strstr(OUT, "\"upload\":null}"));
}

/* ---- 最大の大きさ、バッファ ------------------------------------------------------------------ */

void test_the_worst_case_fits_in_the_response_buffer(void) {
    reset();
    char name[ISO_NAME_MAX_BYTES + 1];
    memset(name, '"', ISO_NAME_MAX_BYTES); /* すべて `"` = エスケープで 126 バイト */
    name[ISO_NAME_MAX_BYTES] = '\0';
    char esc[2 * ISO_NAME_MAX_BYTES + 1];
    for (size_t i = 0; i < ISO_NAME_MAX_BYTES; i++) {
        esc[2 * i] = '\\';
        esc[2 * i + 1] = '"';
    }
    esc[2 * ISO_NAME_MAX_BYTES] = '\0';
    S.view.total_bytes = 18446744073709551615ull;
    S.view.free_bytes = 18446744073709551615ull;
    S.view.hidden = 4294967295u;
    S.view.chunk_bytes = 1048576;
    char want[ISO_API_RESPONSE_MAX + 4096];
    size_t pos = (size_t)snprintf(want, sizeof(want), "{\"sd\":{\"state\":\"ready\",\"total_bytes\":18446744073709551615,\"free_bytes\":18446744073709551615},\"images\":[");
    for (unsigned i = 0; i < ISO_MAX_IMAGES; i++) {
        char id[9];
        snprintf(id, sizeof(id), "%08x", 0xf0000000u + i);
        iso_view_image_t *im = add(id, name, 17179869184ull, ISO_STATE_READY, 17179869184ull);
        im->has_sha = true;
        fill_sha(im->sha256);
        pos += (size_t)snprintf(want + pos, sizeof(want) - pos,
                                "%s{\"id\":\"%s\",\"name\":\"%s\",\"size\":17179869184,\"state\":\"ready\",\"sha256\":\"%s\",\"received\":17179869184,\"invalid_reason\":null}",
                                i ? "," : "", id, esc, SHA_HEX);
    }
    M.mounted = true;
    M.id = 0xf000000fu;
    M.locked = true;
    M.ejected_by_target = true;
    S.view.upload_active = true;
    snprintf(S.view.upload_id, sizeof(S.view.upload_id), "%s", "f0000000");
    S.view.upload_state = ISO_UP_ACT_FINALIZING;
    pos += (size_t)snprintf(want + pos, sizeof(want) - pos,
                            "],\"mounted\":{\"id\":\"f000000f\",\"name\":\"%s\"},\"target\":{\"locked\":true,\"ejected_by_target\":true},"
                            "\"chunk_size\":1048576,\"hidden_images\":4294967295,\"upload\":{\"id\":\"f0000000\",\"state\":\"finalizing\"}}",
                            esc);
    TEST_ASSERT_TRUE_MESSAGE(pos < ISO_API_RESPONSE_MAX, "the worst case exceeds ISO_API_RESPONSE_MAX");
    TEST_ASSERT_TRUE_MESSAGE(pos > 5000, "the worst case is not large");
    expect_list(want);

    /* ちょうどの大きさ: cap = len + 1 は成功、cap = len は失敗(出力は空) */
    size_t n = 0;
    char *exact = malloc(pos + 1);
    TEST_ASSERT_NOT_NULL(exact);
    TEST_ASSERT_EQUAL_INT(0, iso_api_format_list(&S, &M, exact, pos + 1, &n));
    TEST_ASSERT_EQUAL_UINT(pos, n);
    TEST_ASSERT_EQUAL_STRING(want, exact);
    memset(exact, 'X', pos + 1);
    n = 99;
    TEST_ASSERT_EQUAL_INT(-1, iso_api_format_list(&S, &M, exact, pos, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)exact[0]);
    free(exact);
}

void test_small_buffers_fail_cleanly_everywhere(void) {
    reset();
    iso_view_image_t *a = add("a1b2c3d4", "ubuntu", 6126805504ull, ISO_STATE_READY, 6126805504ull);
    a->has_sha = true;
    fill_sha(a->sha256);
    M.mounted = true;
    M.id = 0xa1b2c3d4u;
    S.view.upload_active = true;
    snprintf(S.view.upload_id, sizeof(S.view.upload_id), "%s", "a1b2c3d4");
    S.view.upload_state = ISO_UP_ACT_IDLE;
    size_t full = 0;
    TEST_ASSERT_EQUAL_INT(0, iso_api_format_list(&S, &M, OUT, sizeof(OUT), &full));
    for (size_t cap = 0; cap <= full; cap++) { /* ヒープの境界(ASan)で、書き込みの超過を見つける */
        char *small = malloc(cap ? cap : 1);
        size_t n = 7;
        int rc = iso_api_format_list(&S, &M, small, cap, &n);
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, rc, "a buffer smaller than the output fails");
        TEST_ASSERT_EQUAL_UINT(0, n);
        if (cap > 0) TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)small[0]);
        free(small);
    }
    TEST_ASSERT_EQUAL_INT(-1, iso_api_format_list(NULL, &M, OUT, sizeof(OUT), NULL));
    TEST_ASSERT_EQUAL_INT(-1, iso_api_format_list(&S, NULL, OUT, sizeof(OUT), NULL));
    TEST_ASSERT_EQUAL_INT(-1, iso_api_format_list(&S, &M, NULL, 10, NULL));
    TEST_ASSERT_EQUAL_INT(-1, iso_api_format_list(&S, &M, OUT, 0, NULL));
}

/* ---- 公開ビューのコピー --------------------------------------------------------------------- */

void test_snapshot_update_copies_the_view_and_drops_the_table_pointers(void) {
    static iso_view_t v;
    memset(&v, 0, sizeof(v));
    v.sd_state = ISO_SD_READY;
    v.count = 2;
    v.chunk_bytes = 4096;
    v.hidden = 9;
    snprintf(v.images[0].id, sizeof(v.images[0].id), "%s", "00000001");
    snprintf(v.images[1].id, sizeof(v.images[1].id), "%s", "00000002");
    v.images[0].state = ISO_STATE_READY;
    v.images[0].table = (iso_extent_table_t *)(uintptr_t)0x1000; /* ダングリングになりうるポインター */
    v.images[1].table = (iso_extent_table_t *)(uintptr_t)0x2000;
    v.images[ISO_MAX_IMAGES - 1].table = (iso_extent_table_t *)(uintptr_t)0x3000; /* 最後の要素も */
    static iso_upload_t up;
    memset(&up, 0, sizeof(up));
    up.consumed_since = 123456;
    static iso_api_snapshot_t snap;
    memset(&snap, 0xCC, sizeof(snap));
    iso_api_snapshot_update(&snap, &v, &up);
    TEST_ASSERT_EQUAL_INT(ISO_SD_READY, snap.view.sd_state);
    TEST_ASSERT_EQUAL_UINT(2, snap.view.count);
    TEST_ASSERT_EQUAL_UINT32(4096, snap.view.chunk_bytes);
    TEST_ASSERT_EQUAL_UINT32(9, snap.view.hidden);
    TEST_ASSERT_EQUAL_STRING("00000002", snap.view.images[1].id);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, snap.view.images[0].state);
    TEST_ASSERT_EQUAL_UINT64(123456, snap.consumed_since);
    for (size_t i = 0; i < ISO_MAX_IMAGES; i++) TEST_ASSERT_NULL(snap.view.images[i].table);
    iso_api_snapshot_update(&snap, &v, NULL);
    TEST_ASSERT_EQUAL_UINT64(0, snap.consumed_since);
    /* count は ISO_MAX_IMAGES で頭打ち(読む側が配列の外を読まない) */
    v.count = ISO_MAX_IMAGES;
    iso_api_snapshot_update(&snap, &v, &up);
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, snap.view.count);
    v.count = ISO_MAX_IMAGES + 1;
    iso_api_snapshot_update(&snap, &v, &up);
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, snap.view.count);
    v.count = 1000;
    iso_api_snapshot_update(&snap, &v, &up);
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, snap.view.count);
}

/* ---- サービスが起動していない ----------------------------------------------------------------- */

void test_not_started_service_reports_no_card(void) {
    iso_api_request_t rq;
    memset(&rq, 0, sizeof(rq));
    rq.method = ISO_API_M_GET;
    rq.path = "/api/v1/iso";
    iso_api_response_t rsp;
    iso_api_response_init(&rsp, OUT, sizeof(OUT));
    iso_api_handle(NULL, &rq, &rsp);
    TEST_ASSERT_EQUAL_INT(200, rsp.status);
    TEST_ASSERT_EQUAL_STRING(
        "{\"sd\":{\"state\":\"no_card\",\"total_bytes\":null,\"free_bytes\":null},\"images\":[],\"mounted\":null,"
        "\"target\":{\"locked\":false,\"ejected_by_target\":false},\"chunk_size\":131072,\"hidden_images\":0,\"upload\":null}",
        rsp.body);
    TEST_ASSERT_EQUAL_UINT(strlen(rsp.body), rsp.body_len);
    TEST_ASSERT_FALSE(rsp.log); /* GET は記録しない */
    /* 必須のメンバーが欠けた環境も、起動していない */
    iso_api_env_t env;
    memset(&env, 0, sizeof(env));
    iso_api_response_init(&rsp, OUT, sizeof(OUT));
    iso_api_handle(&env, &rq, &rsp);
    TEST_ASSERT_EQUAL_INT(200, rsp.status);
    TEST_ASSERT_NOT_NULL(strstr(rsp.body, "\"state\":\"no_card\""));
}

/* ---- 書き出しの部品 --------------------------------------------------------------------------- */

void test_writer_primitives(void) {
    char b[64];
    iso_w_t w;
    size_t n = 0;
    iso_w_init(&w, b, sizeof(b));
    iso_w_u64(&w, 0);
    iso_w_str(&w, ",");
    iso_w_u64(&w, 9);
    iso_w_str(&w, ",");
    iso_w_u64(&w, 10);
    iso_w_str(&w, ",");
    iso_w_u64(&w, 18446744073709551615ull);
    iso_w_str(&w, ",");
    iso_w_bool(&w, true);
    iso_w_str(&w, ",");
    iso_w_bool(&w, false);
    TEST_ASSERT_EQUAL_INT(0, iso_w_finish(&w, &n));
    TEST_ASSERT_EQUAL_STRING("0,9,10,18446744073709551615,true,false", b);
    TEST_ASSERT_EQUAL_UINT(38, n);

    iso_w_init(&w, b, sizeof(b));
    iso_w_hex(&w, (const uint8_t *)"\x00\x0f\xf0\xab\xcd", 5);
    TEST_ASSERT_EQUAL_INT(0, iso_w_finish(&w, &n));
    TEST_ASSERT_EQUAL_STRING("000ff0abcd", b);

    iso_w_init(&w, b, sizeof(b));
    iso_w_json_str(&w, "a\"b\\c\x01\x1f \x7f~", 10);
    TEST_ASSERT_EQUAL_INT(0, iso_w_finish(&w, &n));
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\\u0001\\u001f \x7f~\"", b);

    /* 溢れ: ちょうどの cap(NUL を含めた)で成功し、1 つ少ないと失敗 */
    iso_w_init(&w, b, 6);
    iso_w_str(&w, "12345");
    TEST_ASSERT_EQUAL_INT(0, iso_w_finish(&w, &n));
    TEST_ASSERT_EQUAL_UINT(5, n);
    iso_w_init(&w, b, 5);
    iso_w_str(&w, "12345");
    TEST_ASSERT_EQUAL_INT(-1, iso_w_finish(&w, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)b[0]);
    iso_w_init(&w, b, 4);
    iso_w_str(&w, "12");
    iso_w_str(&w, "34"); /* 2 回に分けても、NUL の分が残る */
    TEST_ASSERT_EQUAL_INT(-1, iso_w_finish(&w, NULL));
    iso_w_init(&w, NULL, 10);
    TEST_ASSERT_FALSE(w.ok);
    iso_w_init(&w, b, 0);
    TEST_ASSERT_FALSE(w.ok);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_example_of_the_spec);
    RUN_TEST(test_an_empty_ready_card);
    RUN_TEST(test_sd_states_other_than_ready);
    RUN_TEST(test_an_unknown_sd_state_is_reported_as_error);
    RUN_TEST(test_total_and_free_are_null_until_sampled);
    RUN_TEST(test_chunk_size_and_hidden_images);
    RUN_TEST(test_every_image_state_and_invalid_reason);
    RUN_TEST(test_meta_corrupt_image_has_the_id_as_its_name_and_null_sizes);
    RUN_TEST(test_names_are_json_escaped);
    RUN_TEST(test_mounted_target_flags);
    RUN_TEST(test_mounted_image_missing_from_the_view_is_named_by_its_id);
    RUN_TEST(test_mounted_name_is_found_among_several_images);
    RUN_TEST(test_upload_slot_states);
    RUN_TEST(test_the_worst_case_fits_in_the_response_buffer);
    RUN_TEST(test_small_buffers_fail_cleanly_everywhere);
    RUN_TEST(test_snapshot_update_copies_the_view_and_drops_the_table_pointers);
    RUN_TEST(test_not_started_service_reports_no_card);
    RUN_TEST(test_writer_primitives);
    return UNITY_END();
}

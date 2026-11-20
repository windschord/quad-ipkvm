/*
 * iso_api の経路(iso_api_route、iso_api_allow_for_path)、Content-Type の分類、u64 の解析、id の変換。期待値は手書き。
 * WEB_API.md 8 章: 定義していないメソッドは 405(Allow 付き)、GET /api/v1/iso/<id> は 405、<id> の形式不正は 404(段 3)。
 */
#include "unity.h"
#include "iso_api.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define OK ISO_API_ROUTE_OK
#define NF ISO_API_ROUTE_NOT_FOUND
#define MT ISO_API_ROUTE_METHOD

typedef struct {
    const char *path;
    iso_api_method_t method;
    iso_api_route_result_t result;
    iso_api_endpoint_t ep;
    const char *allow; /* METHOD のとき */
    bool has_id;
    bool id_ok;
    const char *id;
} row_t;

#define G ISO_API_M_GET
#define P ISO_API_M_POST
#define U ISO_API_M_PUT
#define D ISO_API_M_DELETE
#define X ISO_API_M_OTHER

static const row_t ROWS[] = {
    /* /api/v1/iso */
    {"/api/v1/iso", G, OK, ISO_API_EP_LIST, NULL, false, false, ""},
    {"/api/v1/iso", P, OK, ISO_API_EP_BEGIN, NULL, false, false, ""},
    {"/api/v1/iso", U, MT, ISO_API_EP_NONE, "GET, POST", false, false, ""},
    {"/api/v1/iso", D, MT, ISO_API_EP_NONE, "GET, POST", false, false, ""},
    {"/api/v1/iso", X, MT, ISO_API_EP_NONE, "GET, POST", false, false, ""},
    {"/api/v1/iso?x=1", G, OK, ISO_API_EP_LIST, NULL, false, false, ""},
    {"/api/v1/iso?", P, OK, ISO_API_EP_BEGIN, NULL, false, false, ""},
    /* 固定の POST */
    {"/api/v1/iso/mount", P, OK, ISO_API_EP_MOUNT, NULL, false, false, ""},
    {"/api/v1/iso/mount", G, MT, ISO_API_EP_NONE, "POST", false, false, ""},
    {"/api/v1/iso/mount", U, MT, ISO_API_EP_NONE, "POST", false, false, ""},
    {"/api/v1/iso/mount", D, MT, ISO_API_EP_NONE, "POST", false, false, ""},
    {"/api/v1/iso/mount?a=b", P, OK, ISO_API_EP_MOUNT, NULL, false, false, ""},
    {"/api/v1/iso/eject", P, OK, ISO_API_EP_EJECT, NULL, false, false, ""},
    {"/api/v1/iso/eject", G, MT, ISO_API_EP_NONE, "POST", false, false, ""},
    {"/api/v1/iso/eject", X, MT, ISO_API_EP_NONE, "POST", false, false, ""},
    {"/api/v1/iso/rescan", P, OK, ISO_API_EP_RESCAN, NULL, false, false, ""},
    {"/api/v1/iso/rescan", D, MT, ISO_API_EP_NONE, "POST", false, false, ""},
    /* <id> */
    {"/api/v1/iso/a1b2c3d4", U, OK, ISO_API_EP_PUT, NULL, true, true, "a1b2c3d4"},
    {"/api/v1/iso/a1b2c3d4", D, OK, ISO_API_EP_DELETE, NULL, true, true, "a1b2c3d4"},
    {"/api/v1/iso/a1b2c3d4", G, MT, ISO_API_EP_NONE, "PUT, DELETE", true, true, "a1b2c3d4"}, /* 単体の GET は定義しない */
    {"/api/v1/iso/a1b2c3d4", P, MT, ISO_API_EP_NONE, "PUT, DELETE", true, true, "a1b2c3d4"},
    {"/api/v1/iso/a1b2c3d4", X, MT, ISO_API_EP_NONE, "PUT, DELETE", true, true, "a1b2c3d4"},
    {"/api/v1/iso/00000000", U, OK, ISO_API_EP_PUT, NULL, true, true, "00000000"},
    {"/api/v1/iso/ffffffff", D, OK, ISO_API_EP_DELETE, NULL, true, true, "ffffffff"},
    {"/api/v1/iso/a1b2c3d4?x", U, OK, ISO_API_EP_PUT, NULL, true, true, "a1b2c3d4"},
    /* <id> の形式不正: 経路は成立し、id_ok が偽(段 3 で 404)。メソッドの不一致は、先に 405 */
    {"/api/v1/iso/A1B2C3D4", U, OK, ISO_API_EP_PUT, NULL, true, false, ""},
    {"/api/v1/iso/a1b2c3d", D, OK, ISO_API_EP_DELETE, NULL, true, false, ""},
    {"/api/v1/iso/a1b2c3d45", U, OK, ISO_API_EP_PUT, NULL, true, false, ""},
    {"/api/v1/iso/a1b2c3dg", D, OK, ISO_API_EP_DELETE, NULL, true, false, ""},
    {"/api/v1/iso/zzzzzzzz", U, OK, ISO_API_EP_PUT, NULL, true, false, ""},
    {"/api/v1/iso/mounts", U, OK, ISO_API_EP_PUT, NULL, true, false, ""},
    {"/api/v1/iso/Mount", P, MT, ISO_API_EP_NONE, "PUT, DELETE", true, false, ""},
    {"/api/v1/iso/zzzzzzzz", G, MT, ISO_API_EP_NONE, "PUT, DELETE", true, false, ""},
    {"/api/v1/iso/a1b2c3d%34", U, OK, ISO_API_EP_PUT, NULL, true, false, ""}, /* パーセントエンコードは解釈しない */
    /* finish */
    {"/api/v1/iso/a1b2c3d4/finish", P, OK, ISO_API_EP_FINISH, NULL, true, true, "a1b2c3d4"},
    {"/api/v1/iso/a1b2c3d4/finish", G, MT, ISO_API_EP_NONE, "POST", true, true, "a1b2c3d4"},
    {"/api/v1/iso/a1b2c3d4/finish", U, MT, ISO_API_EP_NONE, "POST", true, true, "a1b2c3d4"},
    {"/api/v1/iso/a1b2c3d4/finish", D, MT, ISO_API_EP_NONE, "POST", true, true, "a1b2c3d4"},
    {"/api/v1/iso/a1b2c3d4/finish?q=1", P, OK, ISO_API_EP_FINISH, NULL, true, true, "a1b2c3d4"},
    {"/api/v1/iso/xyz/finish", P, OK, ISO_API_EP_FINISH, NULL, true, false, ""},
    {"/api/v1/iso/mount/finish", P, OK, ISO_API_EP_FINISH, NULL, true, false, ""},
    {"/api/v1/iso/A1B2C3D4/finish", P, OK, ISO_API_EP_FINISH, NULL, true, false, ""},
    /* 定義していない経路は 404(メソッドによらない) */
    {"/api/v1/iso/", G, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/iso/", P, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/iso//finish", P, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/iso//", P, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/iso/mount/", P, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/iso/a1b2c3d4/", U, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/iso/a1b2c3d4/finish/", P, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/iso/a1b2c3d4/finishx", P, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/iso/a1b2c3d4/Finish", P, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/iso/a1b2c3d4/finis", P, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/iso/a1b2c3d4/other", P, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/iso/a1b2c3d4/finish/x", P, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/iso/mount/eject", P, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/isoX", G, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/isomount", P, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/is", G, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/i", G, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v1/kvm/status", G, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/api/v2/iso", G, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/API/V1/ISO", G, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"api/v1/iso", G, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"/", G, NF, ISO_API_EP_NONE, NULL, false, false, ""},
    {"", G, NF, ISO_API_EP_NONE, NULL, false, false, ""},
};

void test_route_table(void) {
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
        const row_t *r = &ROWS[i];
        iso_api_route_t out;
        memset(&out, 0xA5, sizeof(out));
        iso_api_route(r->method, r->path, &out);
        char msg[128];
        snprintf(msg, sizeof(msg), "row %zu: %s", i, r->path);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r->result, out.result, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r->ep, out.ep, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r->has_id, out.has_id, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r->id_ok, out.id_ok, msg);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(r->id, out.id, msg);
        if (r->result == MT) {
            TEST_ASSERT_NOT_NULL_MESSAGE(out.allow, msg);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(r->allow, out.allow, msg);
        } else {
            TEST_ASSERT_NULL_MESSAGE(out.allow, msg);
        }
    }
}

void test_route_null_path_is_not_found(void) {
    iso_api_route_t out;
    iso_api_route(ISO_API_M_GET, NULL, &out);
    TEST_ASSERT_EQUAL_INT(NF, out.result);
    TEST_ASSERT_EQUAL_INT(ISO_API_EP_NONE, out.ep);
}

void test_allow_for_path(void) {
    TEST_ASSERT_EQUAL_STRING("GET, POST", iso_api_allow_for_path("/api/v1/iso"));
    TEST_ASSERT_EQUAL_STRING("GET, POST", iso_api_allow_for_path("/api/v1/iso?x=1"));
    TEST_ASSERT_EQUAL_STRING("POST", iso_api_allow_for_path("/api/v1/iso/mount"));
    TEST_ASSERT_EQUAL_STRING("POST", iso_api_allow_for_path("/api/v1/iso/eject"));
    TEST_ASSERT_EQUAL_STRING("POST", iso_api_allow_for_path("/api/v1/iso/rescan"));
    TEST_ASSERT_EQUAL_STRING("PUT, DELETE", iso_api_allow_for_path("/api/v1/iso/a1b2c3d4"));
    TEST_ASSERT_EQUAL_STRING("PUT, DELETE", iso_api_allow_for_path("/api/v1/iso/whatever"));
    TEST_ASSERT_EQUAL_STRING("POST", iso_api_allow_for_path("/api/v1/iso/a1b2c3d4/finish"));
    TEST_ASSERT_NULL(iso_api_allow_for_path("/api/v1/iso/"));
    TEST_ASSERT_NULL(iso_api_allow_for_path("/api/v1/iso/a1b2c3d4/other"));
    TEST_ASSERT_NULL(iso_api_allow_for_path("/api/v1/kvm/status"));
    TEST_ASSERT_NULL(iso_api_allow_for_path("/"));
    TEST_ASSERT_NULL(iso_api_allow_for_path(NULL));
}

void test_endpoints_with_a_json_body(void) {
    TEST_ASSERT_TRUE(iso_api_ep_has_body(ISO_API_EP_BEGIN));
    TEST_ASSERT_TRUE(iso_api_ep_has_body(ISO_API_EP_MOUNT));
    TEST_ASSERT_TRUE(iso_api_ep_has_body(ISO_API_EP_FINISH));
    TEST_ASSERT_TRUE(iso_api_ep_has_body(ISO_API_EP_EJECT));
    TEST_ASSERT_TRUE(iso_api_ep_has_body(ISO_API_EP_RESCAN));
    TEST_ASSERT_FALSE(iso_api_ep_has_body(ISO_API_EP_LIST));
    TEST_ASSERT_FALSE(iso_api_ep_has_body(ISO_API_EP_PUT));
    TEST_ASSERT_FALSE(iso_api_ep_has_body(ISO_API_EP_DELETE));
    TEST_ASSERT_FALSE(iso_api_ep_has_body(ISO_API_EP_NONE));
}

/* ---- Content-Type ---------------------------------------------------------------------- */

typedef struct {
    const char *v;
    iso_api_ctype_t t;
} ct_row_t;

static const ct_row_t CT[] = {
    {"application/json", ISO_API_CT_JSON},
    {"Application/JSON", ISO_API_CT_JSON},
    {"APPLICATION/JSON", ISO_API_CT_JSON},
    {"application/json; charset=utf-8", ISO_API_CT_JSON},
    {"application/json;charset=utf-8", ISO_API_CT_JSON},
    {"application/json ;charset=utf-8", ISO_API_CT_JSON},
    {" application/json", ISO_API_CT_JSON},
    {"\tapplication/json", ISO_API_CT_JSON},
    {"application/json ", ISO_API_CT_JSON},
    {"application/json;", ISO_API_CT_JSON},
    {"application/octet-stream", ISO_API_CT_OCTET},
    {"Application/Octet-Stream", ISO_API_CT_OCTET},
    {"application/octet-stream; foo=bar", ISO_API_CT_OCTET},
    {"  application/octet-stream  ", ISO_API_CT_OCTET},
    {"application/jsonx", ISO_API_CT_OTHER},
    {"application/json2", ISO_API_CT_OTHER},
    {"application/json,text/plain", ISO_API_CT_OTHER},
    {"application/jso", ISO_API_CT_OTHER},
    {"application/octet-streamx", ISO_API_CT_OTHER},
    {"application/octet-stream,x", ISO_API_CT_OTHER},
    {"application/octet", ISO_API_CT_OTHER},
    {"application/x-json", ISO_API_CT_OTHER},
    {"text/plain", ISO_API_CT_OTHER},
    {"text/json", ISO_API_CT_OTHER},
    {"json", ISO_API_CT_OTHER},
    {"", ISO_API_CT_OTHER},
    {" ", ISO_API_CT_OTHER},
    {";application/json", ISO_API_CT_OTHER},
    {"x application/json", ISO_API_CT_OTHER},
};

void test_ctype_table(void) {
    for (size_t i = 0; i < sizeof(CT) / sizeof(CT[0]); i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "row %zu: '%s'", i, CT[i].v);
        TEST_ASSERT_EQUAL_INT_MESSAGE(CT[i].t, iso_api_ctype_parse(CT[i].v), msg);
    }
    TEST_ASSERT_EQUAL_INT(ISO_API_CT_NONE, iso_api_ctype_parse(NULL));
}

/* ---- u64 ----------------------------------------------------------------------------- */

static void expect_u64(const char *s, size_t len, bool ok, uint64_t v) {
    uint64_t out = 0x1234;
    int rc = iso_api_parse_u64(s, len, &out);
    char msg[64];
    snprintf(msg, sizeof(msg), "'%s' len %zu", s ? s : "NULL", len);
    if (ok) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, msg);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(v, out, msg);
    } else {
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, rc, msg);
    }
}

void test_parse_u64(void) {
    expect_u64("0", 1, true, 0);
    expect_u64("1", 1, true, 1);
    expect_u64("9", 1, true, 9);
    expect_u64("10", 2, true, 10);
    expect_u64("34816", 5, true, 34816);
    expect_u64("6126805504", 10, true, 6126805504ull);
    expect_u64("9223372036854775807", 19, true, 9223372036854775807ull);
    expect_u64("9223372036854775808", 19, true, 9223372036854775808ull);
    expect_u64("18446744073709551615", 20, true, 18446744073709551615ull);
    expect_u64("18446744073709551616", 20, false, 0); /* u64 の超過 */
    expect_u64("18446744073709551619", 20, false, 0);
    expect_u64("18446744073709551620", 20, false, 0);
    expect_u64("19999999999999999999", 20, false, 0);
    expect_u64("99999999999999999999", 20, false, 0);
    expect_u64("184467440737095516150", 21, false, 0); /* 21 桁 */
    expect_u64("00", 2, false, 0);                     /* 先頭ゼロ */
    expect_u64("01", 2, false, 0);
    expect_u64("007", 3, false, 0);
    expect_u64("", 0, false, 0);
    expect_u64("-1", 2, false, 0);
    expect_u64("+1", 2, false, 0);
    expect_u64(" 1", 2, false, 0);
    expect_u64("1 ", 2, false, 0);
    expect_u64("1a", 2, false, 0);
    expect_u64("a1", 2, false, 0);
    expect_u64("1.0", 3, false, 0);
    expect_u64("1e3", 3, false, 0);
    expect_u64("0x10", 4, false, 0);
    expect_u64("/", 1, false, 0); /* '0' の 1 つ前の文字 */
    expect_u64(":", 1, false, 0); /* '9' の 1 つ後の文字 */
    expect_u64("123abc", 3, true, 123); /* len で切る */
    expect_u64("1\0" "2", 3, false, 0);  /* NUL */
    expect_u64(NULL, 1, false, 0);
    TEST_ASSERT_EQUAL_INT(-1, iso_api_parse_u64("1", 1, NULL));
}

/* ---- id の変換 ------------------------------------------------------------------------- */

void test_id_conversions(void) {
    uint32_t v = 0xFFFFFFFFu;
    TEST_ASSERT_TRUE(iso_api_id_to_u32("a1b2c3d4", &v));
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4u, v);
    TEST_ASSERT_TRUE(iso_api_id_to_u32("00000000", &v));
    TEST_ASSERT_EQUAL_HEX32(0u, v);
    TEST_ASSERT_TRUE(iso_api_id_to_u32("ffffffff", &v));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, v);
    TEST_ASSERT_TRUE(iso_api_id_to_u32("0000000f", &v));
    TEST_ASSERT_EQUAL_HEX32(0xFu, v);
    TEST_ASSERT_TRUE(iso_api_id_to_u32("00000010", &v));
    TEST_ASSERT_EQUAL_HEX32(0x10u, v);
    TEST_ASSERT_TRUE(iso_api_id_to_u32("0f9e8d7c", &v));
    TEST_ASSERT_EQUAL_HEX32(0x0f9e8d7cu, v);
    TEST_ASSERT_TRUE(iso_api_id_to_u32("90000009", &v));
    TEST_ASSERT_EQUAL_HEX32(0x90000009u, v);
    v = 0x77;
    TEST_ASSERT_FALSE(iso_api_id_to_u32("A1B2C3D4", &v));
    TEST_ASSERT_FALSE(iso_api_id_to_u32("a1b2c3d", &v));
    TEST_ASSERT_FALSE(iso_api_id_to_u32("a1b2c3d45", &v));
    TEST_ASSERT_FALSE(iso_api_id_to_u32("a1b2c3dg", &v));
    TEST_ASSERT_FALSE(iso_api_id_to_u32("", &v));
    TEST_ASSERT_FALSE(iso_api_id_to_u32(NULL, &v));
    TEST_ASSERT_EQUAL_HEX32(0x77u, v); /* 失敗のとき、変えない */

    char id[ISO_ID_LEN + 1];
    memset(id, 'X', sizeof(id));
    iso_api_u32_to_id(0xa1b2c3d4u, id);
    TEST_ASSERT_EQUAL_STRING("a1b2c3d4", id);
    iso_api_u32_to_id(0, id);
    TEST_ASSERT_EQUAL_STRING("00000000", id);
    iso_api_u32_to_id(0xFu, id);
    TEST_ASSERT_EQUAL_STRING("0000000f", id);
    iso_api_u32_to_id(0xF0000000u, id);
    TEST_ASSERT_EQUAL_STRING("f0000000", id);
    iso_api_u32_to_id(0xdeadbeefu, id);
    TEST_ASSERT_EQUAL_STRING("deadbeef", id);
    iso_api_u32_to_id(0x12345678u, id);
    TEST_ASSERT_EQUAL_STRING("12345678", id);
    iso_api_u32_to_id(0x9abcdef0u, id);
    TEST_ASSERT_EQUAL_STRING("9abcdef0", id);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_route_table);
    RUN_TEST(test_route_null_path_is_not_found);
    RUN_TEST(test_allow_for_path);
    RUN_TEST(test_endpoints_with_a_json_body);
    RUN_TEST(test_ctype_table);
    RUN_TEST(test_parse_u64);
    RUN_TEST(test_id_conversions);
    return UNITY_END();
}

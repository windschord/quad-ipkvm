/*
 * iso_api の固定シードのファズ。
 *   1. JSON の本体: 正しい本体のバイト列を壊して(反転、削除、挿入、切り詰め)解析する。受理したものは、仕様の制約を満たす
 *      (名前の規則、size の範囲、cursor の範囲、id の形式)。ランダムな名前を、ランダムにエスケープして、往復する(復号したバイト列が一致する)。
 *   2. 要求列: ランダムな要求(正しいもの、壊したもの)と、ワーカー・時刻・メディア・SD・エラーの注入を混ぜる。要求のたびに、不変条件を調べる:
 *      - 状態が表の集合の中にある(500 は出ない)。本文は JSON の形(`{` で始まり `}` で終わる、引用符が釣り合う、生の制御文字がない)で、
 *        body_len が長さと一致し、400 以上は `{"error":"` で始まる。
 *      - ロックの順序と釣り合い(state_lock の入れ子、state_lock を持ったままの op_trylock がない。要求の後で、どちらも放している)。
 *      - 拒否(400 / 404 / 405 / 409 / 411 / 413 / 415 / 422 / 503 busy)は、カタログ、メディア、ワーカーのキューを変えない。
 *        PUT の precheck は、受理でも拒否でも変えない。
 *      - GET の一覧が、カタログと一致する(イメージの数、マウント中の有無)。
 *   最後に、後始末(ワーカーを流し切り、leak がない)。期待値は、実装から計算しない。
 */
#include "api_common.h"

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    uint64_t s;
} rng_t;
static uint64_t rnd_next(rng_t *r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 7;
    r->s ^= r->s << 17;
    return r->s;
}
static uint32_t rnd(rng_t *r, uint32_t n) { return (uint32_t)(rnd_next(r) % n); }

/* ---- 1. JSON の本体 ---------------------------------------------------------------------- */

static size_t mutate(rng_t *r, char *buf, size_t len, size_t cap) {
    uint32_t kind = rnd(r, 6);
    if (len == 0) return len;
    size_t pos = rnd(r, (uint32_t)len);
    switch (kind) {
    case 0: buf[pos] = (char)rnd(r, 256); break;
    case 1: buf[pos] ^= (char)(1u << rnd(r, 8)); break;
    case 2: /* 削除 */
        memmove(buf + pos, buf + pos + 1, len - pos - 1);
        return len - 1;
    case 3: /* 挿入 */
        if (len + 1 < cap) {
            memmove(buf + pos + 1, buf + pos, len - pos);
            buf[pos] = (char)rnd(r, 256);
            return len + 1;
        }
        break;
    case 4: return pos; /* 切り詰め */
    default: { /* 記号に置き換え */
        static const char SYM[] = "{}[]\",:\\ u0123456789abcdefnulltruefalse-.e";
        buf[pos] = SYM[rnd(r, (uint32_t)sizeof(SYM) - 1)];
        break;
    }
    }
    return len;
}

static void check_accepted(iso_api_schema_t sc, const iso_api_body_t *b) {
    switch (sc) {
    case ISO_API_SCHEMA_BEGIN:
        TEST_ASSERT_TRUE(iso_name_valid(b->name, b->name_len));
        TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)b->name[b->name_len]);
        TEST_ASSERT_TRUE(b->size >= ISO_IMAGE_BYTES_MIN && b->size <= ISO_IMAGE_BYTES_MAX);
        break;
    case ISO_API_SCHEMA_MOUNT: TEST_ASSERT_TRUE(iso_id_valid(b->id)); break;
    case ISO_API_SCHEMA_RESCAN: TEST_ASSERT_TRUE(b->cursor <= 65535u); break;
    default: break;
    }
}

void test_fuzz_mutated_bodies(void) {
    static const struct {
        iso_api_schema_t sc;
        const char *body;
    } SEEDS[] = {
        {ISO_API_SCHEMA_BEGIN, "{\"name\":\"ubuntu-24.04\",\"size\":6126805504,\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}"},
        {ISO_API_SCHEMA_BEGIN, "{\"name\":\"caf\\u00e9 \\ud83d\\ude00\",\"size\":34816}"},
        {ISO_API_SCHEMA_BEGIN, " {\"size\" : 69632 , \"name\" : \"a b\\\"c\" } "},
        {ISO_API_SCHEMA_MOUNT, "{\"id\":\"a1b2c3d4\",\"force\":true}"},
        {ISO_API_SCHEMA_MOUNT, "{\"id\":\"a1b2c3d4\"}"},
        {ISO_API_SCHEMA_EJECT, "{\"force\":false}"},
        {ISO_API_SCHEMA_RESCAN, "{\"cursor\":65535}"},
        {ISO_API_SCHEMA_RESCAN, "{}"},
        {ISO_API_SCHEMA_FINISH, "{}"},
    };
    rng_t r = {0x9E3779B97F4A7C15ull};
    unsigned accepted = 0, rejected = 0;
    for (int it = 0; it < 60000; it++) {
        size_t k = rnd(&r, sizeof(SEEDS) / sizeof(SEEDS[0]));
        char buf[ISO_API_BODY_MAX + 8];
        size_t len = strlen(SEEDS[k].body);
        memcpy(buf, SEEDS[k].body, len);
        unsigned n = 1 + rnd(&r, 4);
        for (unsigned m = 0; m < n; m++) len = mutate(&r, buf, len, sizeof(buf) - 1);
        iso_api_body_t b;
        int rc = iso_api_parse_body(SEEDS[k].sc, buf, len, &b);
        if (rc == 0) {
            accepted++;
            check_accepted(SEEDS[k].sc, &b);
        } else {
            rejected++;
        }
    }
    TEST_ASSERT_TRUE(accepted > 1000); /* 受理もそれなりにある(壊し方が強すぎない) */
    TEST_ASSERT_TRUE(rejected > 1000);
}

/* ランダムな正しい名前を、ランダムにエスケープして、往復する */
static size_t encode_cp(uint32_t cp, uint8_t *o) {
    if (cp < 0x80) {
        o[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (uint8_t)(0xC0 | (cp >> 6));
        o[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (uint8_t)(0xE0 | (cp >> 12));
        o[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (uint8_t)(0xF0 | (cp >> 18));
    o[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

static uint32_t random_cp(rng_t *r) {
    for (;;) {
        uint32_t cp;
        switch (rnd(r, 5)) {
        case 0: cp = 0x20 + rnd(r, 0x5F); break;           /* ASCII の印字文字 */
        case 1: cp = 0xA0 + rnd(r, 0x700 - 0xA0); break;   /* C1 の後ろの 2 バイト領域 */
        case 2: cp = 0x800 + rnd(r, 0xD800 - 0x800); break; /* 3 バイト領域(サロゲートの前) */
        case 3: cp = 0xE000 + rnd(r, 0x10000 - 0xE000); break;
        default: cp = 0x10000 + rnd(r, 0x100000); break;   /* 4 バイト領域 */
        }
        if (cp == '/' || cp == '\\') continue;
        return cp;
    }
}

void test_fuzz_random_names_round_trip_through_any_escaping(void) {
    rng_t r = {0x1234567887654321ull};
    for (int it = 0; it < 4000; it++) {
        uint8_t name[80];
        size_t nl = 0;
        char json[ISO_API_BODY_MAX + 1];
        size_t jl = (size_t)snprintf(json, sizeof(json), "{\"name\":\"");
        uint32_t want_cps = 1 + rnd(&r, 20);
        for (uint32_t c = 0; c < want_cps; c++) {
            uint32_t cp = random_cp(&r);
            uint8_t enc[4];
            size_t el = encode_cp(cp, enc);
            if (nl + el > ISO_NAME_MAX_BYTES) break;
            /* 先頭と末尾が空白にならないように、ASCII の空白は使わない */
            if (cp == 0x20) cp = 0x41, el = encode_cp(cp, enc);
            memcpy(name + nl, enc, el);
            nl += el;
            bool esc = rnd(&r, 2) == 0;
            if (cp == '"') {
                jl += (size_t)snprintf(json + jl, sizeof(json) - jl, "\\\"");
            } else if (esc && cp >= 0x10000) {
                uint32_t v = cp - 0x10000;
                jl += (size_t)snprintf(json + jl, sizeof(json) - jl, rnd(&r, 2) ? "\\u%04x\\u%04x" : "\\u%04X\\u%04X", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
            } else if (esc) {
                jl += (size_t)snprintf(json + jl, sizeof(json) - jl, rnd(&r, 2) ? "\\u%04x" : "\\u%04X", cp);
            } else {
                memcpy(json + jl, enc, el);
                jl += el;
            }
        }
        if (nl == 0) continue;
        jl += (size_t)snprintf(json + jl, sizeof(json) - jl, "\",\"size\":34816}");
        iso_api_body_t b;
        int rc = iso_api_parse_body(ISO_API_SCHEMA_BEGIN, json, jl, &b);
        bool name_ok = iso_name_valid((const char *)name, nl); /* 符号位置の選び方で C1 などは除いてあるので、通る */
        TEST_ASSERT_TRUE(name_ok);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, json);
        TEST_ASSERT_EQUAL_UINT(nl, b.name_len);
        TEST_ASSERT_EQUAL_MEMORY(name, b.name, nl);
    }
}

/* ---- 2. 要求列 ---------------------------------------------------------------------------- */

static const char *IDS[] = {"aaaaaaa1", "bbbbbbb2", "ccccccc3", "ddddddd4", "eeeeeee5", "11110001", "11110002", "11110003", "ffffffff", "A1B2C3D4", "zz"};
#define N_IDS (sizeof(IDS) / sizeof(IDS[0]))

typedef struct {
    size_t count;
    char ids[ISO_MAX_IMAGES][ISO_ID_LEN + 1];
    int state[ISO_MAX_IMAGES];
    uint64_t received[ISO_MAX_IMAGES];
    bool loaded;
    uint32_t mid;
    bool prevent, ejt;
    size_t qn;
    bool slot;
    char slot_id[ISO_ID_LEN + 1];
} fp_t;

static void fingerprint(fp_t *f) {
    memset(f, 0, sizeof(*f));
    f->count = CAT.count;
    for (size_t i = 0; i < CAT.count; i++) {
        memcpy(f->ids[i], CAT.images[i].id, ISO_ID_LEN + 1);
        f->state[i] = CAT.images[i].state;
        f->received[i] = CAT.images[i].received;
    }
    iso_media_status_t s;
    iso_media_get_status(&MEDIA, &s);
    f->loaded = s.mounted;
    f->mid = s.mounted ? s.id : 0;
    f->prevent = iso_media_prevent_removal(&MEDIA);
    f->ejt = s.ejected_by_target;
    f->qn = iso_core_queue_len(&CORE);
    f->slot = iso_upload_slot_busy(api_up());
    const char *sid = iso_upload_slot_id(api_up());
    if (sid) memcpy(f->slot_id, sid, ISO_ID_LEN + 1);
}

static void check_json_shape(const char *b, size_t len) {
    TEST_ASSERT_EQUAL_UINT(strlen(b), len);
    TEST_ASSERT_TRUE(len >= 2);
    TEST_ASSERT_EQUAL_CHAR('{', b[0]);
    TEST_ASSERT_EQUAL_CHAR('}', b[len - 1]);
    bool in = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)b[i];
        if (in) {
            TEST_ASSERT_TRUE_MESSAGE(c >= 0x20, "raw control character inside a JSON string");
            if (c == '\\') {
                i++;
                TEST_ASSERT_TRUE(i < len);
            } else if (c == '"') {
                in = false;
            }
        } else if (c == '"') {
            in = true;
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(in, "unbalanced quotes");
}

static const char *PATHS[] = {"/api/v1/iso", "/api/v1/iso/mount", "/api/v1/iso/eject", "/api/v1/iso/rescan", "/api/v1/iso/",
                              "/api/v1/iso/x", "/api/v1/iso/aaaaaaa1/other", "/nope"};

static int g_calls;
static unsigned g_status_seen[600];

/* mode: 0 = 拒否のときだけ、状態が変わっていないことを確かめる。1 = 必ず確かめる。2 = 確かめない */
static void check_response(const fp_t *before, int mode) {
    TEST_ASSERT_EQUAL_INT(0, SL_DEPTH);
    TEST_ASSERT_EQUAL_INT(0, OL_DEPTH);
    TEST_ASSERT_FALSE(LOCK_VIOLATION);
    static const int OKS[] = {200, 201, 202, 400, 404, 405, 409, 411, 413, 415, 422, 503, 507};
    bool known = false;
    for (size_t i = 0; i < sizeof(OKS) / sizeof(OKS[0]); i++) known |= (RSP.status == OKS[i]);
    char msg[160];
    snprintf(msg, sizeof(msg), "status %d body %.100s", RSP.status, RSP.body);
    TEST_ASSERT_TRUE_MESSAGE(known, msg);
    g_status_seen[RSP.status]++;
    check_json_shape(RSP.body, RSP.body_len);
    if (RSP.status >= 400) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(RSP.body, "{\"error\":\"", 10), msg);
    }
    if (RSP.status == 405) TEST_ASSERT_NOT_NULL(RSP.allow);
    if (RSP.has_retry_after) {
        TEST_ASSERT_EQUAL_INT(503, RSP.status);
        TEST_ASSERT_EQUAL_UINT32(1, RSP.retry_after_s);
    }
    bool rejected = RSP.status == 400 || RSP.status == 404 || RSP.status == 405 || RSP.status == 409 || RSP.status == 411 || RSP.status == 413 ||
                    RSP.status == 415 || RSP.status == 422 || (RSP.status == 503 && RSP.has_retry_after);
    if (mode == 1 || (mode == 0 && rejected)) {
        fp_t after;
        fingerprint(&after);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before, &after, sizeof(after), msg);
    }
}

/* GET の一覧が、カタログと一致する */
static void check_list_matches_catalog(void) {
    api_get();
    TEST_ASSERT_EQUAL_INT(200, RSP.status);
    check_json_shape(RSP.body, RSP.body_len);
    size_t n = 0;
    for (const char *p = RSP.body; (p = strstr(p, "\"invalid_reason\":")) != NULL; p++) n++;
    size_t expect = (SD_STATE == ISO_SD_READY) ? CAT.count : 0;
    TEST_ASSERT_EQUAL_UINT(expect, n);
    iso_media_status_t s;
    iso_media_get_status(&MEDIA, &s);
    TEST_ASSERT_EQUAL_INT(s.mounted, strstr(RSP.body, "\"mounted\":{") != NULL);
}

static const char *VALID_BODIES[] = {
    "{\"name\":\"fz-1\",\"size\":34816}",
    "{\"name\":\"fz-2\",\"size\":69632}",
    "{\"name\":\"alpha\",\"size\":69632}",
    "{\"name\":\"fz-3\",\"size\":100000,\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}",
    "{\"name\":\"huge\",\"size\":17179869184}",
    "{\"id\":\"aaaaaaa1\"}",
    "{\"id\":\"bbbbbbb2\",\"force\":true}",
    "{\"id\":\"ccccccc3\"}",
    "{\"id\":\"eeeeeee5\",\"force\":false}",
    "{\"id\":\"11110001\"}",
    "{}",
    "{\"force\":true}",
    "{\"cursor\":0}",
    "{\"cursor\":3}",
};

static void random_request(rng_t *r, iso_api_request_t *rq, char *bodybuf, char *pathbuf, char *offbuf) {
    memset(rq, 0, sizeof(*rq));
    uint32_t kind = rnd(r, 10);
    static const iso_api_method_t METHODS[] = {ISO_API_M_GET, ISO_API_M_POST, ISO_API_M_PUT, ISO_API_M_DELETE, ISO_API_M_OTHER};
    if (kind < 6) { /* 正しい形に近いもの */
        uint32_t ep = rnd(r, 6);
        const char *id = IDS[rnd(r, N_IDS)];
        switch (ep) {
        case 0:
            rq->method = ISO_API_M_POST;
            snprintf(pathbuf, 64, "/api/v1/iso");
            break;
        case 1:
            rq->method = ISO_API_M_POST;
            snprintf(pathbuf, 64, "/api/v1/iso/%s", rnd(r, 3) == 0 ? "mount" : (rnd(r, 2) ? "eject" : "rescan"));
            break;
        case 2:
            rq->method = ISO_API_M_POST;
            snprintf(pathbuf, 64, "/api/v1/iso/%s/finish", id);
            break;
        case 3:
            rq->method = ISO_API_M_DELETE;
            snprintf(pathbuf, 64, "/api/v1/iso/%s", id);
            break;
        case 4:
            rq->method = ISO_API_M_GET;
            snprintf(pathbuf, 64, "/api/v1/iso");
            break;
        default:
            rq->method = ISO_API_M_PUT;
            snprintf(pathbuf, 64, "/api/v1/iso/%s", id);
            break;
        }
        if (rnd(r, 8) == 0) rq->method = METHODS[rnd(r, 5)]; /* ときどき、メソッドを違える */
    } else {
        rq->method = METHODS[rnd(r, 5)];
        snprintf(pathbuf, 64, "%s", PATHS[rnd(r, sizeof(PATHS) / sizeof(PATHS[0]))]);
    }
    rq->path = pathbuf;
    switch (rnd(r, 8)) {
    case 0: rq->content_type = NULL; break;
    case 1: rq->content_type = "text/plain"; break;
    case 2: rq->content_type = OCTET_CT; break;
    default: rq->content_type = rq->method == ISO_API_M_PUT ? OCTET_CT : JSON_CT; break;
    }
    /* 本体: 正しい本体、または壊したもの */
    size_t bl = 0;
    if (rnd(r, 5) != 0) {
        const char *b = VALID_BODIES[rnd(r, sizeof(VALID_BODIES) / sizeof(VALID_BODIES[0]))];
        bl = strlen(b);
        memcpy(bodybuf, b, bl);
        if (rnd(r, 3) == 0) bl = mutate(r, bodybuf, bl, ISO_API_BODY_MAX);
    }
    bodybuf[bl] = '\0';
    rq->body = bodybuf;
    rq->body_len = bl;
    rq->has_content_length = rnd(r, 10) != 0;
    rq->content_length = (rq->method == ISO_API_M_PUT) ? (rnd(r, 8) == 0 ? 0 : 1 + rnd(r, 3000)) : bl;
    if (rnd(r, 30) == 0) rq->content_length = ISO_API_BODY_MAX + 1 + rnd(r, 100);
    if (rq->method == ISO_API_M_PUT) {
        if (rnd(r, 12) == 0) {
            rq->upload_offset = NULL;
        } else if (rnd(r, 12) == 0) {
            rq->upload_offset = "abc";
        } else {
            uint64_t known = 0;
            const iso_catalog_entry_t *e = api_find(IDS[rnd(r, N_IDS)]);
            if (e && rnd(r, 3) != 0) known = e->received;
            else known = rnd(r, 40000);
            snprintf(offbuf, 24, "%llu", (unsigned long long)known);
            rq->upload_offset = offbuf;
        }
    }
}

static void do_media_event(rng_t *r) {
    uint32_t ids[] = {0xaaaaaaa1u, 0xbbbbbbb2u, 0xeeeeeee5u};
    switch (rnd(r, 7)) {
    case 0: (void)iso_media_target_eject(&MEDIA); break;
    case 1: (void)iso_media_set_prevent(&MEDIA, rnd(r, 2) == 0); break;
    case 2: iso_media_bus_reset(&MEDIA); break;
    case 3: (void)iso_media_mount(&MEDIA, ids[rnd(r, 3)], true); break;
    case 4: (void)iso_media_eject(&MEDIA, true); break;
    case 5: iso_media_force_eject(&MEDIA); break;
    default: break;
    }
}

static void do_worker(rng_t *r) {
    unsigned steps = 1 + rnd(r, 6);
    for (unsigned i = 0; i < steps; i++) {
        iso_job_t *j = iso_core_pick(&CORE);
        if (!j) break;
        iso_step_result_t s = iso_job_step(&CORE, j, 4096);
        if (s.kind == ISO_STEP_CONTINUE && iso_job_waiting(j)) NOW_MS += 31000;
    }
}

static void run_seed(uint64_t seed, int ops) {
    rng_t r = {seed * 0x9E3779B97F4A7C15ull + 1};
    fx_api_setup();
    api_add_ready("aaaaaaa1", "alpha", SZ2);
    api_add_ready("bbbbbbb2", "beta", SZ2);
    api_add_incomplete("ccccccc3", "gamma", SZ2, PART_SMALL);
    api_add_invalid("ddddddd4", "delta", SZ2, ISO_INVALID_NOT_ISO);
    api_add_ready("eeeeeee5", "eps", SZ2);
    api_boot();
    static char rbody[ISO_API_BODY_MAX + 16];
    for (int op = 0; op < ops; op++) {
        uint32_t what = rnd(&r, 24);
        fp_t before;
        fingerprint(&before);
        if (what < 14) {
            iso_api_request_t rq;
            char path[64], off[24];
            random_request(&r, &rq, rbody, path, off);
            if (rq.method == ISO_API_M_PUT && rnd(&r, 2) == 0) {
                iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
                bool ok = iso_api_put_precheck(&ENV, &rq, &RSP);
                if (ok) {
                    TEST_ASSERT_EQUAL_INT(0, RSP.status);
                    TEST_ASSERT_TRUE(RSP.put.accepted);
                    TEST_ASSERT_FALSE(RSP.close_connection);
                    fp_t after;
                    fingerprint(&after);
                    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(after)); /* precheck は何も変えない */
                    static uint8_t data[4096];
                    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)rnd(&r, 256);
                    iso_api_put_t put = RSP.put;
                    if (rnd(&r, 4) == 0) { /* precheck と commit の間に、状態が変わる */
                        do_worker(&r);
                        do_media_event(&r);
                    }
                    iso_api_response_init(&RSP, RBUF, sizeof(RBUF));
                    iso_api_put_commit(&ENV, &put, data, put.len, &RSP);
                    check_response(&before, 2);
                    if (RSP.status == 200) TEST_ASSERT_EQUAL_INT(0, strncmp(RSP.body, "{\"received\":", 12));
                } else {
                    check_response(&before, 1); /* 拒否は何も変えない */
                    TEST_ASSERT_TRUE(RSP.close_connection);
                }
            } else {
                api_call_rq(&ENV, &rq);
                if (rq.method == ISO_API_M_PUT && RSP.put.accepted) { /* handle の PUT は、precheck まで。受理なら status = 0 */
                    TEST_ASSERT_EQUAL_INT(0, RSP.status);
                    fp_t after;
                    fingerprint(&after);
                    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(after));
                } else {
                    check_response(&before, rq.method == ISO_API_M_PUT ? 1 : 0);
                    if (rq.method == ISO_API_M_PUT) TEST_ASSERT_TRUE(RSP.close_connection);
                    if (RSP.status == 201) {
                        TEST_ASSERT_TRUE(strlen(RSP.log_id) == ISO_ID_LEN);
                        const iso_catalog_entry_t *e = api_find(RSP.log_id);
                        TEST_ASSERT_NOT_NULL(e);
                        TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, e->state);
                    }
                }
            }
            g_calls++;
        } else if (what < 17) {
            do_worker(&r);
        } else if (what < 19) {
            do_media_event(&r);
        } else if (what == 19) {
            NOW_MS += rnd(&r, 2) ? 6000 : 400000;
            iso_upload_tick(&CORE, NOW_MS);
        } else if (what == 20) {
            api_set_sd(rnd(&r, 3) == 0 ? ISO_SD_ERROR : ISO_SD_READY);
        } else if (what == 21) {
            OL_FAIL = (rnd(&r, 3) == 0);
        } else if (what == 22) {
            mock_fs_fail_write(&FS, rnd(&r, 2) ? 1 + (int)rnd(&r, 5) : 0, 0, rnd(&r, 2) ? ISO_ERR_IO : ISO_ERR_NO_SPACE);
        } else {
            check_list_matches_catalog();
        }
    }
    /* 後始末: 注入を止め、ワーカーを流し切る */
    OL_FAIL = false;
    mock_fs_fail_write(&FS, 0, 0, 0);
    SD_STATE = ISO_SD_READY;
    iso_core_publish(&CORE);
    check_list_matches_catalog();
    for (iso_job_t *j; (j = iso_core_pick(&CORE)) != NULL;) {
        iso_step_result_t s;
        int guard = 0;
        do {
            s = iso_job_step(&CORE, j, 4096);
            if (s.kind == ISO_STEP_CONTINUE && iso_job_waiting(j)) NOW_MS += 31000;
        } while (s.kind == ISO_STEP_CONTINUE && ++guard < 100000);
    }
    fx_api_teardown();
}

void test_fuzz_request_sequences(void) {
    for (uint64_t seed = 1; seed <= 40; seed++) run_seed(seed, 160);
    /* 十分に多様な結果が出ている */
    TEST_ASSERT_TRUE(g_calls > 3000);
    TEST_ASSERT_TRUE(g_status_seen[200] > 100);
    TEST_ASSERT_TRUE(g_status_seen[400] > 50);
    TEST_ASSERT_TRUE(g_status_seen[404] > 50);
    TEST_ASSERT_TRUE(g_status_seen[409] > 50);
    TEST_ASSERT_TRUE(g_status_seen[415] > 20);
    TEST_ASSERT_TRUE(g_status_seen[503] > 10);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_mutated_bodies);
    RUN_TEST(test_fuzz_random_names_round_trip_through_any_escaping);
    RUN_TEST(test_fuzz_request_sequences);
    return UNITY_END();
}

/*
 * iso_catalog: 固定シードのファズ(ASan / UBSan)。
 *   1. meta の復号に、任意のバイト列・任意の長さ(ちょうどの大きさのヒープ領域)・壊した有効な meta を与える。クラッシュ、範囲外の読み出しがない。
 *      OK を返したものは、検査を通り、符号化し直せて、復号し直すと同じになる。
 *   2. 名前の規則を、実装とは独立な基準(コードポイントを計算して判定)と、短い全バイト列で総当たりで比べる。
 *   3. begin / apply / 削除 / rescan / スキャン(再起動)/ 電源断 を、ランダムな順序で行い、不変条件を毎回確かめる。
 *      不変条件の基準は、ディスク(モック)上のスロットのバイト列を、実装を使わずに読んで求める。
 */
#include "ic_common.h"

void setUp(void) {}
void tearDown(void) {}

static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }
/* 64 ビットの乱数。上位、下位の順に別々の文で取る(| の左右の評価順は決まっていない) */
static uint64_t rnd64(void) {
    uint64_t hi = rnd();
    uint64_t lo = rnd();
    return (hi << 32) | lo;
}

static void set_crc(uint8_t *b) {
    uint32_t c = iso_crc32(b, 252);
    b[252] = (uint8_t)c;
    b[253] = (uint8_t)(c >> 8);
    b[254] = (uint8_t)(c >> 16);
    b[255] = (uint8_t)(c >> 24);
}

static bool same_meta(const iso_meta_t *a, const iso_meta_t *b) {
    return a->state == b->state && a->invalid_reason == b->invalid_reason && a->size_bytes == b->size_bytes &&
           a->part_count == b->part_count && a->part_blocks == b->part_blocks && memcmp(a->sha256, b->sha256, 32) == 0 &&
           a->sha_flag == b->sha_flag && a->name_len == b->name_len && memcmp(a->name, b->name, a->name_len) == 0 &&
           a->generation == b->generation;
}

static void check_decoded(const iso_meta_t *m) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_validate(m));
    TEST_ASSERT_EQUAL_UINT(m->name_len, strlen(m->name));
    TEST_ASSERT_TRUE(m->name_len >= 1 && m->name_len <= 63);
    uint8_t re[256];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(m, re));
    iso_meta_t again;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_decode(re, 256, &again));
    TEST_ASSERT_TRUE(same_meta(m, &again));
}

/* ---- 1. meta の復号 ---- */

void test_fuzz_decode_random_bytes_and_lengths(void) {
    unsigned accepted = 0;
    for (int i = 0; i < 100000; i++) {
        size_t len = (below(4) == 0) ? below(300) : 256;
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1); /* ちょうどの大きさ: 範囲外の読み出しを ASan が見つける */
        for (size_t k = 0; k < len; k++) buf[k] = (uint8_t)rnd();
        if (len == 256 && below(2)) {
            /* 先頭・版・各フィールドをほぼ正しくして、CRC も合わせる(フィールドの検査まで届かせる) */
            memcpy(buf, "ISO1", 4);
            buf[4] = 1;
            buf[5] = 0;
            buf[6] = (uint8_t)below(6);
            buf[7] = (uint8_t)below(7);
            memset(buf + 8, 0, 8);
            buf[8] = (uint8_t)(rnd() | 1u);
            buf[9] = (uint8_t)rnd();
            buf[10] = (uint8_t)rnd();
            buf[11] = (uint8_t)below(4);
            buf[24] = (uint8_t)below(18);
            buf[25] = 0;
            memset(buf + 28, 0, 4);
            buf[28] = (uint8_t)rnd();
            buf[29] = (uint8_t)rnd();
            buf[30] = (uint8_t)below(0x22);
            buf[64] = (uint8_t)below(4);
            buf[65] = (uint8_t)(1 + below(66));
            for (int k = 66; k < 130; k++) buf[k] = (uint8_t)(below(20) == 0 ? rnd() : 'a' + below(26));
            set_crc(buf);
        }
        iso_meta_t m;
        int rc = iso_meta_decode(buf, len, &m);
        if (rc == ISO_OK) {
            accepted++;
            check_decoded(&m);
        } else {
            TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, rc);
        }
        free(buf);
    }
    printf("fuzz(meta decode random): accepted=%u\n", accepted);
    TEST_ASSERT_TRUE_MESSAGE(accepted > 300, "fuzz reached the field checks too rarely");
}

void test_fuzz_decode_mutated_valid_metas(void) {
    unsigned accepted = 0, rejected = 0;
    for (int i = 0; i < 100000; i++) {
        iso_meta_t m;
        memset(&m, 0, sizeof(m));
        m.state = (uint8_t)below(5);
        m.invalid_reason = (uint8_t)below(5);
        m.size_bytes = 1 + ((uint64_t)rnd() << 3) % ISO_IMAGE_BYTES_MAX;
        m.part_count = (uint16_t)(1 + below(16));
        m.part_blocks = 1 + below(2097152);
        for (int k = 0; k < 32; k++) m.sha256[k] = (uint8_t)rnd();
        m.sha_flag = (uint8_t)below(3);
        m.name_len = (uint8_t)(1 + below(63));
        for (int k = 0; k < m.name_len; k++) m.name[k] = (char)('a' + below(26));
        m.generation = rnd();
        uint8_t b[256];
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, b));
        iso_meta_t d;
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_decode(b, 256, &d)); /* 正しいものは通る */
        TEST_ASSERT_TRUE(same_meta(&m, &d));
        int muts = 1 + (int)below(4);
        for (int k = 0; k < muts; k++) b[below(256)] = (uint8_t)rnd();
        bool fixed = below(2);
        if (fixed) set_crc(b);
        int rc = iso_meta_decode(b, 256, &d);
        if (rc == ISO_OK) {
            accepted++;
            check_decoded(&d);
        } else {
            rejected++;
        }
    }
    printf("fuzz(meta decode mutated): accepted=%u rejected=%u\n", accepted, rejected);
    TEST_ASSERT_TRUE(accepted > 5000);
    TEST_ASSERT_TRUE(rejected > 20000);
}

void test_fuzz_encode_matches_validate(void) {
    unsigned ok = 0, bad = 0;
    for (int i = 0; i < 200000; i++) {
        iso_meta_t m;
        memset(&m, 0, sizeof(m));
        m.state = (uint8_t)below(7);
        m.invalid_reason = (uint8_t)below(7);
        m.size_bytes = below(3) == 0 ? rnd64() : (uint64_t)below(20) * 1073741824ull + below(3);
        m.part_count = (uint16_t)below(20);
        m.part_blocks = below(3) ? below(2097160) : 0;
        m.sha_flag = (uint8_t)below(5);
        m.name_len = (uint8_t)below(70);
        for (int k = 0; k < 64; k++) m.name[k] = (char)('a' + below(26));
        if (below(8) == 0) m.name[below(64)] = (char)below(256);
        m.generation = rnd();
        uint8_t out[256];
        int rv = iso_meta_validate(&m);
        int re = iso_meta_encode(&m, out);
        TEST_ASSERT_EQUAL_INT(rv, re);
        if (re == ISO_OK) {
            ok++;
            iso_meta_t d;
            TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_decode(out, 256, &d));
            TEST_ASSERT_TRUE(same_meta(&m, &d));
        } else {
            bad++;
        }
    }
    printf("fuzz(meta encode): ok=%u bad=%u\n", ok, bad);
    TEST_ASSERT_TRUE(ok > 2000);
    TEST_ASSERT_TRUE(bad > 100000);
}

/* ---- 2. 名前 ---- */

/* 独立な基準: コードポイントを計算して、規則を当てはめる */
static bool name_oracle(const uint8_t *s, size_t n) {
    if (n < 1 || n > 63) return false;
    if (s[0] == 0x20 || s[n - 1] == 0x20) return false;
    static const uint32_t minv[5] = {0, 0, 0x80, 0x800, 0x10000};
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        size_t len;
        uint8_t b = s[i];
        if (b < 0x80) {
            cp = b;
            len = 1;
        } else if ((b >> 5) == 0x6) {
            len = 2;
            cp = b & 0x1Fu;
        } else if ((b >> 4) == 0xE) {
            len = 3;
            cp = b & 0x0Fu;
        } else if ((b >> 3) == 0x1E) {
            len = 4;
            cp = b & 0x07u;
        } else {
            return false;
        }
        if (i + len > n) return false;
        for (size_t k = 1; k < len; k++) {
            if ((s[i + k] >> 6) != 2) return false;
            cp = (cp << 6) | (s[i + k] & 0x3Fu);
        }
        if (cp < minv[len]) return false;
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || cp == '/' || cp == '\\') return false;
        i += len;
    }
    return true;
}

void test_fuzz_name_exhaustive_short_strings(void) {
    unsigned valid = 0;
    uint8_t s[3];
    for (uint32_t a = 0; a < 256; a++) {
        s[0] = (uint8_t)a;
        bool o = name_oracle(s, 1);
        TEST_ASSERT_EQUAL_INT(o ? 1 : 0, iso_name_valid((const char *)s, 1) ? 1 : 0);
        valid += o;
        for (uint32_t b = 0; b < 256; b++) {
            s[1] = (uint8_t)b;
            o = name_oracle(s, 2);
            TEST_ASSERT_EQUAL_INT_MESSAGE(o ? 1 : 0, iso_name_valid((const char *)s, 2) ? 1 : 0, "2-byte string");
            valid += o;
            for (uint32_t c = 0; c < 256; c++) {
                s[2] = (uint8_t)c;
                o = name_oracle(s, 3);
                if (o != iso_name_valid((const char *)s, 3)) {
                    char msg[60];
                    snprintf(msg, sizeof(msg), "3-byte string %02X %02X %02X", a, b, c);
                    TEST_FAIL_MESSAGE(msg);
                }
                valid += o;
            }
        }
    }
    printf("fuzz(name exhaustive): valid strings = %u\n", valid);
    TEST_ASSERT_TRUE(valid > 500000);
}

void test_fuzz_name_random_long_strings(void) {
    static const uint8_t interesting[] = {0x00, 0x1F, 0x20, 0x2F, 0x41, 0x5C, 0x7E, 0x7F, 0x80, 0x85, 0x8F, 0x90, 0x9F, 0xA0,
                                          0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xDF, 0xE0, 0xE1, 0xE3, 0xED, 0xEE, 0xEF, 0xF0, 0xF1,
                                          0xF4, 0xF5, 0xF8, 0xFF, 0x82, 0xA4, 0xBF, 0x81};
    unsigned valid = 0, invalid = 0;
    for (int i = 0; i < 1000000; i++) {
        size_t n = below(5) == 0 ? (size_t)(60 + below(8)) : (size_t)(1 + below(8));
        uint8_t *s = (uint8_t *)malloc(n);
        for (size_t k = 0; k < n; k++) s[k] = below(3) ? interesting[below(sizeof(interesting))] : (uint8_t)(0x61 + below(26));
        /* 妥当な UTF-8 の並びを混ぜる */
        if (below(3) == 0 && n >= 3) {
            s[0] = 0xE3;
            s[1] = (uint8_t)(0x80 + below(64));
            s[2] = (uint8_t)(0x80 + below(64));
        }
        bool o = name_oracle(s, n);
        TEST_ASSERT_EQUAL_INT(o ? 1 : 0, iso_name_valid((const char *)s, n) ? 1 : 0);
        if (o) valid++; else invalid++;
        free(s);
    }
    printf("fuzz(name random): valid=%u invalid=%u\n", valid, invalid);
    TEST_ASSERT_TRUE(valid > 50000);
    TEST_ASSERT_TRUE(invalid > 200000);
}

/* ---- 3. ランダムな操作列 + 電源断 ---- */

/* ディスク上のスロットを、実装を使わずに読む基準: 有効 = 長さ 256 + iso_meta_decode が OK(復号そのものは meta のテストが固定) */
typedef struct {
    bool valid[2];
    uint32_t gen[2];
    uint8_t state[2];
} oracle_slots_t;

static oracle_slots_t oracle_read(const char *id) {
    oracle_slots_t o;
    memset(&o, 0, sizeof(o));
    for (int s = 0; s < 2; s++) {
        char path[ISO_PATH_MAX];
        slot_path(path, sizeof(path), id, s);
        size_t len = 0;
        const uint8_t *d = mock_fs_data(&FS, path, &len);
        if (!d || mock_fs_size(&FS, path) != 256 || len != 256) continue;
        iso_meta_t m;
        if (iso_meta_decode(d, 256, &m) == ISO_OK) {
            o.valid[s] = true;
            o.gen[s] = m.generation;
            o.state[s] = m.state;
        }
    }
    return o;
}

/* カタログが、ディスク上の内容と矛盾しないこと */
static void check_catalog_against_disk(const iso_catalog_t *c) {
    char names[ISO_MAX_IMAGES + 8][ISO_NAME_MAX + 1];
    int n = mock_fs_children(&FS, "/iso", names, ISO_MAX_IMAGES + 8);
    unsigned id_dirs = 0;
    for (int i = 0; i < n; i++) {
        char path[ISO_PATH_MAX];
        snprintf(path, sizeof(path), "/iso/%s", names[i]);
        if (iso_id_valid(names[i]) && mock_fs_is_dir(&FS, path)) id_dirs++;
    }
    TEST_ASSERT_EQUAL_UINT(id_dirs, c->count + c->hidden);
    for (size_t i = 0; i < c->count; i++) {
        const iso_catalog_entry_t *e = &c->images[i];
        TEST_ASSERT_TRUE(iso_id_valid(e->id));
        for (size_t j = i + 1; j < c->count; j++) TEST_ASSERT_TRUE(strcmp(e->id, c->images[j].id) != 0);
        oracle_slots_t o = oracle_read(e->id);
        int adopted;
        if (!o.valid[0] && !o.valid[1]) adopted = -1;
        else if (o.valid[0] && !o.valid[1]) adopted = 0;
        else if (!o.valid[0] && o.valid[1]) adopted = 1;
        else adopted = ((int32_t)(o.gen[1] - o.gen[0]) > 0) ? 1 : 0;
        TEST_ASSERT_EQUAL_INT8(adopted, e->adopted);
        TEST_ASSERT_EQUAL_INT(adopted >= 0 ? 1 : 0, e->meta_valid ? 1 : 0);
        if (adopted >= 0) {
            TEST_ASSERT_EQUAL_UINT32(o.gen[adopted], e->meta.generation);
            TEST_ASSERT_EQUAL_UINT8(o.state[adopted], e->meta.state);
            /* 実行時の状態(スキャン後): 保存された uploading は incomplete、それ以外はそのまま(仕様の対応を、ここに手書きする) */
            static const iso_state_t runtime_of_meta[5] = {ISO_STATE_INCOMPLETE, ISO_STATE_READY, ISO_STATE_INCOMPLETE, ISO_STATE_INVALID,
                                                           ISO_STATE_DELETING};
            TEST_ASSERT_EQUAL_INT(runtime_of_meta[o.state[adopted]], e->state);
            TEST_ASSERT_EQUAL_INT(o.state[adopted] == ISO_META_DELETING ? 1 : 0, e->resume_delete ? 1 : 0);
            TEST_ASSERT_TRUE(iso_name_valid(e->meta.name, e->meta.name_len));
        } else {
            TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, e->state);
            TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_META_CORRUPT, e->invalid_reason);
        }
    }
}

static void fuzz_one_seed(int seed, unsigned *st_ops, unsigned *st_cuts, unsigned *st_imports, unsigned *st_deletes) {
    rs = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xD1B54A32D192ED03ull);
    mock_fs_free(&FS);
    fx_setup();
    uint32_t seq[64];
    for (int i = 0; i < 64; i++) seq[i] = rnd() % 40; /* 衝突が起きやすい小さな範囲 */
    mock_rnd_set(&RND, seq, 64);
    fx_settle();

    iso_delete_job_t job;
    bool have_job = false;
    int imp = 0;
    for (int op = 0; op < 100; op++) {
        (*st_ops)++;
        int rc = 0;
        switch (below(11)) {
        case 0: { /* begin */
            char id[9];
            char name[16];
            snprintf(name, sizeof(name), "img%u", below(6));
            uint64_t size = below(8) == 0 ? below(40000) : 34816 + below(3) * GIB + below(100000);
            rc = iso_catalog_begin(&CAT, name, strlen(name), size, below(2) ? (const uint8_t *)"01234567890123456789012345678901" : NULL, id);
            break;
        }
        case 1:
        case 2:
        case 3: { /* 任意のイベント */
            if (CAT.count == 0) break;
            iso_catalog_entry_t *e = &CAT.images[below((uint32_t)CAT.count)];
            iso_event_t ev = (iso_event_t)below(ISO_EV_COUNT);
            iso_trans_ctx_t ctx = {below(2), below(2), below(8) == 0};
            iso_apply_arg_t arg;
            memset(&arg, 0, sizeof(arg));
            arg.invalid_reason = (uint8_t)below(7);
            arg.set_sha = below(2);
            arg.sha_flag = (uint8_t)below(4);
            arg.sha256 = below(4) ? (const uint8_t *)"01234567890123456789012345678901" : NULL;
            if (below(3) == 0) e->received = e->meta_valid ? e->meta.size_bytes : 0;
            if (ev == ISO_EV_DELETE_DONE || ev == ISO_EV_BEGIN) break; /* エントリを消す / 作るイベントは別の経路 */
            rc = iso_catalog_apply(&CAT, e, ev, &ctx, below(2) ? &arg : NULL);
            if (rc > 0) rc = 0; /* 拒否は I/O のエラーではない */
            if (rc == ISO_ERR_INVALID) rc = 0; /* 引数の検査 */
            break;
        }
        case 4:
        case 5: { /* 削除のステップ */
            if (!have_job && CAT.count > 0) {
                iso_catalog_entry_t *e = &CAT.images[below((uint32_t)CAT.count)];
                iso_trans_ctx_t ctx = {false, true, false};
                int ar = iso_catalog_apply(&CAT, e, ISO_EV_DELETE, &ctx, NULL);
                if (ar == 0 || e->state == ISO_STATE_DELETING) {
                    if (iso_delete_job_init(&job, e->id) == ISO_OK) have_job = true;
                }
                if (ar < 0) rc = ar;
            }
            if (have_job && rc == 0) {
                iso_step_result_t r = iso_catalog_delete_step(&CAT, &job);
                if (r.kind == ISO_STEP_DONE) {
                    have_job = false;
                    (*st_deletes)++;
                } else if (r.kind == ISO_STEP_ERROR) {
                    rc = r.err;
                }
            }
            break;
        }
        case 6: { /* import するファイルを置く */
            char fn[24];
            snprintf(fn, sizeof(fn), "f%d.iso", imp++);
            put_import(fn, below(5) == 0 ? 100 : 40000, below(6) == 0 ? NULL : "CD001", 0);
            break;
        }
        case 7: { /* rescan */
            iso_rescan_result_t r;
            rc = iso_catalog_rescan(&CAT, below(3) == 0 ? below(4) : 0, &r);
            if (rc == 0) *st_imports += r.imported;
            break;
        }
        case 8: { /* 再起動(スキャン): カタログを作り直す */
            have_job = false;
            iso_catalog_init(&CAT, &IFS, &IRND);
            rc = iso_catalog_scan(&CAT);
            break;
        }
        default: { /* 電源断の予約: 今から k 回目の書き込みで落ちる(頻度は抑える) */
            if (below(3) != 0) break;
            mock_fs_cut_at(&FS, 1 + (int)below(3), (mock_tear_t)below(3));
            break;
        }
        }
        if (rc == ISO_ERR_IO || FS.dead) {
            /* 電源が落ちた: 再起動(電源断を数えるのは、実際に落ちたとき。予約の上書きや、起きなかった予約は数えない) */
            if (FS.dead) (*st_cuts)++;
            have_job = false;
            mock_fs_power_on(&FS);
            iso_catalog_init(&CAT, &IFS, &IRND);
            TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
        }
        if (!FS.dead && FS.cut_at == 0) {
            /* 電源が入っている間は、カタログがディスクと矛盾しないこと(メモリ上の状態遷移の途中は除く: 再スキャンして確かめる) */
            iso_catalog_t c2;
            iso_catalog_init(&c2, &IFS, &IRND);
            TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&c2));
            check_catalog_against_disk(&c2);
        }
    }
    /* 最後に: 電源を戻して、すべて消せる */
    mock_fs_power_on(&FS);
    iso_catalog_init(&CAT, &IFS, &IRND);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    check_catalog_against_disk(&CAT);
    for (int guard = 0; CAT.count > 0 && guard < 40; guard++) {
        iso_delete_job_t j;
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&j, CAT.images[0].id));
        iso_step_result_t r;
        int g2 = 0;
        do {
            r = iso_catalog_delete_step(&CAT, &j);
        } while (r.kind == ISO_STEP_CONTINUE && ++g2 < 40);
        TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
        (*st_deletes)++;
    }
    /* hidden が残っていれば、もう一度スキャンして続ける */
    for (int guard = 0; guard < 4; guard++) {
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
        while (CAT.count > 0) {
            iso_delete_job_t j;
            TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&j, CAT.images[0].id));
            iso_step_result_t r;
            int g2 = 0;
            do {
                r = iso_catalog_delete_step(&CAT, &j);
            } while (r.kind == ISO_STEP_CONTINUE && ++g2 < 40);
            TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
        }
    }
    char names[8][ISO_NAME_MAX + 1];
    int n = mock_fs_children(&FS, "/iso", names, 8);
    for (int i = 0; i < n; i++) TEST_ASSERT_EQUAL_STRING("import", names[i]);
}

void test_fuzz_random_operations_with_power_cuts(void) {
    unsigned ops = 0, cuts = 0, imports = 0, deletes = 0;
    for (int seed = 1; seed <= 150; seed++) fuzz_one_seed(seed, &ops, &cuts, &imports, &deletes);
    printf("fuzz(operations): ops=%u cuts=%u imports=%u deletes=%u\n", ops, cuts, imports, deletes);
    TEST_ASSERT_TRUE(cuts > 300);
    TEST_ASSERT_TRUE(imports > 100);
    TEST_ASSERT_TRUE(deletes > 100);
    mock_fs_free(&FS);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_decode_random_bytes_and_lengths);
    RUN_TEST(test_fuzz_decode_mutated_valid_metas);
    RUN_TEST(test_fuzz_encode_matches_validate);
    RUN_TEST(test_fuzz_name_exhaustive_short_strings);
    RUN_TEST(test_fuzz_name_random_long_strings);
    RUN_TEST(test_fuzz_random_operations_with_power_cuts);
    return UNITY_END();
}

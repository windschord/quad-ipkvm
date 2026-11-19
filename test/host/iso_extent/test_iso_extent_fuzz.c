/*
 * iso_extent: 固定シードのファズ。ランダムな表(パーツ、エクステントの切り方、隣接・逆向き・長さ 0・余分な末尾を含む)と
 * ランダムな範囲を作り、実装とは独立な「1 セクタずつ展開する」基準(オラクル)と、すべての結果を比べる。
 * ASan / UBSan で実行する。
 */
#include "ie_common.h"

void setUp(void) { memset(&H, 0, sizeof(H)); }
void tearDown(void) { TEST_ASSERT_EQUAL_INT(H.allocs, H.frees); }

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}
static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0; }

#define MAXP 4
#define MAXE 64

typedef struct {
    iso_extent_t ext[MAXP][MAXE];
    size_t n[MAXP];
    uint64_t file_bytes[MAXP];
} gen_t;

static void gen_extents(uint64_t need_sectors, iso_extent_t *out, size_t *n_out) {
    size_t n = 0;
    uint64_t left = need_sectors;
    uint64_t cursor = (uint64_t)rnd_below(1000) * 16;
    uint64_t prev_start = cursor;
    while (left > 0 && n < MAXE - 6) {
        /* 長さ 0 のエクステントを混ぜる */
        if (rnd_below(10) == 0) {
            out[n].start_sector = rnd();
            out[n].sector_count = 0;
            n++;
        }
        uint64_t len = 1 + rnd_below(9);
        if (len > left || rnd_below(8) == 0) len = left; /* 最後まで 1 つで */
        uint32_t k = rnd_below(10);
        uint64_t start;
        if (k < 4) {
            start = cursor; /* 直前の終わりに隣接 */
        } else if (k < 5 && prev_start >= len) {
            start = prev_start - len; /* 直前の開始の手前に、逆向きに接する */
        } else if (k < 6) {
            start = 4294967296ull + rnd_below(100000); /* 32 ビットを超える */
        } else {
            start = cursor + 1 + rnd_below(50); /* 離れる */
        }
        out[n].start_sector = start;
        out[n].sector_count = len;
        n++;
        cursor = start + len;
        prev_start = start;
        left -= len;
    }
    /* 余分な末尾(孤児のクラスタ) */
    if (rnd_below(3) == 0) {
        out[n].start_sector = 7000000 + rnd_below(1000);
        out[n].sector_count = 1 + rnd_below(8);
        n++;
    }
    *n_out = n;
}

typedef struct {
    uint64_t phys[MAXP * 6 * 4 * 16]; /* 論理セクタ -> SD セクタ */
    uint64_t file_sectors;
} oracle_t;

void test_fuzz_map_matches_sector_by_sector_oracle(void) {
    unsigned queries = 0, ok_queries = 0, merged_queries = 0, split_queries = 0, full_checks = 0;
    for (int seed = 0; seed < 300; seed++) {
        uint32_t pb = 1 + rnd_below(6);
        uint32_t parts = 1 + rnd_below(MAXP);
        uint64_t part_bytes = (uint64_t)pb * 2048;
        uint64_t size = (uint64_t)(parts - 1) * part_bytes + 1 + rnd_below((uint32_t)part_bytes);
        TEST_ASSERT_EQUAL_UINT64(parts, iso_extent_part_count(size, pb));

        static gen_t g;
        static oracle_t o;
        memset(&g, 0, sizeof(g));
        iso_part_input_t in[MAXP];
        o.file_sectors = iso_extent_file_sectors(size);
        for (uint32_t p = 0; p < parts; p++) {
            uint64_t bytes = (p + 1 < parts) ? part_bytes : size - (uint64_t)(parts - 1) * part_bytes;
            uint64_t need = (bytes + 511) / 512; /* 独立な計算 */
            gen_extents(need, g.ext[p], &g.n[p]);
            g.file_bytes[p] = bytes;
            in[p].file_bytes = bytes;
            in[p].extents = g.ext[p];
            in[p].n_extents = g.n[p];
            /* オラクル: 1 セクタずつ展開 */
            uint64_t k = 0;
            for (size_t i = 0; i < g.n[p] && k < need; i++) {
                for (uint64_t s = 0; s < g.ext[p][i].sector_count && k < need; s++) {
                    o.phys[(uint64_t)p * pb * 4 + k] = g.ext[p][i].start_sector + s;
                    k++;
                }
            }
            TEST_ASSERT_EQUAL_UINT64(need, k);
        }
        iso_allocator_t a = ie_allocator();
        iso_extent_table_t *t = NULL;
        TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_table_build(&a, (uint32_t)seed, size, pb, in, parts, &t));

        uint64_t cap = (size + 2047) / 2048;
        TEST_ASSERT_EQUAL_UINT64(cap, iso_extent_table_capacity_blocks(t));
        for (int q = 0; q < 200; q++) {
            uint64_t lba = rnd_below((uint32_t)cap + 2);
            uint32_t blocks = rnd_below(((uint32_t)cap + 3) / (1 + rnd_below(4)) + 1);
            iso_extent_t sp[256];
            size_t n = 123;
            int rc = iso_extent_map(t, lba, blocks, sp, 256, &n);
            queries++;
            if (lba >= cap || blocks > cap - lba) {
                TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_RANGE, rc);
                TEST_ASSERT_EQUAL_UINT(0, n);
                if (lba >= cap) TEST_ASSERT_EQUAL_UINT64(0, iso_extent_valid_bytes(t, lba, blocks));
                continue;
            }
            TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, rc);
            ok_queries++;
            /* オラクルの区間 */
            iso_extent_t want[256];
            size_t nw = 0;
            uint64_t first = lba * 4, last = lba * 4 + (uint64_t)blocks * 4;
            if (last > o.file_sectors) last = o.file_sectors;
            for (uint64_t L = first; L < last; L++) {
                uint64_t ph = o.phys[L];
                if (nw > 0 && want[nw - 1].start_sector + want[nw - 1].sector_count == ph) {
                    want[nw - 1].sector_count++;
                } else {
                    TEST_ASSERT_TRUE(nw < 256);
                    want[nw].start_sector = ph;
                    want[nw].sector_count = 1;
                    nw++;
                }
            }
            TEST_ASSERT_EQUAL_UINT(nw, n);
            for (size_t i = 0; i < nw; i++) {
                TEST_ASSERT_EQUAL_UINT64(want[i].start_sector, sp[i].start_sector);
                TEST_ASSERT_EQUAL_UINT64(want[i].sector_count, sp[i].sector_count);
            }
            if (blocks > 0) {
                /* 有効バイト: 独立な式 */
                uint64_t start = lba * 2048, len = (uint64_t)blocks * 2048;
                uint64_t exp_valid = (size > start) ? ((size - start) < len ? (size - start) : len) : 0;
                TEST_ASSERT_EQUAL_UINT64(exp_valid, iso_extent_valid_bytes(t, lba, blocks));
                uint64_t total = 0;
                for (size_t i = 0; i < n; i++) total += sp[i].sector_count;
                TEST_ASSERT_EQUAL_UINT64((exp_valid + 511) / 512, total);
            }
            /* 出力の配列の大きさ: ちょうど n 個なら OK、n - 1 個なら SPANS_FULL */
            if (n >= 1 && full_checks < 100000) {
                size_t m = 0;
                TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(t, lba, blocks, sp, n, &m));
                TEST_ASSERT_EQUAL_UINT(n, m);
                TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_SPANS_FULL, iso_extent_map(t, lba, blocks, sp, n - 1, &m));
                TEST_ASSERT_EQUAL_UINT(0, m);
                full_checks++;
            }
            if (n >= 2) split_queries++;
            if (n >= 1 && (last - first) > 0 && n < (size_t)(last - first)) merged_queries++;
        }
        iso_extent_table_unref(t);
    }
    printf("fuzz(iso_extent): queries=%u ok=%u split=%u merged=%u full_checks=%u\n", queries, ok_queries, split_queries,
           merged_queries, full_checks);
    TEST_ASSERT_TRUE_MESSAGE(ok_queries > 20000, "fuzz explored too few valid queries");
    TEST_ASSERT_TRUE_MESSAGE(split_queries > 5000, "fuzz produced too few multi-span queries");
    TEST_ASSERT_TRUE_MESSAGE(merged_queries > 5000, "fuzz produced too few merges");
}

/* build が、ランダムな入力(壊れたものを含む)でクラッシュせず、エラーのとき何も確保しない */
void test_fuzz_build_with_garbage_inputs(void) {
    unsigned ok = 0, bad = 0;
    for (int i = 0; i < 20000; i++) {
        iso_extent_t e[MAXP + 1][8];
        iso_part_input_t in[MAXP + 1];
        uint32_t pb = rnd_below(8);
        uint64_t size = rnd_below(5) == 0 ? ((uint64_t)rnd() << 32 | rnd()) : rnd_below(70000);
        uint64_t expect = iso_extent_part_count(size, pb);
        uint32_t parts = (rnd_below(4) != 0 && expect <= MAXP) ? (uint32_t)expect : rnd_below(MAXP + 2);
        for (uint32_t p = 0; p < MAXP + 1; p++) {
            size_t ne = rnd_below(9);
            for (size_t k = 0; k < ne; k++) {
                e[p][k].start_sector = rnd_below(4) == 0 ? UINT64_MAX - rnd_below(4) : rnd_below(100000);
                e[p][k].sector_count = rnd_below(3) == 0 ? 0 : rnd_below(40);
            }
            in[p].extents = e[p];
            in[p].n_extents = ne;
            in[p].file_bytes = rnd_below(8) ? iso_extent_part_bytes(size, pb, p) : rnd_below(20000);
        }
        int before = H.allocs;
        iso_allocator_t a = ie_allocator();
        iso_extent_table_t *t = (iso_extent_table_t *)0x1;
        int rc = iso_extent_table_build(&a, 0, size, pb, in, parts, &t);
        if (rc == ISO_EXT_OK) {
            ok++;
            TEST_ASSERT_NOT_NULL(t);
            TEST_ASSERT_EQUAL_INT(before + 1, H.allocs);
            iso_extent_t sp[64];
            size_t n;
            (void)iso_extent_map(t, 0, (uint32_t)iso_extent_table_capacity_blocks(t), sp, 64, &n);
            iso_extent_table_unref(t);
        } else {
            bad++;
            TEST_ASSERT_NULL(t);
            TEST_ASSERT_EQUAL_INT(before, H.allocs);
        }
    }
    printf("fuzz(iso_extent build): ok=%u bad=%u\n", ok, bad);
    TEST_ASSERT_TRUE_MESSAGE(ok > 50, "fuzz built too few tables");
    TEST_ASSERT_TRUE_MESSAGE(bad > 1000, "fuzz rejected too few inputs");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_map_matches_sector_by_sector_oracle);
    RUN_TEST(test_fuzz_build_with_garbage_inputs);
    return UNITY_END();
}

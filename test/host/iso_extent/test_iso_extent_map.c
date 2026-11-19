/*
 * iso_extent: CD のブロックの範囲 -> SD のセクタの区間。期待値はすべて手書き(基準の表 A は ie_common.h)。
 */
#include "ie_common.h"

void setUp(void) { memset(&H, 0, sizeof(H)); }
void tearDown(void) { TEST_ASSERT_EQUAL_INT(H.allocs, H.frees); }

static int build(uint32_t epoch, uint64_t size, uint32_t pb, const iso_part_input_t *in, size_t n, iso_extent_table_t **out) {
    iso_allocator_t a = ie_allocator();
    return iso_extent_table_build(&a, epoch, size, pb, in, n, out);
}

typedef struct {
    uint64_t lba;
    uint32_t blocks;
    size_t n;
    iso_extent_t sp[4];
    uint64_t valid_bytes;
    const char *what;
} map_case_t;

static void check_cases(iso_extent_table_t *t, const map_case_t *cs, size_t ncases) {
    for (size_t i = 0; i < ncases; i++) {
        iso_extent_t out[8];
        memset(out, 0xAB, sizeof(out));
        size_t n = 77;
        int rc = iso_extent_map(t, cs[i].lba, cs[i].blocks, out, 8, &n);
        char msg[160];
        snprintf(msg, sizeof(msg), "case %zu: %s (lba=%llu blocks=%u)", i, cs[i].what, (unsigned long long)cs[i].lba, cs[i].blocks);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_EXT_OK, rc, msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(cs[i].n, n, msg);
        for (size_t k = 0; k < cs[i].n; k++) {
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(cs[i].sp[k].start_sector, out[k].start_sector, msg);
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(cs[i].sp[k].sector_count, out[k].sector_count, msg);
        }
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(cs[i].valid_bytes, iso_extent_valid_bytes(t, cs[i].lba, cs[i].blocks), msg);
    }
}

/*
 * 基準の表 A。論理セクタ s (= lba * 4 + 0..3) の SD 上の位置(手計算):
 *   パーツ 0: s 0..9 -> 1000..1009、s 10..15 -> 2000..2005
 *   パーツ 1: s 16..31 -> 2006..2021
 *   パーツ 2: s 32..39 -> 5000..5007、s 40..44 -> 6000..6004 (s 45 以降は存在しない。size_bytes = 22628)
 */
void test_map_table_a(void) {
    iso_extent_table_t *t = build_table_a();
    const map_case_t cs[] = {
        {0, 1, 1, {EXT(1000, 4)}, 2048, "first block"},
        {1, 1, 1, {EXT(1004, 4)}, 2048, "second block"},
        {2, 1, 2, {EXT(1008, 2), EXT(2000, 2)}, 2048, "one block split by an extent boundary (sectors 8..11 = 1008,1009,2000,2001)"},
        {2, 2, 2, {EXT(1008, 2), EXT(2000, 6)}, 4096, "crosses an extent boundary inside the part"},
        {3, 2, 1, {EXT(2002, 8)}, 4096, "crosses a part boundary; SD-contiguous, so merged"},
        {4, 1, 1, {EXT(2006, 4)}, 2048, "first block of part 1"},
        {0, 8, 2, {EXT(1000, 10), EXT(2000, 22)}, 16384, "parts 0 and 1 together; merged across the part boundary"},
        {7, 2, 2, {EXT(2018, 4), EXT(5000, 4)}, 4096, "crosses a part boundary that is not SD-contiguous"},
        {8, 1, 1, {EXT(5000, 4)}, 2048, "first block of part 2"},
        {9, 1, 1, {EXT(5004, 4)}, 2048, "second block of part 2 (still inside the first extent)"},
    };
    check_cases(t, cs, sizeof(cs) / sizeof(cs[0]));
    const map_case_t tail[] = {
        /* ブロック 10 = s 40..43 -> パーツ 2 の局所 8..11 = 2 つ目のエクステント [6000,8] の先頭 4 */
        {10, 1, 1, {EXT(6000, 4)}, 2048, "block 10"},
        /* ブロック 10 と 11 (s 40..47)。s 45 以降はファイルにない -> 5 セクタだけ。有効バイト = 22628 - 10 * 2048 = 2148 */
        {10, 2, 1, {EXT(6000, 5)}, 2148, "last two blocks; the tail beyond size_bytes has no span"},
        /* ブロック 11 (s 44..47)。s 44 だけが有効。有効バイト = 22628 - 11 * 2048 = 100 */
        {11, 1, 1, {EXT(6004, 1)}, 100, "last block holds 100 valid bytes -> 1 sector"},
        /* 全体 */
        {0, 12, 4, {EXT(1000, 10), EXT(2000, 22), EXT(5000, 8), EXT(6000, 5)}, 22628, "whole image"},
    };
    check_cases(t, tail, sizeof(tail) / sizeof(tail[0]));
    iso_extent_table_unref(t);
}

void test_map_range_errors(void) {
    iso_extent_table_t *t = build_table_a(); /* capacity_blocks = 12 */
    iso_extent_t sp[8];
    size_t n = 5;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_RANGE, iso_extent_map(t, 12, 1, sp, 8, &n)); /* LBA == capacity */
    TEST_ASSERT_EQUAL_UINT(0, n);
    n = 5;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_RANGE, iso_extent_map(t, 13, 1, sp, 8, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_RANGE, iso_extent_map(t, UINT64_MAX, 1, sp, 8, &n));
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_RANGE, iso_extent_map(t, 11, 2, sp, 8, &n)); /* 11 + 2 = 13 > 12 */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_RANGE, iso_extent_map(t, 0, 13, sp, 8, &n));
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_RANGE, iso_extent_map(t, 0, UINT32_MAX, sp, 8, &n));
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_RANGE, iso_extent_map(t, 12, 0, sp, 8, &n)); /* 0 ブロックでも LBA >= capacity は拒否 */
    /* 境界は通る */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(t, 11, 1, sp, 8, &n)); /* 最終ブロック */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(t, 0, 12, sp, 8, &n)); /* 全体 */
    iso_extent_table_unref(t);
}

void test_map_zero_blocks(void) {
    iso_extent_table_t *t = build_table_a();
    iso_extent_t sp[2];
    size_t n = 9;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(t, 11, 0, sp, 2, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    n = 9;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(t, 0, 0, NULL, 0, &n)); /* 区間の配列なしでも OK */
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_valid_bytes(t, 3, 0));
    iso_extent_table_unref(t);
}

void test_map_spans_full(void) {
    iso_extent_table_t *t = build_table_a();
    iso_extent_t sp[2];
    size_t n = 9;
    /* [1000,10] [2000,22] の 2 個。配列が 1 個では足りない */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_SPANS_FULL, iso_extent_map(t, 0, 8, sp, 1, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_SPANS_FULL, iso_extent_map(t, 0, 8, sp, 0, &n));
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(t, 0, 8, sp, 2, &n)); /* ちょうど 2 個なら OK */
    TEST_ASSERT_EQUAL_UINT(2, n);
    /* 1 個で済む範囲は、配列が 1 個でも OK */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(t, 3, 2, sp, 1, &n));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT64(2002, sp[0].start_sector);
    /* 4 個の全体 */
    iso_extent_t big[4];
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_SPANS_FULL, iso_extent_map(t, 0, 12, big, 3, &n));
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(t, 0, 12, big, 4, &n));
    iso_extent_table_unref(t);
}

void test_map_param_errors(void) {
    iso_extent_table_t *t = build_table_a();
    iso_extent_t sp[2];
    size_t n = 9;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, iso_extent_map(NULL, 0, 1, sp, 2, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, iso_extent_map(t, 0, 1, sp, 2, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, iso_extent_map(t, 0, 1, NULL, 2, &n));
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_valid_bytes(NULL, 0, 1));
    iso_extent_table_unref(t);
}

void test_valid_bytes(void) {
    iso_extent_table_t *t = build_table_a(); /* size_bytes = 22628、capacity = 12 */
    TEST_ASSERT_EQUAL_UINT64(2048, iso_extent_valid_bytes(t, 0, 1));
    TEST_ASSERT_EQUAL_UINT64(2048 * 11, iso_extent_valid_bytes(t, 0, 11));
    TEST_ASSERT_EQUAL_UINT64(22628, iso_extent_valid_bytes(t, 0, 12)); /* 11 * 2048 + 100 */
    TEST_ASSERT_EQUAL_UINT64(100, iso_extent_valid_bytes(t, 11, 1));
    TEST_ASSERT_EQUAL_UINT64(2148, iso_extent_valid_bytes(t, 10, 2));
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_valid_bytes(t, 12, 1)); /* 範囲外 */
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_valid_bytes(t, UINT64_MAX, 1));
    iso_extent_table_unref(t);
}

/* ---- 大きさが 2048 の倍数・512 の倍数でない / 倍数のとき ---- */

static iso_extent_table_t *single_part(uint64_t size, iso_extent_t *e, size_t ne) {
    iso_part_input_t in[1] = {{size, e, ne}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, size, ISO_PART_BLOCKS_DEFAULT, in, 1, &t));
    return t;
}

void test_map_size_2049(void) {
    /* 2049 B: capacity = 2 ブロック、ファイルは 5 セクタ(4.002 を切り上げ) */
    iso_extent_t e[1] = {EXT(100, 5)};
    iso_extent_table_t *t = single_part(2049, e, 1);
    TEST_ASSERT_EQUAL_UINT64(2, iso_extent_table_capacity_blocks(t));
    const map_case_t cs[] = {
        {0, 1, 1, {EXT(100, 4)}, 2048, "block 0"},
        {1, 1, 1, {EXT(104, 1)}, 1, "block 1 has 1 valid byte -> 1 sector"},
        {0, 2, 1, {EXT(100, 5)}, 2049, "both blocks"},
    };
    check_cases(t, cs, 3);
    iso_extent_table_unref(t);
}

void test_map_size_exact_multiple_of_2048(void) {
    /* 34816 B = 17 ブロック = 68 セクタ。最終ブロックは満杯 */
    iso_extent_t e[1] = {EXT(1000, 68)};
    iso_extent_table_t *t = single_part(34816, e, 1);
    TEST_ASSERT_EQUAL_UINT64(17, iso_extent_table_capacity_blocks(t));
    const map_case_t cs[] = {
        {16, 1, 1, {EXT(1064, 4)}, 2048, "sector 16 of the CD (signature) = SD sectors 1064..1067"},
        {0, 17, 1, {EXT(1000, 68)}, 34816, "whole image"},
    };
    check_cases(t, cs, 2);
    iso_extent_t sp[1];
    size_t n;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_RANGE, iso_extent_map(t, 17, 1, sp, 1, &n));
    iso_extent_table_unref(t);
}

void test_map_size_1_and_multiple_of_512_only(void) {
    iso_extent_t e1[1] = {EXT(9, 1)};
    iso_extent_table_t *t = single_part(1, e1, 1);
    const map_case_t c1[] = {{0, 1, 1, {EXT(9, 1)}, 1, "1 byte"}};
    check_cases(t, c1, 1);
    iso_extent_table_unref(t);
    /* 1536 B = 3 セクタ。capacity = 1 ブロック */
    iso_extent_t e3[1] = {EXT(50, 3)};
    t = single_part(1536, e3, 1);
    const map_case_t c3[] = {{0, 1, 1, {EXT(50, 3)}, 1536, "3 sectors"}};
    check_cases(t, c3, 1);
    iso_extent_table_unref(t);
}

/* ---- 64 ビットのセクタ番号 ---- */

void test_map_64bit_sector_numbers(void) {
    /* 2 パーツ(1 GiB + 4096 B)。パーツ 0 の先頭が 2^32 を超える */
    iso_extent_t e0[1] = {EXT(4294967306ull, 2097152)}; /* 2^32 + 10 */
    iso_extent_t e1[1] = {EXT(8000000, 8)};
    iso_part_input_t in[2] = {{1073741824ull, e0, 1}, {4096, e1, 1}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 1073741824ull + 4096, ISO_PART_BLOCKS_DEFAULT, in, 2, &t));
    TEST_ASSERT_EQUAL_UINT64(524290, iso_extent_table_capacity_blocks(t));
    const map_case_t cs[] = {
        {0, 1, 1, {EXT(4294967306ull, 4)}, 2048, "first block, start beyond 32 bits"},
        {524287, 3, 2, {EXT(4294967306ull + 2097148, 4), EXT(8000000, 8)}, 6144, "last block of part 0 and both blocks of part 1"},
        {524289, 1, 1, {EXT(8000004, 4)}, 2048, "last block of the image"},
        {524288, 1, 1, {EXT(8000000, 4)}, 2048, "first block of part 1"},
        {524286, 1, 1, {EXT(4294967306ull + 2097144, 4)}, 2048, "second to last block of part 0"},
    };
    check_cases(t, cs, 5);
    iso_extent_t sp[1];
    size_t n;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_RANGE, iso_extent_map(t, 524290, 1, sp, 1, &n));
    iso_extent_table_unref(t);
}

/* ---- 併合 ---- */

void test_map_merges_contiguous_extents_and_not_others(void) {
    /* part_blocks = 4 (16 セクタ)、1 パーツ。エクステント: [100,4] [104,4] (連続) [200,4] [199,4] (後ろ向き = 連続ではない) */
    iso_extent_t e[4] = {EXT(100, 4), EXT(104, 4), EXT(200, 4), EXT(199, 4)};
    iso_part_input_t in[1] = {{8192, e, 4}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 8192, 4, in, 1, &t));
    const map_case_t cs[] = {
        {0, 2, 1, {EXT(100, 8)}, 4096, "contiguous extents are merged"},
        {0, 4, 3, {EXT(100, 8), EXT(200, 4), EXT(199, 4)}, 8192, "a backwards neighbour is not merged"},
        {1, 2, 2, {EXT(104, 4), EXT(200, 4)}, 4096, "gap is not merged"},
    };
    check_cases(t, cs, 3);
    iso_extent_table_unref(t);
}

void test_map_merge_requires_exact_adjacency(void) {
    /* [100,4] の次が [105,4] (1 セクタ空く) -> 併合しない */
    iso_extent_t e[2] = {EXT(100, 4), EXT(105, 4)};
    iso_part_input_t in[1] = {{4096, e, 2}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 4096, 2, in, 1, &t));
    const map_case_t cs[] = {{0, 2, 2, {EXT(100, 4), EXT(105, 4)}, 4096, "off by one"}};
    check_cases(t, cs, 1);
    iso_extent_table_unref(t);
    /* [100,4] の次が [99,4] -> 後ろ向きに接していても併合しない(終端 = 次の開始 でも逆) */
    iso_extent_t e2[2] = {EXT(100, 4), EXT(96, 4)};
    iso_part_input_t in2[1] = {{4096, e2, 2}};
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 4096, 2, in2, 1, &t));
    const map_case_t cs2[] = {{0, 2, 2, {EXT(100, 4), EXT(96, 4)}, 4096, "backwards adjacent"}};
    check_cases(t, cs2, 1);
    iso_extent_table_unref(t);
}

void test_map_starts_exactly_at_an_extent_boundary(void) {
    /* part_blocks = 4。エクステント [100,8] [300,8]。ブロック 2 の先頭 (局所セクタ 8) が 2 つ目の先頭 */
    iso_extent_t e[2] = {EXT(100, 8), EXT(300, 8)};
    iso_part_input_t in[1] = {{4096 * 2, e, 2}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 8192, 4, in, 1, &t));
    const map_case_t cs[] = {
        {2, 1, 1, {EXT(300, 4)}, 2048, "starts at the second extent"},
        {1, 1, 1, {EXT(104, 4)}, 2048, "ends exactly at the end of the first extent"},
        {1, 2, 2, {EXT(104, 4), EXT(300, 4)}, 4096, "ends exactly at the end, then the next extent"},
        {3, 1, 1, {EXT(304, 4)}, 2048, "last block"},
    };
    check_cases(t, cs, 4);
    iso_extent_table_unref(t);
}

void test_map_ends_exactly_at_a_part_boundary(void) {
    /* 基準の表 A: ブロック 3 は s 12..15 でパーツ 0 の最後。ブロック 4 は s 16..19 でパーツ 1 の最初 */
    iso_extent_table_t *t = build_table_a();
    const map_case_t cs[] = {
        {3, 1, 1, {EXT(2002, 4)}, 2048, "last block of part 0"},
        {3, 2, 1, {EXT(2002, 8)}, 4096, "part 0 last + part 1 first (merged)"},
        {5, 2, 1, {EXT(2010, 8)}, 4096, "inside part 1"},
        {7, 1, 1, {EXT(2018, 4)}, 2048, "last block of part 1"},
    };
    check_cases(t, cs, 4);
    iso_extent_table_unref(t);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_map_table_a);
    RUN_TEST(test_map_range_errors);
    RUN_TEST(test_map_zero_blocks);
    RUN_TEST(test_map_spans_full);
    RUN_TEST(test_map_param_errors);
    RUN_TEST(test_valid_bytes);
    RUN_TEST(test_map_size_2049);
    RUN_TEST(test_map_size_exact_multiple_of_2048);
    RUN_TEST(test_map_size_1_and_multiple_of_512_only);
    RUN_TEST(test_map_64bit_sector_numbers);
    RUN_TEST(test_map_merges_contiguous_extents_and_not_others);
    RUN_TEST(test_map_merge_requires_exact_adjacency);
    RUN_TEST(test_map_starts_exactly_at_an_extent_boundary);
    RUN_TEST(test_map_ends_exactly_at_a_part_boundary);
    return UNITY_END();
}

#define _POSIX_C_SOURCE 200809L
/*
 * iso_extent: 純粋な計算、表の構築と検証、参照カウント。期待値はすべて手書き(実装から計算しない)。
 * 変換 (iso_extent_map) は test_iso_extent_map.c。
 */
#include <pthread.h>
#include "ie_common.h"

void setUp(void) { memset(&H, 0, sizeof(H)); }
void tearDown(void) {
    /* すべてのテストが、確保したものを解放して終わる */
    TEST_ASSERT_EQUAL_INT(H.allocs, H.frees);
}

#define GIB 1073741824ull

static int build(uint32_t epoch, uint64_t size, uint32_t pb, const iso_part_input_t *in, size_t n, iso_extent_table_t **out) {
    iso_allocator_t a = ie_allocator();
    return iso_extent_table_build(&a, epoch, size, pb, in, n, out);
}

/* ---- 純粋な計算 ---- */

void test_capacity_blocks_rounds_up(void) {
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_capacity_blocks(0));
    TEST_ASSERT_EQUAL_UINT64(1, iso_extent_capacity_blocks(1));
    TEST_ASSERT_EQUAL_UINT64(1, iso_extent_capacity_blocks(2047));
    TEST_ASSERT_EQUAL_UINT64(1, iso_extent_capacity_blocks(2048));
    TEST_ASSERT_EQUAL_UINT64(2, iso_extent_capacity_blocks(2049));
    TEST_ASSERT_EQUAL_UINT64(17, iso_extent_capacity_blocks(34816));
    TEST_ASSERT_EQUAL_UINT64(524288, iso_extent_capacity_blocks(GIB));
    TEST_ASSERT_EQUAL_UINT64(524289, iso_extent_capacity_blocks(GIB + 1));
    TEST_ASSERT_EQUAL_UINT64(8388608, iso_extent_capacity_blocks(16 * GIB));
    TEST_ASSERT_EQUAL_UINT64(9007199254740992ull, iso_extent_capacity_blocks(UINT64_MAX));
}

void test_file_sectors_rounds_up(void) {
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_file_sectors(0));
    TEST_ASSERT_EQUAL_UINT64(1, iso_extent_file_sectors(1));
    TEST_ASSERT_EQUAL_UINT64(1, iso_extent_file_sectors(511));
    TEST_ASSERT_EQUAL_UINT64(1, iso_extent_file_sectors(512));
    TEST_ASSERT_EQUAL_UINT64(2, iso_extent_file_sectors(513));
    TEST_ASSERT_EQUAL_UINT64(45, iso_extent_file_sectors(22628)); /* 44.19... */
    TEST_ASSERT_EQUAL_UINT64(2097152, iso_extent_file_sectors(GIB));
}

void test_part_count_and_bytes(void) {
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_part_count(0, 524288));
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_part_count(100, 0));
    TEST_ASSERT_EQUAL_UINT64(1, iso_extent_part_count(1, 524288));
    TEST_ASSERT_EQUAL_UINT64(1, iso_extent_part_count(GIB, 524288));
    TEST_ASSERT_EQUAL_UINT64(2, iso_extent_part_count(GIB + 1, 524288));
    TEST_ASSERT_EQUAL_UINT64(16, iso_extent_part_count(16 * GIB, 524288));
    TEST_ASSERT_EQUAL_UINT64(17, iso_extent_part_count(16 * GIB + 1, 524288));
    TEST_ASSERT_EQUAL_UINT64(3, iso_extent_part_count(22628, 4));
    /* 最後以外は part_blocks * 2048、最後は残り */
    TEST_ASSERT_EQUAL_UINT64(8192, iso_extent_part_bytes(22628, 4, 0));
    TEST_ASSERT_EQUAL_UINT64(8192, iso_extent_part_bytes(22628, 4, 1));
    TEST_ASSERT_EQUAL_UINT64(6244, iso_extent_part_bytes(22628, 4, 2));
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_part_bytes(22628, 4, 3));
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_part_bytes(22628, 4, 100));
    /* 大きさがちょうど 2 パーツ: 最後のパーツも満杯 */
    TEST_ASSERT_EQUAL_UINT64(8192, iso_extent_part_bytes(16384, 4, 1));
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_part_bytes(16384, 4, 2));
    /* 1 パーツ: 大きさ全体(import は part_blocks がファイルの大きさ) */
    TEST_ASSERT_EQUAL_UINT64(5000, iso_extent_part_bytes(5000, 3, 0)); /* 3 ブロック = 6144 >= 5000 */
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_part_bytes(0, 4, 0));
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_part_bytes(100, 0, 0));
    /* 4 GiB のパーツ (part_blocks = 2097152。part_blocks * 2048 は 32 ビットでは 0 に戻る) */
    TEST_ASSERT_EQUAL_UINT64(2, iso_extent_part_count(5 * GIB, 2097152));
    TEST_ASSERT_EQUAL_UINT64(4 * GIB, iso_extent_part_bytes(5 * GIB, 2097152, 0));
    TEST_ASSERT_EQUAL_UINT64(GIB, iso_extent_part_bytes(5 * GIB, 2097152, 1));
    /* index + 1 が 32 ビットで 0 に戻る位置: part_blocks = 1 (2048 B / パーツ)、パーツ数 = 2^32、最後のパーツは 1 バイト */
    const uint64_t big_size = (uint64_t)UINT32_MAX * 2048u + 1u;
    TEST_ASSERT_EQUAL_UINT64(4294967296ull, iso_extent_part_count(big_size, 1));
    TEST_ASSERT_EQUAL_UINT64(1, iso_extent_part_bytes(big_size, 1, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT64(2048, iso_extent_part_bytes(big_size, 1, UINT32_MAX - 1));
}

/* ---- 表の構築: 成功 ---- */

void test_build_table_a_attributes(void) {
    iso_extent_table_t *t = build_table_a();
    TEST_ASSERT_EQUAL_INT(1, H.allocs); /* 確保は 1 回だけ */
    TEST_ASSERT_EQUAL_UINT32(7, iso_extent_table_epoch(t));
    TEST_ASSERT_EQUAL_UINT64(22628, iso_extent_table_size_bytes(t));
    TEST_ASSERT_EQUAL_UINT64(12, iso_extent_table_capacity_blocks(t));
    TEST_ASSERT_EQUAL_UINT32(3, iso_extent_table_part_count(t));
    TEST_ASSERT_EQUAL_UINT32(4, iso_extent_table_part_blocks(t));
    TEST_ASSERT_EQUAL_UINT32(5, iso_extent_table_extent_count(t)); /* 2 + 1 + 2(クリップ後) */
    TEST_ASSERT_EQUAL_UINT32(1, iso_extent_table_refs(t));
    TEST_ASSERT_EQUAL_UINT32(0, iso_extent_table_unref(t));
    TEST_ASSERT_EQUAL_INT(1, H.frees);
}

void test_null_table_accessors_return_zero(void) {
    TEST_ASSERT_EQUAL_UINT32(0, iso_extent_table_refs(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, iso_extent_table_epoch(NULL));
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_table_size_bytes(NULL));
    TEST_ASSERT_EQUAL_UINT64(0, iso_extent_table_capacity_blocks(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, iso_extent_table_part_count(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, iso_extent_table_part_blocks(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, iso_extent_table_extent_count(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, iso_extent_table_unref(NULL));
    iso_extent_table_ref(NULL); /* 何も起きない */
}

void test_build_does_not_keep_pointers_to_the_input(void) {
    iso_extent_t p0[2] = {EXT(1000, 10), EXT(2000, 6)};
    iso_extent_t p1[1] = {EXT(2006, 16)};
    iso_extent_t p2[3] = {EXT(5000, 8), EXT(6000, 8), EXT(9000, 4)};
    iso_part_input_t in[3] = {{8192, p0, 2}, {8192, p1, 1}, {6244, p2, 3}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(1, 22628, 4, in, 3, &t));
    memset(p0, 0xEE, sizeof(p0));
    memset(p1, 0xEE, sizeof(p1));
    memset(p2, 0xEE, sizeof(p2));
    iso_extent_t sp[4];
    size_t n = 99;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(t, 0, 1, sp, 4, &n));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT64(1000, sp[0].start_sector);
    TEST_ASSERT_EQUAL_UINT64(4, sp[0].sector_count);
    iso_extent_table_unref(t);
}

void test_build_max_image_16_parts(void) {
    iso_extent_t e[16];
    iso_part_input_t in[16];
    for (int i = 0; i < 16; i++) {
        e[i].start_sector = 1000000ull + (uint64_t)i * 3000000ull;
        e[i].sector_count = 2097152; /* 1 GiB */
        in[i].file_bytes = GIB;
        in[i].extents = &e[i];
        in[i].n_extents = 1;
    }
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(3, 16 * GIB, 524288, in, 16, &t));
    TEST_ASSERT_EQUAL_UINT32(16, iso_extent_table_part_count(t));
    TEST_ASSERT_EQUAL_UINT64(8388608, iso_extent_table_capacity_blocks(t));
    TEST_ASSERT_EQUAL_UINT32(16, iso_extent_table_extent_count(t));
    TEST_ASSERT_EQUAL_UINT64(16 * GIB, iso_extent_valid_bytes(t, 0, 8388608));
    iso_extent_table_unref(t);
}

void test_build_single_part_import_with_odd_part_blocks(void) {
    /* import: 1 パーツで part_blocks = ceil(size / 2048)。size = 5000 -> 3 ブロック、10 セクタ */
    iso_extent_t e[1] = {EXT(77, 10)};
    iso_part_input_t in[1] = {{5000, e, 1}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 5000, 3, in, 1, &t));
    TEST_ASSERT_EQUAL_UINT64(3, iso_extent_table_capacity_blocks(t));
    iso_extent_table_unref(t);
}

/* ---- 表の構築: 引数と大きさの検査 ---- */

void test_build_invalid_args(void) {
    iso_extent_t e[1] = {EXT(0, 16)};
    iso_part_input_t in[1] = {{8192, e, 1}};
    iso_extent_table_t *t = (iso_extent_table_t *)0x1;
    iso_allocator_t a = ie_allocator();
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, iso_extent_table_build(&a, 0, 8192, 4, in, 1, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, iso_extent_table_build(NULL, 0, 8192, 4, in, 1, &t));
    TEST_ASSERT_NULL(t); /* 失敗のとき *out は NULL */
    iso_allocator_t b = a;
    b.alloc = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, iso_extent_table_build(&b, 0, 8192, 4, in, 1, &t));
    b = a;
    b.free = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, iso_extent_table_build(&b, 0, 8192, 4, in, 1, &t));
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, iso_extent_table_build(&a, 0, 8192, 4, NULL, 1, &t));
    /* 正常な呼び出しは通る(上の失敗が、引数 1 つだけのせいだと分かる) */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_table_build(&a, 0, 8192, 4, in, 1, &t));
    iso_extent_table_unref(t);
    TEST_ASSERT_EQUAL_INT(1, H.allocs); /* 失敗では何も確保していない */
}

void test_build_size_and_part_blocks_limits(void) {
    iso_extent_t e[1] = {EXT(0, 16)};
    iso_part_input_t in[1] = {{8192, e, 1}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, build(0, 0, 4, in, 1, &t)); /* size 0 */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, build(0, 8192, 0, in, 1, &t)); /* part_blocks 0 */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, build(0, 16 * GIB + 1, 524288, in, 1, &t));
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, build(0, 8192, 2097153, in, 1, &t)); /* part_blocks > 2097152 */
    /* part_blocks = 2097152 ちょうどは許す(4 GiB のパーツ。size は小さくてよい) */
    iso_extent_t e2[1] = {EXT(0, 16)};
    iso_part_input_t in2[1] = {{8192, e2, 1}};
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 8192, 2097152, in2, 1, &t));
    iso_extent_table_unref(t);
}

void test_build_image_size_limit_is_checked_by_itself(void) {
    /* 4 GiB のパーツなら、16 GiB + 1 でも 5 パーツ(16 以下)。大きさの上限だけが拒否する */
    iso_extent_t e[5] = {EXT(0, 8388608), EXT(0, 8388608), EXT(0, 8388608), EXT(0, 8388608), EXT(0, 1)};
    iso_part_input_t in[5];
    for (int i = 0; i < 5; i++) {
        in[i].file_bytes = (i < 4) ? 4 * GIB : 1;
        in[i].extents = &e[i];
        in[i].n_extents = 1;
    }
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, build(0, 16 * GIB + 1, 2097152, in, 5, &t));
    TEST_ASSERT_NULL(t);
    in[4].file_bytes = 0; /* 16 GiB ちょうど: 4 パーツ */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 16 * GIB, 2097152, in, 4, &t));
    TEST_ASSERT_EQUAL_UINT64(8388608, iso_extent_table_capacity_blocks(t));
    TEST_ASSERT_EQUAL_UINT64(16 * GIB, iso_extent_valid_bytes(t, 0, 8388608)); /* 32 ビットの積に収まらない */
    iso_extent_table_unref(t);
}

void test_build_rejects_more_than_16_parts_but_accepts_16(void) {
    /* part_blocks = 1 (2048 B / パーツ)。size = 16 * 2048 -> 16 パーツ。size = 16 * 2048 + 1 -> 17 パーツ(INVALID) */
    iso_extent_t e[17];
    iso_part_input_t in[17];
    for (int i = 0; i < 17; i++) {
        e[i].start_sector = (uint64_t)i * 100;
        e[i].sector_count = 4;
        in[i].file_bytes = 2048;
        in[i].extents = &e[i];
        in[i].n_extents = 1;
    }
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 16 * 2048, 1, in, 16, &t));
    TEST_ASSERT_EQUAL_UINT32(16, iso_extent_table_part_count(t));
    iso_extent_table_unref(t);
    in[16].file_bytes = 1;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, build(0, 16 * 2048 + 1, 1, in, 17, &t));
    TEST_ASSERT_NULL(t);
}

void test_build_part_count_mismatch(void) {
    iso_extent_t e[3] = {EXT(0, 16), EXT(100, 16), EXT(200, 13)};
    iso_part_input_t in[3] = {{8192, &e[0], 1}, {8192, &e[1], 1}, {6244, &e[2], 1}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_PART_MISMATCH, build(0, 22628, 4, in, 2, &t)); /* 1 つ足りない */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_PART_MISMATCH, build(0, 22628, 4, in, 0, &t));
    iso_part_input_t in4[4] = {in[0], in[1], in[2], {2048, &e[2], 1}};
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_PART_MISMATCH, build(0, 22628, 4, in4, 4, &t)); /* 1 つ多い */
    iso_part_input_t in4z[4] = {in[0], in[1], in[2], {0, NULL, 0}}; /* 余分なパーツが空でも、数が違えば不一致 */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_PART_MISMATCH, build(0, 22628, 4, in4z, 4, &t));
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 22628, 4, in, 3, &t));
    iso_extent_table_unref(t);
}

void test_build_part_size_mismatch(void) {
    iso_extent_t e[3] = {EXT(0, 16), EXT(100, 16), EXT(200, 13)};
    iso_extent_t big[3] = {EXT(0, 32), EXT(100, 32), EXT(200, 32)};
    iso_part_input_t ok[3] = {{8192, &e[0], 1}, {8192, &e[1], 1}, {6244, &e[2], 1}};
    iso_extent_table_t *t = NULL;
    for (int i = 0; i < 3; i++) {
        iso_part_input_t bad[3] = {ok[0], ok[1], ok[2]};
        bad[i].extents = &big[i];
        bad[i].file_bytes += 1; /* 1 バイト多い */
        TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_PART_MISMATCH, build(0, 22628, 4, bad, 3, &t));
        bad[i].file_bytes -= 2; /* 1 バイト少ない */
        TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_PART_MISMATCH, build(0, 22628, 4, bad, 3, &t));
    }
    TEST_ASSERT_NULL(t);
    TEST_ASSERT_EQUAL_INT(0, H.allocs);
}

/* ---- 表の構築: エクステントの検査とクリップ ---- */

void test_build_extents_must_cover_the_file(void) {
    iso_extent_table_t *t = NULL;
    /* 6244 B = 13 セクタ。12 セクタでは足りない、13 ちょうどは OK、非連続の複数でも OK */
    iso_extent_t short12[1] = {EXT(200, 12)};
    iso_extent_t exact13[1] = {EXT(200, 13)};
    iso_extent_t split13[2] = {EXT(200, 7), EXT(900, 6)};
    iso_extent_t e0[1] = {EXT(0, 16)}, e1[1] = {EXT(100, 16)};
    iso_part_input_t in[3] = {{8192, e0, 1}, {8192, e1, 1}, {6244, short12, 1}};
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_PART_MISMATCH, build(0, 22628, 4, in, 3, &t));
    in[2].extents = exact13;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 22628, 4, in, 3, &t));
    TEST_ASSERT_EQUAL_UINT32(3, iso_extent_table_extent_count(t));
    iso_extent_table_unref(t);
    in[2].extents = split13;
    in[2].n_extents = 2;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 22628, 4, in, 3, &t));
    TEST_ASSERT_EQUAL_UINT32(4, iso_extent_table_extent_count(t));
    iso_extent_table_unref(t);
    /* 最初のパーツ(満杯)が 15 セクタ分しかない */
    iso_extent_t s15[1] = {EXT(0, 15)};
    iso_part_input_t in2[3] = {{8192, s15, 1}, {8192, e1, 1}, {6244, exact13, 1}};
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_PART_MISMATCH, build(0, 22628, 4, in2, 3, &t));
    /* エクステントが 0 個 */
    iso_part_input_t in3[3] = {{8192, e0, 1}, {8192, NULL, 0}, {6244, exact13, 1}};
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_PART_MISMATCH, build(0, 22628, 4, in3, 3, &t));
}

void test_build_null_extents_with_nonzero_count_is_invalid(void) {
    iso_extent_t e1[1] = {EXT(100, 16)}, e2[1] = {EXT(200, 13)};
    iso_part_input_t in[3] = {{8192, NULL, 2}, {8192, e1, 1}, {6244, e2, 1}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, build(0, 22628, 4, in, 3, &t));
    in[0].n_extents = 1; /* 1 個でも NULL は不正 */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, build(0, 22628, 4, in, 3, &t));
}

void test_build_clips_tail_and_drops_zero_length(void) {
    /* 末尾の余分なエクステント (孤児のクラスタ) は捨てる。sector_count 0 は個数に数えない */
    iso_extent_t p0[4] = {EXT(1000, 0), EXT(1000, 10), EXT(77, 0), EXT(2000, 6)};
    iso_extent_t p1[2] = {EXT(2006, 16), EXT(50000, 8)}; /* 2 個目は丸ごと余分 */
    iso_extent_t p2[3] = {EXT(5000, 8), EXT(6000, 8), EXT(9000, 4)};
    iso_part_input_t in[3] = {{8192, p0, 4}, {8192, p1, 2}, {6244, p2, 3}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 22628, 4, in, 3, &t));
    TEST_ASSERT_EQUAL_UINT32(5, iso_extent_table_extent_count(t));
    /* 全体を変換すると、クリップ後の内容が出る: [1000,10] [2000,22] (パーツ 0 末尾とパーツ 1 は連続) [5000,8] [6000,5] */
    iso_extent_t sp[8];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(t, 0, 12, sp, 8, &n));
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_UINT64(1000, sp[0].start_sector);
    TEST_ASSERT_EQUAL_UINT64(10, sp[0].sector_count);
    TEST_ASSERT_EQUAL_UINT64(2000, sp[1].start_sector);
    TEST_ASSERT_EQUAL_UINT64(22, sp[1].sector_count);
    TEST_ASSERT_EQUAL_UINT64(5000, sp[2].start_sector);
    TEST_ASSERT_EQUAL_UINT64(8, sp[2].sector_count);
    TEST_ASSERT_EQUAL_UINT64(6000, sp[3].start_sector);
    TEST_ASSERT_EQUAL_UINT64(5, sp[3].sector_count);
    iso_extent_table_unref(t);
}

void test_build_clips_a_single_extent_longer_than_the_file(void) {
    /* 1 つのエクステントが、ファイルより長い (クラスタの端まで)。ファイル 5000 B = 10 セクタ */
    iso_extent_t e[1] = {EXT(77, 64)};
    iso_part_input_t in[1] = {{5000, e, 1}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 5000, 3, in, 1, &t));
    iso_extent_t sp[2];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(t, 0, 3, sp, 2, &n));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT64(77, sp[0].start_sector);
    TEST_ASSERT_EQUAL_UINT64(10, sp[0].sector_count);
    iso_extent_table_unref(t);
}

void test_build_extent_end_overflow_is_invalid(void) {
    iso_extent_t ok1[1] = {EXT(100, 16)};
    iso_extent_t ok2[1] = {EXT(200, 13)};
    iso_extent_table_t *t = NULL;
    iso_extent_t over[1] = {EXT(UINT64_MAX, 2)};
    iso_part_input_t in[3] = {{8192, over, 1}, {8192, ok1, 1}, {6244, ok2, 1}};
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, build(0, 22628, 4, in, 3, &t));
    /* start + count == UINT64_MAX ちょうどは、オーバーフローではない */
    iso_extent_t edge[1] = {EXT(UINT64_MAX - 16, 16)};
    in[0].extents = edge;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 22628, 4, in, 3, &t));
    iso_extent_table_unref(t);
    /* 余分なエクステント (クリップで捨てるもの) でも、オーバーフローは検査する前に捨てられるので問題にならない */
    iso_extent_t tail[2] = {EXT(0, 16), EXT(UINT64_MAX, 5)};
    in[0].extents = tail;
    in[0].n_extents = 2;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 22628, 4, in, 3, &t));
    iso_extent_table_unref(t);
}

/* ---- 表の構築: too_fragmented ---- */

static iso_extent_t *make_extents(size_t n, uint64_t count_each) {
    iso_extent_t *e = (iso_extent_t *)malloc(n * sizeof(iso_extent_t));
    for (size_t i = 0; i < n; i++) {
        e[i].start_sector = (uint64_t)i * 1000; /* 互いに離れている */
        e[i].sector_count = count_each;
    }
    return e;
}

void test_build_too_fragmented_boundary_single_part(void) {
    iso_extent_table_t *t = NULL;
    /* 4096 個(ファイルは 4096 セクタ = 2097152 B)はちょうど OK */
    iso_extent_t *e = make_extents(4097, 1);
    iso_part_input_t in[1] = {{4096ull * 512, e, 4096}};
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 4096ull * 512, ISO_PART_BLOCKS_DEFAULT, in, 1, &t));
    TEST_ASSERT_EQUAL_UINT32(4096, iso_extent_table_extent_count(t));
    iso_extent_table_unref(t);
    /* 4097 個必要なら too_fragmented */
    in[0].file_bytes = 4097ull * 512;
    in[0].n_extents = 4097;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_TOO_FRAGMENTED, build(0, 4097ull * 512, ISO_PART_BLOCKS_DEFAULT, in, 1, &t));
    TEST_ASSERT_NULL(t);
    free(e);
}

void test_build_too_fragmented_counts_after_clipping(void) {
    /* 5000 個渡されても、ファイルが 100 セクタなら、使うのは 100 個 */
    iso_extent_t *e = make_extents(5000, 1);
    iso_part_input_t in[1] = {{100 * 512, e, 5000}};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 100 * 512, ISO_PART_BLOCKS_DEFAULT, in, 1, &t));
    TEST_ASSERT_EQUAL_UINT32(100, iso_extent_table_extent_count(t));
    iso_extent_table_unref(t);
    free(e);
}

void test_build_too_fragmented_limit_is_for_the_whole_image(void) {
    /* part_blocks = 1024 (2 MiB = 4096 セクタ / パーツ)。パーツ 0 に 2048 個、パーツ 1 に 2048 個 = 4096 (OK)、2049 個 = 4097 (NG) */
    iso_extent_t *e0 = make_extents(2048, 2);
    iso_extent_t *e1 = make_extents(2049, 1);
    iso_part_input_t in[2] = {{2048ull * 2 * 512, e0, 2048}, {2048ull * 512, e1, 2048}};
    iso_extent_table_t *t = NULL;
    /* パーツ 0 は 4096 セクタ (2 MiB) でなければならない: 2048 個 x 2 セクタ */
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 2097152ull + 2048ull * 512, 1024, in, 2, &t));
    TEST_ASSERT_EQUAL_UINT32(4096, iso_extent_table_extent_count(t));
    iso_extent_table_unref(t);
    in[1].file_bytes = 2049ull * 512;
    in[1].n_extents = 2049;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_TOO_FRAGMENTED, build(0, 2097152ull + 2049ull * 512, 1024, in, 2, &t));
    free(e0);
    free(e1);
}

void test_build_checks_parts_in_order(void) {
    /* パーツの順に検査する(先のパーツの失敗が先に出る)。part_blocks = 2048 (4 MiB = 8192 セクタ / パーツ) */
    iso_extent_table_t *t = NULL;
    iso_extent_t *many = make_extents(8192, 1); /* 8192 個 x 1 セクタ = 8192 セクタ(too_fragmented) */
    iso_extent_t one[1] = {EXT(0, 8192)};
    iso_extent_t f1[1] = {EXT(0, 8)};
    uint64_t size = 8192ull * 512 + 100;
    /* パーツ 0 が too_fragmented で、パーツ 1 の大きさも不正 -> too_fragmented が先 */
    iso_part_input_t in2[2] = {{8192ull * 512, many, 8192}, {123, f1, 1}};
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_TOO_FRAGMENTED, build(0, size, 2048, in2, 2, &t));
    /* パーツ 0 が正常で、パーツ 1 の大きさが不正 -> part_mismatch */
    in2[0].extents = one;
    in2[0].n_extents = 1;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_PART_MISMATCH, build(0, size, 2048, in2, 2, &t));
    /* パーツ 0 の大きさが不正で、パーツ 1 の大きさも不正 -> part_mismatch (どちらでも同じ結果) */
    in2[0].file_bytes -= 1;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_PART_MISMATCH, build(0, size, 2048, in2, 2, &t));
    /* 両方正常 */
    in2[0].file_bytes += 1;
    in2[1].file_bytes = 100;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, size, 2048, in2, 2, &t));
    iso_extent_table_unref(t);
    free(many);
}

/* ---- メモリ ---- */

static iso_extent_t mut_extents[2];
static void mutate_input_in_alloc(void) { mut_extents[0].sector_count = 2; }

/* 契約違反(入力を alloc の最中に変える)でも、確保した範囲を超えて書かず、表を公開せずに解放する */
void test_build_second_pass_does_not_overrun_if_the_input_changes(void) {
    /* part_blocks = 1 (4 セクタ)。{100,4} {200,2}: 1 回目は 1 個(4 セクタで足りる)。alloc の最中に先頭が 2 セクタになる -> 2 回目は 2 個 */
    mut_extents[0].start_sector = 100;
    mut_extents[0].sector_count = 4;
    mut_extents[1].start_sector = 200;
    mut_extents[1].sector_count = 2;
    iso_part_input_t in[1] = {{2048, mut_extents, 2}};
    iso_extent_table_t *t = (iso_extent_table_t *)0x1;
    H.on_alloc = mutate_input_in_alloc;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, build(0, 2048, 1, in, 1, &t));
    H.on_alloc = NULL;
    TEST_ASSERT_NULL(t);
    TEST_ASSERT_EQUAL_INT(1, H.allocs);
    TEST_ASSERT_EQUAL_INT(1, H.frees); /* 確保した領域は解放した(tearDown でも見る) */
}

static iso_extent_t mut2_p0[1], mut2_p1[2];
static void mutate_second_part_in_alloc(void) { mut2_p1[0].sector_count = 2; }

/* 2 つ目のパーツの入力が変わる場合: 書き出しの容量は、表の全体ではなく、残り(全体 - すでに書いた分) */
void test_build_second_pass_capacity_is_the_remainder_of_the_table(void) {
    mut2_p0[0].start_sector = 100;
    mut2_p0[0].sector_count = 4;
    mut2_p1[0].start_sector = 200;
    mut2_p1[0].sector_count = 4;
    mut2_p1[1].start_sector = 300;
    mut2_p1[1].sector_count = 2;
    iso_part_input_t in[2] = {{2048, mut2_p0, 1}, {2048, mut2_p1, 2}};
    iso_extent_table_t *t = (iso_extent_table_t *)0x1;
    H.on_alloc = mutate_second_part_in_alloc;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_INVALID, build(0, 4096, 1, in, 2, &t));
    H.on_alloc = NULL;
    TEST_ASSERT_NULL(t);
    TEST_ASSERT_EQUAL_INT(H.allocs, H.frees);
}

void test_build_nomem(void) {
    iso_extent_t e0[1] = {EXT(0, 16)}, e1[1] = {EXT(100, 16)}, e2[1] = {EXT(200, 13)};
    iso_part_input_t in[3] = {{8192, e0, 1}, {8192, e1, 1}, {6244, e2, 1}};
    iso_extent_table_t *t = (iso_extent_table_t *)0x1;
    H.fail_next = 1;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_ERR_NOMEM, build(0, 22628, 4, in, 3, &t));
    TEST_ASSERT_NULL(t);
    TEST_ASSERT_EQUAL_INT(0, H.allocs);
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, build(0, 22628, 4, in, 3, &t)); /* 失敗を注入しなければ成功 */
    iso_extent_table_unref(t);
}

void test_refcount(void) {
    iso_extent_table_t *t = build_table_a();
    TEST_ASSERT_EQUAL_UINT32(1, iso_extent_table_refs(t));
    iso_extent_table_ref(t);
    iso_extent_table_ref(t);
    TEST_ASSERT_EQUAL_UINT32(3, iso_extent_table_refs(t));
    TEST_ASSERT_EQUAL_UINT32(2, iso_extent_table_unref(t));
    TEST_ASSERT_EQUAL_UINT32(1, iso_extent_table_unref(t));
    TEST_ASSERT_EQUAL_INT(0, H.frees); /* 参照が残っている間は解放しない(遅延解放) */
    /* 参照が残っている間、表は読める */
    iso_extent_t sp[2];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(t, 0, 1, sp, 2, &n));
    TEST_ASSERT_EQUAL_UINT32(0, iso_extent_table_unref(t));
    TEST_ASSERT_EQUAL_INT(1, H.frees);
}

void test_unref_frees_with_the_same_pointer_and_context(void) {
    iso_extent_table_t *t = build_table_a();
    void *p = H.last;
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_PTR(p, (void *)t); /* 1 回の確保で、表が先頭 */
    iso_extent_table_unref(t);
    TEST_ASSERT_NULL(H.last); /* ie_free が同じポインタで呼ばれた */
}

#define STRESS_THREADS 4
#define STRESS_ITERS 20000

static void *ref_unref_worker(void *arg) {
    iso_extent_table_t *t = (iso_extent_table_t *)arg;
    for (int i = 0; i < STRESS_ITERS; i++) {
        iso_extent_table_ref(t);
        (void)iso_extent_table_unref(t);
    }
    return NULL;
}

/* 参照カウントは原子的: 別のタスク(iso_msc_task の遅延した release など)から、同時に ref / unref してもずれない。 */
void test_refcount_is_atomic_across_threads(void) {
    iso_extent_table_t *t = build_table_a();
    pthread_t th[STRESS_THREADS];
    for (int i = 0; i < STRESS_THREADS; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&th[i], NULL, ref_unref_worker, t));
    }
    for (int i = 0; i < STRESS_THREADS; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(th[i], NULL));
    }
    TEST_ASSERT_EQUAL_UINT32(1, iso_extent_table_refs(t)); /* 作成時の 1 だけが残る */
    TEST_ASSERT_EQUAL_INT(0, H.frees);
    TEST_ASSERT_EQUAL_UINT32(0, iso_extent_table_unref(t));
    TEST_ASSERT_EQUAL_INT(1, H.frees);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_capacity_blocks_rounds_up);
    RUN_TEST(test_file_sectors_rounds_up);
    RUN_TEST(test_part_count_and_bytes);
    RUN_TEST(test_build_table_a_attributes);
    RUN_TEST(test_null_table_accessors_return_zero);
    RUN_TEST(test_build_does_not_keep_pointers_to_the_input);
    RUN_TEST(test_build_max_image_16_parts);
    RUN_TEST(test_build_single_part_import_with_odd_part_blocks);
    RUN_TEST(test_build_invalid_args);
    RUN_TEST(test_build_size_and_part_blocks_limits);
    RUN_TEST(test_build_image_size_limit_is_checked_by_itself);
    RUN_TEST(test_build_rejects_more_than_16_parts_but_accepts_16);
    RUN_TEST(test_build_part_count_mismatch);
    RUN_TEST(test_build_part_size_mismatch);
    RUN_TEST(test_build_extents_must_cover_the_file);
    RUN_TEST(test_build_null_extents_with_nonzero_count_is_invalid);
    RUN_TEST(test_build_clips_tail_and_drops_zero_length);
    RUN_TEST(test_build_clips_a_single_extent_longer_than_the_file);
    RUN_TEST(test_build_extent_end_overflow_is_invalid);
    RUN_TEST(test_build_too_fragmented_boundary_single_part);
    RUN_TEST(test_build_too_fragmented_counts_after_clipping);
    RUN_TEST(test_build_too_fragmented_limit_is_for_the_whole_image);
    RUN_TEST(test_build_checks_parts_in_order);
    RUN_TEST(test_build_second_pass_does_not_overrun_if_the_input_changes);
    RUN_TEST(test_build_second_pass_capacity_is_the_remainder_of_the_table);
    RUN_TEST(test_build_nomem);
    RUN_TEST(test_refcount);
    RUN_TEST(test_refcount_is_atomic_across_threads);
    RUN_TEST(test_unref_frees_with_the_same_pointer_and_context);
    return UNITY_END();
}

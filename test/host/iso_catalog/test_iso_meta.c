/*
 * iso_catalog: CRC-32、メタデータ 256 バイトの符号化と復号(設計 3.2)。期待するバイト列はすべて手書き
 * (CRC の値は、Python の zlib.crc32 で別に求めた定数)。
 */
#include "ic_common.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- CRC-32 ---- */

void test_crc32_known_vectors(void) {
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, iso_crc32("123456789", 9)); /* 標準のチェック値 */
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, iso_crc32("", 0));
    TEST_ASSERT_EQUAL_HEX32(0xE8B7BE43u, iso_crc32("a", 1));
    const uint8_t zeros[4] = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_HEX32(0x2144DF1Cu, iso_crc32(zeros, 4));
    const uint8_t ff[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, iso_crc32(ff, 4));
}

/* ---- 符号化 ---- */

/* 期待するバイト列 1: ready、size = 0x123456789、パーツ 5、sha[i] = i + 1 (計算値)、名前 "ubuntu-24.04"、世代 0x01020304 */
static const uint8_t EXPECT_A[256] = {
    /* 0 */ 'I', 'S', 'O', '1',
    /* 4 */ 0x01, 0x00,
    /* 6 */ 0x01,
    /* 7 */ 0x00,
    /* 8 */ 0x89, 0x67, 0x45, 0x23, 0x01, 0x00, 0x00, 0x00,
    /* 16 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 24 */ 0x05, 0x00,
    /* 26 */ 0x00, 0x00,
    /* 28 */ 0x00, 0x00, 0x08, 0x00,
    /* 32 */ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    /* 64 */ 0x02,
    /* 65 */ 0x0C,
    /* 66 */ 'u', 'b', 'u', 'n', 't', 'u', '-', '2', '4', '.', '0', '4',
    /* 78..129 は 0 */
    /* 130 は、下の初期化では添字で指定できないので、実行時に確認する */
};

static iso_meta_t meta_a(void) {
    iso_meta_t m;
    memset(&m, 0, sizeof(m));
    m.state = ISO_META_READY;
    m.invalid_reason = 0;
    m.size_bytes = 0x123456789ull;
    m.part_count = 5;
    m.part_blocks = 524288;
    for (int i = 0; i < 32; i++) m.sha256[i] = (uint8_t)(i + 1);
    m.sha_flag = ISO_SHA_COMPUTED;
    m.name_len = 12;
    memcpy(m.name, "ubuntu-24.04", 12);
    m.generation = 0x01020304u;
    return m;
}

/* スタックを 0xAA で汚す(符号化の内部の一時バッファが、予約領域を 0 にしていることを見るため) */
static void __attribute__((noinline)) dirty_stack(void) {
    volatile uint8_t junk[4096];
    for (size_t i = 0; i < sizeof(junk); i++) junk[i] = 0xAA;
}

void test_encode_exact_bytes_case_a(void) {
    iso_meta_t m = meta_a();
    uint8_t out[256];
    memset(out, 0xAA, sizeof(out)); /* 予約領域が 0 になることを見る */
    dirty_stack();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(EXPECT_A, out, 130); /* 0..129 */
    /* 130: 世代番号(リトルエンディアン) */
    TEST_ASSERT_EQUAL_HEX8(0x04, out[130]);
    TEST_ASSERT_EQUAL_HEX8(0x03, out[131]);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[132]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[133]);
    for (int i = 134; i < 252; i++) TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, out[i], "reserved @134..251 must be 0");
    /* 252: CRC-32 (先頭 252 バイト)。0x258DD8B1 */
    TEST_ASSERT_EQUAL_HEX8(0xB1, out[252]);
    TEST_ASSERT_EQUAL_HEX8(0xD8, out[253]);
    TEST_ASSERT_EQUAL_HEX8(0x8D, out[254]);
    TEST_ASSERT_EQUAL_HEX8(0x25, out[255]);
}

void test_encode_exact_bytes_minimum_values(void) {
    iso_meta_t m;
    memset(&m, 0, sizeof(m));
    m.state = ISO_META_UPLOADING;
    m.size_bytes = 1;
    m.part_count = 1;
    m.part_blocks = 1;
    m.name_len = 1;
    m.name[0] = 'a';
    m.generation = 0;
    uint8_t out[256];
    memset(out, 0xAA, sizeof(out));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, out));
    uint8_t exp[256];
    memset(exp, 0, sizeof(exp));
    memcpy(exp, "ISO1", 4);
    exp[4] = 1;
    exp[8] = 1;
    exp[24] = 1;
    exp[28] = 1;
    exp[65] = 1;
    exp[66] = 'a';
    exp[252] = 0xB1; /* CRC 0x3D5027B1 */
    exp[253] = 0x27;
    exp[254] = 0x50;
    exp[255] = 0x3D;
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, out, 256);
}

void test_encode_exact_bytes_maximum_values(void) {
    iso_meta_t m;
    memset(&m, 0, sizeof(m));
    m.state = ISO_META_DELETING;
    m.invalid_reason = ISO_INVALID_PART_MISMATCH;
    m.size_bytes = 17179869184ull; /* 16 GiB = 0x4_0000_0000 */
    m.part_count = 16;
    m.part_blocks = 2097152;
    memset(m.sha256, 0xFF, 32);
    m.sha_flag = ISO_SHA_DECLARED;
    m.name_len = 63;
    memset(m.name, 'x', 63);
    m.generation = 0xFFFFFFFFu;
    uint8_t out[256];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, out));
    uint8_t exp[256];
    memset(exp, 0, sizeof(exp));
    memcpy(exp, "ISO1", 4);
    exp[4] = 1;
    exp[6] = 4;
    exp[7] = 4;
    exp[12] = 0x04; /* size の 5 バイト目 */
    exp[24] = 16;
    exp[30] = 0x20; /* 2097152 = 0x00200000 */
    memset(exp + 32, 0xFF, 32);
    exp[64] = 1;
    exp[65] = 63;
    memset(exp + 66, 'x', 63);
    memset(exp + 130, 0xFF, 4);
    exp[252] = 0x64; /* CRC 0x8B1AE264 */
    exp[253] = 0xE2;
    exp[254] = 0x1A;
    exp[255] = 0x8B;
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, out, 256);
    /* 名前の枠 66..129 のうち、最後の 1 バイト (129) は 0 のまま(63 バイト + 終端) */
    TEST_ASSERT_EQUAL_HEX8(0x00, out[129]);
}

void test_decode_roundtrip(void) {
    iso_meta_t cases[3];
    cases[0] = meta_a();
    memset(&cases[1], 0, sizeof(cases[1]));
    cases[1].state = ISO_META_UPLOADING;
    cases[1].size_bytes = 1;
    cases[1].part_count = 1;
    cases[1].part_blocks = 1;
    cases[1].name_len = 1;
    cases[1].name[0] = 'a';
    memset(&cases[2], 0, sizeof(cases[2]));
    cases[2].state = ISO_META_DELETING;
    cases[2].invalid_reason = 4;
    cases[2].size_bytes = 17179869184ull;
    cases[2].part_count = 16;
    cases[2].part_blocks = 2097152;
    memset(cases[2].sha256, 0xFF, 32);
    cases[2].sha_flag = 1;
    cases[2].name_len = 63;
    memset(cases[2].name, 'x', 63);
    cases[2].generation = 0xFFFFFFFFu;
    for (int i = 0; i < 3; i++) {
        uint8_t b[256];
        iso_meta_t d;
        memset(&d, 0x5A, sizeof(d));
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&cases[i], b));
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_decode(b, 256, &d));
        TEST_ASSERT_EQUAL_UINT8(cases[i].state, d.state);
        TEST_ASSERT_EQUAL_UINT8(cases[i].invalid_reason, d.invalid_reason);
        TEST_ASSERT_EQUAL_UINT64(cases[i].size_bytes, d.size_bytes);
        TEST_ASSERT_EQUAL_UINT16(cases[i].part_count, d.part_count);
        TEST_ASSERT_EQUAL_UINT32(cases[i].part_blocks, d.part_blocks);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(cases[i].sha256, d.sha256, 32);
        TEST_ASSERT_EQUAL_UINT8(cases[i].sha_flag, d.sha_flag);
        TEST_ASSERT_EQUAL_UINT8(cases[i].name_len, d.name_len);
        TEST_ASSERT_EQUAL_STRING_LEN(cases[i].name, d.name, cases[i].name_len);
        TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)d.name[d.name_len]); /* NUL 終端 */
        TEST_ASSERT_EQUAL_UINT32(cases[i].generation, d.generation);
    }
}

void test_decode_expected_bytes_case_a(void) {
    /* 手書きのバイト列(EXPECT_A + 世代 + CRC)を復号する */
    uint8_t b[256];
    memcpy(b, EXPECT_A, 130);
    memset(b + 130, 0, 126);
    b[130] = 0x04;
    b[131] = 0x03;
    b[132] = 0x02;
    b[133] = 0x01;
    b[252] = 0xB1;
    b[253] = 0xD8;
    b[254] = 0x8D;
    b[255] = 0x25;
    iso_meta_t d;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_decode(b, 256, &d));
    TEST_ASSERT_EQUAL_UINT64(0x123456789ull, d.size_bytes);
    TEST_ASSERT_EQUAL_UINT16(5, d.part_count);
    TEST_ASSERT_EQUAL_UINT32(524288, d.part_blocks);
    TEST_ASSERT_EQUAL_STRING("ubuntu-24.04", d.name);
    TEST_ASSERT_EQUAL_UINT32(0x01020304u, d.generation);
}

/* ---- 復号の拒否 ---- */

static void fix_crc(uint8_t *b) {
    uint32_t c = iso_crc32(b, 252);
    b[252] = (uint8_t)c;
    b[253] = (uint8_t)(c >> 8);
    b[254] = (uint8_t)(c >> 16);
    b[255] = (uint8_t)(c >> 24);
}

static void good(uint8_t *b) {
    iso_meta_t m = meta_a();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, b));
}

#define EXPECT_REJECT(b) TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_decode((b), 256, &d))
#define EXPECT_ACCEPT(b) TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_decode((b), 256, &d))

void test_decode_rejects_wrong_lengths(void) {
    uint8_t b[300];
    memset(b, 0, sizeof(b));
    good(b);
    iso_meta_t d;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_decode(b, 256, &d));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_decode(b, 255, &d));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_decode(b, 257, &d));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_decode(b, 0, &d));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_decode(b, 1, &d));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_decode(b, 128, &d));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_decode(b, 300, &d));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_decode(NULL, 256, &d));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_decode(b, 256, NULL));
}

void test_decode_rejects_bad_magic_each_byte(void) {
    iso_meta_t d;
    for (int i = 0; i < 4; i++) {
        uint8_t b[256];
        good(b);
        b[i] ^= 0x20; /* 'I' -> 'i' など */
        fix_crc(b);   /* CRC は合っている。マジックだけが違う */
        EXPECT_REJECT(b);
    }
}

void test_decode_rejects_bad_version(void) {
    iso_meta_t d;
    const uint8_t vers[][2] = {{0, 0}, {2, 0}, {0, 1}, {1, 1}, {0xFF, 0xFF}};
    for (size_t i = 0; i < sizeof(vers) / sizeof(vers[0]); i++) {
        uint8_t b[256];
        good(b);
        b[4] = vers[i][0];
        b[5] = vers[i][1];
        fix_crc(b);
        EXPECT_REJECT(b);
    }
}

void test_decode_rejects_each_crc_byte(void) {
    iso_meta_t d;
    for (int i = 252; i < 256; i++) {
        uint8_t b[256];
        good(b);
        b[i] ^= 0x01;
        EXPECT_REJECT(b);
    }
}

void test_decode_rejects_any_single_bit_flip_in_the_body(void) {
    /* 先頭の 252 バイトのどの 1 ビットが変わっても、CRC が合わず拒否される(予約領域を含む) */
    iso_meta_t d;
    for (int i = 0; i < 252; i++) {
        for (int bit = 0; bit < 8; bit += 7) { /* 最下位と最上位のビット */
            uint8_t b[256];
            good(b);
            b[i] ^= (uint8_t)(1u << bit);
            EXPECT_REJECT(b);
        }
    }
}

void test_decode_rejects_out_of_range_fields_with_valid_crc(void) {
    iso_meta_t d;
    uint8_t b[256];
    /* 状態 */
    good(b); b[6] = 4; fix_crc(b); EXPECT_ACCEPT(b);
    good(b); b[6] = 5; fix_crc(b); EXPECT_REJECT(b);
    good(b); b[6] = 0xFF; fix_crc(b); EXPECT_REJECT(b);
    /* invalid の理由 */
    good(b); b[7] = 4; fix_crc(b); EXPECT_ACCEPT(b);
    good(b); b[7] = 5; fix_crc(b); EXPECT_REJECT(b); /* meta_corrupt は保存しない */
    good(b); b[7] = 6; fix_crc(b); EXPECT_REJECT(b);
    /* size_bytes の上位バイト(@15)が非 0: 下位 7 バイトだけを読むと小さな正しい値に見えてしまう */
    good(b); memset(b + 8, 0, 8); b[8] = 5; b[15] = 0x01; fix_crc(b); EXPECT_REJECT(b);
    /* size_bytes: 0 は拒否、16 GiB ちょうどは許す、16 GiB + 1 は拒否 */
    good(b); memset(b + 8, 0, 8); fix_crc(b); EXPECT_REJECT(b);
    good(b); memset(b + 8, 0, 8); b[12] = 0x04; fix_crc(b); EXPECT_ACCEPT(b); /* 0x4_0000_0000 */
    good(b); memset(b + 8, 0, 8); b[12] = 0x04; b[8] = 0x01; fix_crc(b); EXPECT_REJECT(b);
    good(b); memset(b + 8, 0xFF, 8); fix_crc(b); EXPECT_REJECT(b);
    good(b); memset(b + 8, 0, 8); b[8] = 0x01; fix_crc(b); EXPECT_ACCEPT(b); /* 1 */
    /* パーツ数: 0 と 17 は拒否、16 は許す(上位バイトが非 0 でも拒否) */
    good(b); b[24] = 0; b[25] = 0; fix_crc(b); EXPECT_REJECT(b);
    good(b); b[24] = 16; b[25] = 0; fix_crc(b); EXPECT_ACCEPT(b);
    good(b); b[24] = 17; b[25] = 0; fix_crc(b); EXPECT_REJECT(b);
    good(b); b[24] = 1; b[25] = 1; fix_crc(b); EXPECT_REJECT(b); /* 257 */
    /* パーツのブロック数: 0 は拒否、2097152 は許す、2097153 は拒否 */
    good(b); memset(b + 28, 0, 4); fix_crc(b); EXPECT_REJECT(b);
    good(b); memset(b + 28, 0, 4); b[30] = 0x20; fix_crc(b); EXPECT_ACCEPT(b);
    good(b); memset(b + 28, 0, 4); b[30] = 0x20; b[28] = 1; fix_crc(b); EXPECT_REJECT(b);
    good(b); memset(b + 28, 0, 4); b[31] = 0x01; fix_crc(b); EXPECT_REJECT(b); /* 0x01000000 */
    good(b); memset(b + 28, 0, 4); b[28] = 1; fix_crc(b); EXPECT_ACCEPT(b);
    /* SHA-256 のフラグ */
    good(b); b[64] = 0; fix_crc(b); EXPECT_ACCEPT(b);
    good(b); b[64] = 2; fix_crc(b); EXPECT_ACCEPT(b);
    good(b); b[64] = 3; fix_crc(b); EXPECT_REJECT(b);
    good(b); b[64] = 0xFF; fix_crc(b); EXPECT_REJECT(b);
}

void test_decode_rejects_bad_name_length(void) {
    iso_meta_t d;
    uint8_t b[256];
    good(b); b[65] = 0; fix_crc(b); EXPECT_REJECT(b);
    good(b); b[65] = 1; fix_crc(b); EXPECT_ACCEPT(b); /* "u" */
    good(b); b[65] = 63; memset(b + 66, 'k', 63); fix_crc(b); EXPECT_ACCEPT(b);
    good(b); b[65] = 64; memset(b + 66, 'k', 64); fix_crc(b); EXPECT_REJECT(b);
    good(b); b[65] = 65; memset(b + 66, 'k', 64); fix_crc(b); EXPECT_REJECT(b);
    good(b); b[65] = 255; memset(b + 66, 'k', 64); fix_crc(b); EXPECT_REJECT(b);
    /* 名前の長さ 63 のとき、枠の最後の 1 バイト (129) が非 0 でも、名前には入らない(長さが決める) */
    good(b); b[65] = 12; b[129] = 'Z'; fix_crc(b); EXPECT_ACCEPT(b);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_decode(b, 256, &d));
    TEST_ASSERT_EQUAL_STRING("ubuntu-24.04", d.name);
}

void test_decode_applies_the_name_rule(void) {
    iso_meta_t d;
    uint8_t b[256];
    /* 前後の空白、制御文字、'/'、'\\'、不正な UTF-8 は拒否 */
    good(b); memset(b + 66, 0, 64); memcpy(b + 66, " abc", 4); b[65] = 4; fix_crc(b); EXPECT_REJECT(b);
    good(b); memset(b + 66, 0, 64); memcpy(b + 66, "abc ", 4); b[65] = 4; fix_crc(b); EXPECT_REJECT(b);
    good(b); memset(b + 66, 0, 64); memcpy(b + 66, "a\x01" "b", 3); b[65] = 3; fix_crc(b); EXPECT_REJECT(b);
    good(b); memset(b + 66, 0, 64); memcpy(b + 66, "a/b", 3); b[65] = 3; fix_crc(b); EXPECT_REJECT(b);
    good(b); memset(b + 66, 0, 64); memcpy(b + 66, "a\\b", 3); b[65] = 3; fix_crc(b); EXPECT_REJECT(b);
    good(b); memset(b + 66, 0, 64); memcpy(b + 66, "a\xC0\xAF" "b", 4); b[65] = 4; fix_crc(b); EXPECT_REJECT(b);
    good(b); memset(b + 66, 0, 64); memcpy(b + 66, "ab\x00", 3); b[65] = 3; fix_crc(b); EXPECT_REJECT(b); /* 名前の中の NUL */
    /* 日本語の名前は通る: "ISO" + U+30A4 U+30E1 U+30FC U+30B8 */
    good(b); memset(b + 66, 0, 64);
    memcpy(b + 66, "\xE3\x82\xA4\xE3\x83\xA1\xE3\x83\xBC\xE3\x82\xB8", 12); b[65] = 12; fix_crc(b);
    EXPECT_ACCEPT(b);
}

void test_decode_ignores_reserved_bytes(void) {
    /* 予約領域(16..23、26..27、134..251)の内容は見ない(CRC が守る)。非 0 でも CRC が合えば復号できる */
    iso_meta_t d;
    uint8_t b[256];
    good(b); b[16] = 1; b[26] = 1; b[134] = 1; b[251] = 1; fix_crc(b); EXPECT_ACCEPT(b);
    TEST_ASSERT_EQUAL_UINT64(0x123456789ull, d.size_bytes);
}

void test_decode_does_not_touch_out_on_failure(void) {
    iso_meta_t d;
    memset(&d, 0x5A, sizeof(d));
    uint8_t b[256];
    good(b);
    b[100] ^= 1; /* CRC 不一致 */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_decode(b, 256, &d));
    uint8_t *p = (uint8_t *)&d;
    for (size_t i = 0; i < sizeof(d); i++) TEST_ASSERT_EQUAL_HEX8(0x5A, p[i]);
    /* 範囲外の値(CRC は合う)でも同じ */
    good(b); b[6] = 9; fix_crc(b);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_decode(b, 256, &d));
    for (size_t i = 0; i < sizeof(d); i++) TEST_ASSERT_EQUAL_HEX8(0x5A, p[i]);
}

/* ---- 符号化の拒否 ---- */

void test_encode_rejects_out_of_range_values(void) {
    uint8_t out[256];
    memset(out, 0x77, sizeof(out));
    iso_meta_t base = meta_a();
    iso_meta_t m;

    m = base; m.state = 5; TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&m, out));
    m = base; m.invalid_reason = 5; TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&m, out));
    m = base; m.size_bytes = 0; TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&m, out));
    m = base; m.size_bytes = 17179869185ull; TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&m, out));
    m = base; m.part_count = 0; TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&m, out));
    m = base; m.part_count = 17; TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&m, out));
    m = base; m.part_blocks = 0; TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&m, out));
    m = base; m.part_blocks = 2097153; TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&m, out));
    m = base; m.sha_flag = 3; TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&m, out));
    m = base; m.name_len = 0; TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&m, out));
    m = base; m.name_len = 64; memset(m.name, 'a', 64); TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&m, out));
    m = base; m.name[0] = '/'; TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&m, out));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(NULL, out));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_meta_encode(&base, NULL));
    /* 拒否したときは out を書かない */
    for (int i = 0; i < 256; i++) TEST_ASSERT_EQUAL_HEX8(0x77, out[i]);
    /* 境界は通る */
    m = base; m.part_count = 16; TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, out));
    m = base; m.part_blocks = 2097152; TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, out));
    m = base; m.size_bytes = 17179869184ull; TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, out));
    m = base; m.invalid_reason = 4; TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, out));
    m = base; m.state = 4; TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, out));
    m = base; m.sha_flag = 0; TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, out));
}

void test_encode_name_padding_is_zero_even_if_the_struct_has_garbage(void) {
    iso_meta_t m = meta_a();
    m.name_len = 5;
    memcpy(m.name, "abcde", 5);
    m.name[10] = 'Z'; /* 名前の長さより後ろのごみ */
    m.name[63] = 'Y';
    uint8_t out[256];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, out));
    for (int i = 66 + 5; i < 130; i++) TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, out[i], "name padding must be 0");
}

void test_encode_name_64_byte_field_does_not_overlap_generation(void) {
    /* 63 バイトの名前でも、世代番号 (130) と CRC (252) に重ならない */
    iso_meta_t m = meta_a();
    m.name_len = 63;
    memset(m.name, 'n', 63);
    m.generation = 0xA1B2C3D4u;
    uint8_t out[256];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_encode(&m, out));
    iso_meta_t d;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_meta_decode(out, 256, &d));
    TEST_ASSERT_EQUAL_UINT32(0xA1B2C3D4u, d.generation);
    TEST_ASSERT_EQUAL_UINT8(63, d.name_len);
    TEST_ASSERT_EQUAL_HEX8(0xD4, out[130]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_crc32_known_vectors);
    RUN_TEST(test_encode_exact_bytes_case_a);
    RUN_TEST(test_encode_exact_bytes_minimum_values);
    RUN_TEST(test_encode_exact_bytes_maximum_values);
    RUN_TEST(test_decode_roundtrip);
    RUN_TEST(test_decode_expected_bytes_case_a);
    RUN_TEST(test_decode_rejects_wrong_lengths);
    RUN_TEST(test_decode_rejects_bad_magic_each_byte);
    RUN_TEST(test_decode_rejects_bad_version);
    RUN_TEST(test_decode_rejects_each_crc_byte);
    RUN_TEST(test_decode_rejects_any_single_bit_flip_in_the_body);
    RUN_TEST(test_decode_rejects_out_of_range_fields_with_valid_crc);
    RUN_TEST(test_decode_rejects_bad_name_length);
    RUN_TEST(test_decode_applies_the_name_rule);
    RUN_TEST(test_decode_ignores_reserved_bytes);
    RUN_TEST(test_decode_does_not_touch_out_on_failure);
    RUN_TEST(test_encode_rejects_out_of_range_values);
    RUN_TEST(test_encode_name_padding_is_zero_even_if_the_struct_has_garbage);
    RUN_TEST(test_encode_name_64_byte_field_does_not_overlap_generation);
    return UNITY_END();
}

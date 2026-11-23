/*
 * テスト用の SHA-256 参照実装(up_sha256.c)の検査。NIST の既知のテストベクトル("abc"、空、448 ビット、100 万個の 'a')と、
 * Python の hashlib で別に求めた定数。このあとのテストの期待値(宣言値との照合)は、この実装に依存するので、先に固定する。
 * さらに、iso_upload が使う iso_sha256_t のインターフェース(状態を呼び出し側が確保する)としての使い方も確かめる。
 */
#include "up_common.h"

void setUp(void) {}
void tearDown(void) {}

static void expect_hex(const void *data, size_t len, const char *hex) {
    uint8_t out[32], want[32];
    up_sha256(data, len, out);
    hex32(hex, want);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 32);
}

void test_nist_abc(void) { expect_hex("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); }
void test_nist_empty(void) { expect_hex("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"); }
void test_nist_448_bits(void) {
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    expect_hex(m, strlen(m), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

void test_nist_one_million_a(void) {
    up_sha256_state_t s;
    uint8_t chunk[1000], out[32], want[32];
    memset(chunk, 'a', sizeof(chunk));
    up_sha256_init(&s);
    for (int i = 0; i < 1000; i++) up_sha256_update(&s, chunk, sizeof(chunk));
    up_sha256_final(&s, out);
    hex32("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", want);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 32);
}

/* パディングの境界(55 / 56 / 63 / 64 / 65 / 119 / 120 / 128 バイト)。データは (i * 7 + 3) & 0xFF */
void test_padding_boundaries(void) {
    static const struct {
        size_t n;
        const char *hex;
    } T[] = {
        {1, "084fed08b978af4d7d196a7446a86b58009e636b611db16211b65a9aadff29c5"},
        {55, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
        {56, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27"},
        {63, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
        {64, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
        {65, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e"},
        {119, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e"},
        {120, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5"},
        {128, "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6"},
    };
    uint8_t data[128];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)((i * 7 + 3) & 0xFF);
    for (size_t k = 0; k < sizeof(T) / sizeof(T[0]); k++) expect_hex(data, T[k].n, T[k].hex);
}

/* どこで分けて update しても同じ(パーツをまたぐ分け方の参照) */
void test_split_updates_equal_one_shot(void) {
    uint8_t data[300], one[32], two[32];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 31 + 5);
    up_sha256(data, sizeof(data), one);
    for (size_t cut = 0; cut <= sizeof(data); cut += 7) {
        up_sha256_state_t s;
        up_sha256_init(&s);
        up_sha256_update(&s, data, cut);
        up_sha256_update(&s, data + cut, sizeof(data) - cut);
        up_sha256_final(&s, two);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(one, two, 32);
    }
}

void test_iface_has_the_state_size_and_runs(void) {
    iso_sha256_t i = up_sha256_iface();
    TEST_ASSERT_EQUAL_UINT(sizeof(up_sha256_state_t), i.state_size);
    up_sha256_state_t st;
    uint8_t out[32], want[32];
    i.init(&st);
    i.update(&st, "ab", 2);
    i.update(&st, "c", 1);
    TEST_ASSERT_EQUAL_INT(ISO_OK, i.final(&st, out));
    hex32("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 32);
}

/* gen_sha(4 KiB ずつ)は、一括の計算と同じ(チャンクの分け方に依存しない参照になっている) */
void test_gen_sha_matches_a_one_shot_computation(void) {
    static uint8_t big[100000];
    gen_fill(big, 0, sizeof(big));
    uint8_t a[32], b[32];
    up_sha256(big, sizeof(big), a);
    gen_sha(sizeof(big), b);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a, b, 32);
}

void test_gen_byte_places_the_signature_in_sector_16(void) {
    SIG = "CD001";
    TEST_ASSERT_EQUAL_UINT8('C', gen_byte(32769));
    TEST_ASSERT_EQUAL_UINT8('1', gen_byte(32773));
    SIG = "BEA01";
    TEST_ASSERT_EQUAL_UINT8('B', gen_byte(32769));
    TEST_ASSERT_EQUAL_UINT8('1', gen_byte(32773));
    SIG = "CD001";
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_nist_abc);
    RUN_TEST(test_nist_empty);
    RUN_TEST(test_nist_448_bits);
    RUN_TEST(test_nist_one_million_a);
    RUN_TEST(test_padding_boundaries);
    RUN_TEST(test_split_updates_equal_one_shot);
    RUN_TEST(test_iface_has_the_state_size_and_runs);
    RUN_TEST(test_gen_sha_matches_a_one_shot_computation);
    RUN_TEST(test_gen_byte_places_the_signature_in_sector_16);
    return UNITY_END();
}

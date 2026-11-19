/*
 * iso_catalog: 2 つのスロット (meta.a / meta.b) の選択と読み込み(設計 3.2)。期待値は手書き。
 */
#include "ic_common.h"

void setUp(void) { fx_setup(); }
void tearDown(void) { fx_teardown(); }

typedef struct {
    bool av;
    uint32_t ag;
    bool bv;
    uint32_t bg;
    int expect;
    const char *why;
} choose_case_t;

void test_slot_choose_table(void) {
    static const choose_case_t cs[] = {
        {false, 0, false, 0, -1, "neither valid"},
        {false, 9, false, 9, -1, "neither valid, generations ignored"},
        {true, 5, false, 0, 0, "only a"},
        {false, 0, true, 5, 1, "only b"},
        {true, 5, true, 5, 0, "same generation -> a"},
        {true, 0, true, 0, 0, "same generation 0 -> a"},
        {true, 5, true, 6, 1, "b is newer"},
        {true, 6, true, 5, 0, "a is newer"},
        {true, 0xFFFFFFFFu, true, 0, 1, "b wrapped around: 0 is newer than UINT32_MAX"},
        {true, 0, true, 0xFFFFFFFFu, 0, "a is newer than UINT32_MAX (which is 0 - 1)"},
        {true, 0x7FFFFFFFu, true, 0x80000000u, 1, "diff +1 across the sign boundary"},
        {true, 0x80000000u, true, 0x7FFFFFFFu, 0, "diff -1 across the sign boundary"},
        {true, 0, true, 0x7FFFFFFFu, 1, "diff INT32_MAX: b is newer"},
        {true, 0x7FFFFFFFu, true, 0, 0, "diff -INT32_MAX: a is newer"},
        {true, 0, true, 0x80000000u, 0, "diff INT32_MIN: not newer -> a"},
        {true, 0x80000000u, true, 0, 0, "diff INT32_MIN (other direction): not newer -> a"},
        {true, 0, true, 1, 1, "0 -> 1"},
        {true, 1, true, 0, 0, "1 vs 0"},
        {true, 0xFFFFFFFEu, true, 0xFFFFFFFFu, 1, "UINT32_MAX - 1 -> UINT32_MAX"},
        {false, 9, true, 1, 1, "invalid a is ignored even with a higher generation"},
        {true, 1, false, 9, 0, "invalid b is ignored even with a higher generation"},
    };
    for (size_t i = 0; i < sizeof(cs) / sizeof(cs[0]); i++) {
        char msg[100];
        snprintf(msg, sizeof(msg), "case %zu: %s", i, cs[i].why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cs[i].expect, iso_slot_choose(cs[i].av, cs[i].ag, cs[i].bv, cs[i].bg), msg);
    }
}

void test_slots_read_directory_without_meta(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/a1b2c3d4"));
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_FALSE(s.valid[0]);
    TEST_ASSERT_FALSE(s.valid[1]);
    TEST_ASSERT_EQUAL_INT8(-1, s.adopted);
    /* ディレクトリ自体がなくても同じ */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "deadbeef", &s));
    TEST_ASSERT_EQUAL_INT8(-1, s.adopted);
}

void test_slots_read_only_a_and_only_b(void) {
    iso_meta_t m = mk_meta("Alpha", 100000, ISO_META_READY, 7);
    put_slot("a1b2c3d4", 0, &m);
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_TRUE(s.valid[0]);
    TEST_ASSERT_FALSE(s.valid[1]);
    TEST_ASSERT_EQUAL_INT8(0, s.adopted);
    TEST_ASSERT_EQUAL_UINT32(7, s.meta[0].generation);
    TEST_ASSERT_EQUAL_STRING("Alpha", s.meta[0].name);

    m = mk_meta("Beta", 100000, ISO_META_INCOMPLETE, 8);
    put_slot("0f9e8d7c", 1, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "0f9e8d7c", &s));
    TEST_ASSERT_FALSE(s.valid[0]);
    TEST_ASSERT_TRUE(s.valid[1]);
    TEST_ASSERT_EQUAL_INT8(1, s.adopted);
    TEST_ASSERT_EQUAL_STRING("Beta", s.meta[1].name);
}

void test_slots_read_picks_newer_generation(void) {
    iso_meta_t a = mk_meta("Old", 100000, ISO_META_UPLOADING, 3);
    iso_meta_t b = mk_meta("New", 100000, ISO_META_READY, 4);
    put_slot("a1b2c3d4", 0, &a);
    put_slot("a1b2c3d4", 1, &b);
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_TRUE(s.valid[0]);
    TEST_ASSERT_TRUE(s.valid[1]);
    TEST_ASSERT_EQUAL_INT8(1, s.adopted);
    /* a の方が新しい */
    a.generation = 10;
    put_slot("a1b2c3d4", 0, &a);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_EQUAL_INT8(0, s.adopted);
    /* 同世代は a */
    b.generation = 10;
    put_slot("a1b2c3d4", 1, &b);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_EQUAL_INT8(0, s.adopted);
}

void test_slots_read_generation_wraparound(void) {
    iso_meta_t a = mk_meta("A", 100000, ISO_META_UPLOADING, 0xFFFFFFFFu);
    iso_meta_t b = mk_meta("B", 100000, ISO_META_READY, 0);
    put_slot("a1b2c3d4", 0, &a);
    put_slot("a1b2c3d4", 1, &b);
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_EQUAL_INT8(1, s.adopted); /* 0 は UINT32_MAX の次 = 新しい */
}

void test_slots_read_slot_length_boundaries(void) {
    iso_meta_t m = mk_meta("Alpha", 100000, ISO_META_READY, 5);
    put_slot("a1b2c3d4", 0, &m);
    iso_slots_t s;
    /* 255 バイト(作成直後に電源が落ちたような、短いファイル) */
    TEST_ASSERT_TRUE(mock_fs_set_size(&FS, "/iso/a1b2c3d4/meta.a", 255));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_FALSE(s.valid[0]);
    TEST_ASSERT_EQUAL_INT8(-1, s.adopted);
    /* 257 バイト(内容は 256 バイトが正しくても、長さが違う) */
    put_slot("a1b2c3d4", 0, &m);
    TEST_ASSERT_TRUE(mock_fs_set_size(&FS, "/iso/a1b2c3d4/meta.a", 257));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_FALSE(s.valid[0]);
    TEST_ASSERT_EQUAL_INT8(-1, s.adopted);
    /* 256 バイトは有効 */
    put_slot("a1b2c3d4", 0, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_TRUE(s.valid[0]);
    /* 0 バイト、1 バイト、128 バイト(半分だけ書けた) */
    const uint64_t lens[] = {0, 1, 128, 254, 258, 512};
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        put_slot("a1b2c3d4", 0, &m);
        TEST_ASSERT_TRUE(mock_fs_set_size(&FS, "/iso/a1b2c3d4/meta.a", lens[i]));
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
        TEST_ASSERT_FALSE_MESSAGE(s.valid[0], "short or long slot must be invalid");
    }
}

void test_slots_read_corrupt_slot_falls_back_to_the_other(void) {
    iso_meta_t a = mk_meta("A", 100000, ISO_META_READY, 5);
    iso_meta_t b = mk_meta("B", 100000, ISO_META_INCOMPLETE, 6);
    put_slot("a1b2c3d4", 0, &a);
    put_slot("a1b2c3d4", 1, &b);
    iso_slots_t s;
    /* b (新しい方) の CRC を壊す -> a が採用される */
    TEST_ASSERT_TRUE(mock_fs_poke(&FS, "/iso/a1b2c3d4/meta.b", 100, 0x42));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_TRUE(s.valid[0]);
    TEST_ASSERT_FALSE(s.valid[1]);
    TEST_ASSERT_EQUAL_INT8(0, s.adopted);
    /* a も壊す -> どちらも無効 */
    TEST_ASSERT_TRUE(mock_fs_poke(&FS, "/iso/a1b2c3d4/meta.a", 0, 'X'));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_FALSE(s.valid[0]);
    TEST_ASSERT_FALSE(s.valid[1]);
    TEST_ASSERT_EQUAL_INT8(-1, s.adopted);
}

void test_slots_read_slot_that_is_a_directory_is_invalid(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/a1b2c3d4"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/a1b2c3d4/meta.a"));
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_FALSE(s.valid[0]);
    TEST_ASSERT_EQUAL_INT8(-1, s.adopted);
}

void test_slots_read_rejects_bad_id_and_args(void) {
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_slots_read(&IFS, "A1B2C3D4", &s));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_slots_read(&IFS, "../iso/x", &s));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_slots_read(&IFS, NULL, &s));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_slots_read(&IFS, "a1b2c3d4", NULL));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_slots_read(NULL, "a1b2c3d4", &s));
}

void test_slots_read_io_error_is_not_treated_as_invalid(void) {
    iso_meta_t m = mk_meta("Alpha", 100000, ISO_META_READY, 5);
    put_slot("a1b2c3d4", 0, &m);
    put_slot("a1b2c3d4", 1, &m);
    iso_slots_t s;
    mock_fs_fail_path(&FS, "/iso/a1b2c3d4/meta.a");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_slots_read(&IFS, "a1b2c3d4", &s));
    mock_fs_fail_path(&FS, "/iso/a1b2c3d4/meta.b");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_slots_read(&IFS, "a1b2c3d4", &s));
    mock_fs_fail_path(&FS, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_EQUAL_INT8(0, s.adopted);
}

void test_slots_read_open_and_read_errors_are_not_treated_as_invalid(void) {
    iso_meta_t m = mk_meta("Alpha", 100000, ISO_META_READY, 5);
    put_slot("a1b2c3d4", 0, &m);
    iso_slots_t s;
    mock_fs_fail_open(&FS, "/iso/a1b2c3d4/meta.a");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_slots_read(&IFS, "a1b2c3d4", &s));
    mock_fs_fail_open(&FS, NULL);
    mock_fs_fail_read(&FS, "/iso/a1b2c3d4/meta.a", 0);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_EQUAL_INT(0, FS.open_files); /* 読み出しが失敗しても、閉じる */
    mock_fs_fail_read(&FS, NULL, 0);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_EQUAL_INT8(0, s.adopted);
}

void test_slots_read_directory_with_the_size_of_a_slot_is_invalid(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/a1b2c3d4"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/a1b2c3d4/meta.a"));
    TEST_ASSERT_TRUE(mock_fs_set_size(&FS, "/iso/a1b2c3d4/meta.a", 256)); /* 大きさが 256 のディレクトリ(大きさだけでは区別できない) */
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_FALSE(s.valid[0]);
}

void test_slots_read_leaves_no_open_handles(void) {
    iso_meta_t m = mk_meta("Alpha", 100000, ISO_META_READY, 5);
    put_slot("a1b2c3d4", 0, &m);
    put_slot("a1b2c3d4", 1, &m);
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "a1b2c3d4", &s));
    TEST_ASSERT_EQUAL_INT(0, FS.open_files); /* tearDown でも見る */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_slot_choose_table);
    RUN_TEST(test_slots_read_directory_without_meta);
    RUN_TEST(test_slots_read_only_a_and_only_b);
    RUN_TEST(test_slots_read_picks_newer_generation);
    RUN_TEST(test_slots_read_generation_wraparound);
    RUN_TEST(test_slots_read_slot_length_boundaries);
    RUN_TEST(test_slots_read_corrupt_slot_falls_back_to_the_other);
    RUN_TEST(test_slots_read_slot_that_is_a_directory_is_invalid);
    RUN_TEST(test_slots_read_rejects_bad_id_and_args);
    RUN_TEST(test_slots_read_io_error_is_not_treated_as_invalid);
    RUN_TEST(test_slots_read_open_and_read_errors_are_not_treated_as_invalid);
    RUN_TEST(test_slots_read_directory_with_the_size_of_a_slot_is_invalid);
    RUN_TEST(test_slots_read_leaves_no_open_handles);
    return UNITY_END();
}

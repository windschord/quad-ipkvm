/*
 * iso_catalog: begin(作成の順序)、<id> の生成、イベントの適用(meta の書き込みと世代)。期待値は手書き。
 */
#include "ic_common.h"

void setUp(void) { fx_setup(); }
void tearDown(void) { fx_teardown(); }

#define MIB 1048576ull

static iso_meta_t read_slot_meta(const char *id, int slot) {
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, id, &s));
    TEST_ASSERT_TRUE_MESSAGE(s.valid[slot], "slot must be valid");
    return s.meta[slot];
}

/* ---- begin ---- */

void test_begin_creates_directory_then_meta_a(void) {
    fx_settle();
    char id[9] = "";
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "Ubuntu 24.04", 12, 6126805504ull, NULL, id));
    TEST_ASSERT_EQUAL_STRING("11110001", id);
    /* 書き込みの順序: mkdir -> meta.a だけ(meta.b は最初の更新で作られる) */
    TEST_ASSERT_EQUAL_INT(2, FS.nlog);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "mkdir /iso/11110001"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "overwrite /iso/11110001/meta.a len=256"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/11110001/meta.b"));
    TEST_ASSERT_EQUAL_INT64(256, mock_fs_size(&FS, "/iso/11110001/meta.a"));
    /* 内容 */
    iso_meta_t m = read_slot_meta("11110001", 0);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_UPLOADING, m.state);
    TEST_ASSERT_EQUAL_UINT8(0, m.invalid_reason);
    TEST_ASSERT_EQUAL_UINT64(6126805504ull, m.size_bytes);
    TEST_ASSERT_EQUAL_UINT16(6, m.part_count); /* 5.7 GiB -> 6 パーツ */
    TEST_ASSERT_EQUAL_UINT32(524288, m.part_blocks);
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_NONE, m.sha_flag);
    TEST_ASSERT_EQUAL_UINT32(1, m.generation);
    TEST_ASSERT_EQUAL_UINT8(12, m.name_len);
    TEST_ASSERT_EQUAL_STRING("Ubuntu 24.04", m.name);
    for (int i = 0; i < 32; i++) TEST_ASSERT_EQUAL_HEX8(0, m.sha256[i]);
    /* カタログ */
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, "11110001");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, e->state);
    TEST_ASSERT_TRUE(e->meta_valid);
    TEST_ASSERT_EQUAL_INT8(0, e->adopted);
    TEST_ASSERT_EQUAL_UINT64(0, e->received);
    TEST_ASSERT_FALSE(e->resume_delete);
    TEST_ASSERT_EQUAL_UINT32(1, e->meta.generation);
    TEST_ASSERT_EQUAL_STRING("Ubuntu 24.04", e->meta.name);
}

void test_begin_with_declared_sha256(void) {
    uint8_t sha[32];
    for (int i = 0; i < 32; i++) sha[i] = (uint8_t)(0xF0 + (i & 7));
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "x", 1, 40000, sha, id));
    iso_meta_t m = read_slot_meta(id, 0);
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_DECLARED, m.sha_flag);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sha, m.sha256, 32);
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_DECLARED, CAT.images[0].meta.sha_flag);
}

void test_begin_part_count_for_sizes(void) {
    /* 34816 -> 1 パーツ、1 GiB ちょうど -> 1、1 GiB + 1 -> 2、16 GiB -> 16 */
    const struct {
        uint64_t size;
        uint16_t parts;
    } cs[] = {{34816, 1}, {GIB, 1}, {GIB + 1, 2}, {2 * GIB, 2}, {16 * GIB, 16}};
    for (size_t i = 0; i < sizeof(cs) / sizeof(cs[0]); i++) {
        char id[9];
        char name[2] = {(char)('a' + i), 0};
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, name, 1, cs[i].size, NULL, id));
        iso_meta_t m = read_slot_meta(id, 0);
        TEST_ASSERT_EQUAL_UINT16(cs[i].parts, m.part_count);
        TEST_ASSERT_EQUAL_UINT64(cs[i].size, m.size_bytes);
    }
}

void test_begin_validates_before_touching_the_card(void) {
    char id[9] = "keep";
    fx_settle();
    char big[80];
    memset(big, 'k', sizeof(big));
    const struct {
        const char *name;
        size_t len;
        uint64_t size;
        const char *why;
    } bad[] = {
        {"", 0, 100000, "empty name"},
        {big, 64, 100000, "64-byte name"},
        {"a/b", 3, 100000, "slash"},
        {" a", 2, 100000, "leading space"},
        {"a\x01", 2, 100000, "control char"},
        {"\xC0\xAF", 2, 100000, "bad UTF-8"},
        {"ok", 2, 0, "size 0"},
        {"ok", 2, 34815, "size below 34816"},
        {"ok", 2, 16 * GIB + 1, "size above 16 GiB"},
        {"ok", 2, UINT64_MAX, "size max"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_ERR_INVALID, iso_catalog_begin(&CAT, bad[i].name, bad[i].len, bad[i].size, NULL, id), bad[i].why);
    }
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);       /* 何も書いていない */
    TEST_ASSERT_EQUAL_INT(0, RND.calls);     /* 乱数も引いていない */
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_EQUAL_STRING("keep", id);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_begin(&CAT, NULL, 2, 100000, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_begin(&CAT, "ok", 2, 100000, NULL, NULL));
    /* 境界は通る */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "ok", 2, 34816, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, big, 63, 16 * GIB, NULL, id));
}

void test_begin_same_name_is_rejected_by_exact_bytes(void) {
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "Win11", 5, 100000, NULL, id));
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_NAME_EXISTS, iso_catalog_begin(&CAT, "Win11", 5, 200000, NULL, id));
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    TEST_ASSERT_EQUAL_INT(1, RND.calls);
    /* 大文字小文字、末尾の 1 文字違いは別の名前 */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "win11", 5, 100000, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "Win1", 4, 100000, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "Win112", 6, 100000, NULL, id));
    /* 正規化はしない */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "\xC3\xA9", 2, 100000, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "e\xCC\x81", 3, 100000, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_NAME_EXISTS, iso_catalog_begin(&CAT, "e\xCC\x81", 3, 100000, NULL, id));
}

void test_name_of_deleting_image_does_not_block_and_corrupt_has_no_name(void) {
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "Gone", 4, 100000, NULL, id));
    TEST_ASSERT_TRUE(iso_catalog_name_exists(&CAT, "Gone", 4));
    CAT.images[0].state = ISO_STATE_DELETING; /* API からは見えない */
    TEST_ASSERT_FALSE(iso_catalog_name_exists(&CAT, "Gone", 4));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "Gone", 4, 100000, NULL, id)); /* 同じ名前で作れる */
    TEST_ASSERT_TRUE(iso_catalog_name_exists(&CAT, "Gone", 4));
    CAT.images[0].state = ISO_STATE_READY; /* 削除中でなければ、2 つ目とも同名 */
    TEST_ASSERT_TRUE(iso_catalog_name_exists(&CAT, "Gone", 4));
    /* meta_corrupt のエントリは名前を持たない */
    CAT.images[0].meta_valid = false;
    CAT.images[1].meta_valid = false;
    TEST_ASSERT_FALSE(iso_catalog_name_exists(&CAT, "Gone", 4));
}

void test_begin_image_limit_and_precedence(void) {
    char id[9];
    for (int i = 0; i < 15; i++) {
        char name[3] = {'n', (char)('a' + i), 0};
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, name, 2, 100000, NULL, id));
    }
    TEST_ASSERT_EQUAL_UINT(15, CAT.count);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "last", 4, 100000, NULL, id)); /* 16 個目は OK */
    TEST_ASSERT_EQUAL_UINT(16, CAT.count);
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_TOO_MANY, iso_catalog_begin(&CAT, "17th", 4, 100000, NULL, id));
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    /* 同名と上限が同時: name_exists が先(WEB_API 8 の並び) */
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_NAME_EXISTS, iso_catalog_begin(&CAT, "last", 4, 100000, NULL, id));
    /* 検査(INVALID)は、どちらよりも先 */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_begin(&CAT, "last", 4, 10, NULL, id));
}

void test_begin_retries_the_id_on_collision(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/0000000a"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/0000000b"));
    const uint32_t seq[] = {0x0a, 0x0b, 0x0a, 0x0c};
    mock_rnd_set(&RND, seq, 4);
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "x", 1, 100000, NULL, id));
    TEST_ASSERT_EQUAL_STRING("0000000c", id); /* 8 桁にゼロ埋め */
    TEST_ASSERT_EQUAL_INT(4, RND.calls);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/0000000c/meta.a"));
}

void test_id_generate_formats_lowercase_hex_with_padding(void) {
    char id[9];
    const uint32_t seq1[] = {0xDEADBEEF};
    mock_rnd_set(&RND, seq1, 1);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_STRING("deadbeef", id);
    TEST_ASSERT_TRUE(iso_id_valid(id));
    const uint32_t seq2[] = {0x00000abc};
    mock_rnd_set(&RND, seq2, 1);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_STRING("00000abc", id);
    const uint32_t seq3[] = {1};
    mock_rnd_set(&RND, seq3, 1);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_STRING("00000001", id); /* 最小の id(00000000 は予約。下のテスト) */
    const uint32_t seq4[] = {0xFFFFFFFFu};
    mock_rnd_set(&RND, seq4, 1);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_STRING("ffffffff", id);
    TEST_ASSERT_TRUE(mock_fs_is_dir(&FS, "/iso/ffffffff"));
}

void test_id_generate_gives_up_after_16_tries(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/0000000a"));
    uint32_t seq[40];
    char id[9];
    /* 15 回衝突して 16 回目で成功 */
    for (int i = 0; i < 15; i++) seq[i] = 0x0a;
    seq[15] = 0x0b;
    mock_rnd_set(&RND, seq, 16);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_STRING("0000000b", id);
    TEST_ASSERT_EQUAL_INT(16, RND.calls);
    /* 16 回衝突したら、あきらめる(17 回目は引かない) */
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/0000000b"));
    for (int i = 0; i < 16; i++) seq[i] = 0x0a;
    seq[16] = 0x0b;
    mock_rnd_set(&RND, seq, 17);
    id[0] = 'x';
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_ID_EXHAUSTED, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_INT(16, RND.calls);
    TEST_ASSERT_EQUAL_CHAR('\0', id[0]);
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/0000000b"));
}

/* ---- 予約 ID 00000000(C-14。#160 の worker は media_id == 0 を「ない」として扱うので、生成しない) ---- */

void test_id_generate_redraws_when_the_random_value_is_zero(void) {
    const uint32_t seq[] = {0, 0, 0x1};
    mock_rnd_set(&RND, seq, 3);
    char id[9];
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_STRING("00000001", id);
    TEST_ASSERT_EQUAL_INT(3, RND.calls);
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000000")); /* 予約の id のディレクトリは作らない */
    TEST_ASSERT_TRUE(mock_fs_is_dir(&FS, "/iso/00000001"));
    TEST_ASSERT_EQUAL_INT(1, FS.nlog); /* mkdir は、採用した id の 1 回だけ */
}

void test_id_generate_zero_draws_count_as_tries_and_never_create_the_reserved_directory(void) {
    char id[9];
    uint32_t seq[20];
    /* 15 回 0 を引いても、16 回目の値で成功する */
    for (int i = 0; i < 15; i++) seq[i] = 0;
    seq[15] = 0xabc;
    mock_rnd_set(&RND, seq, 16);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_STRING("00000abc", id);
    TEST_ASSERT_EQUAL_INT(16, RND.calls);
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/00000abc"));
    /* 16 回 0 を引いたら、あきらめる(17 回目は引かない。ずっと 0 を返す乱数でも、止まる) */
    for (int i = 0; i < 16; i++) seq[i] = 0;
    seq[16] = 0xabc;
    mock_rnd_set(&RND, seq, 17);
    id[0] = 'x';
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_ID_EXHAUSTED, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_INT(16, RND.calls);
    TEST_ASSERT_EQUAL_CHAR('\0', id[0]);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog); /* mkdir を 1 回もしない */
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000000"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000abc"));
}

void test_id_generate_zero_draws_and_collisions_share_the_same_16_tries(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/0000000a"));
    uint32_t seq[20];
    char id[9];
    for (int i = 0; i < 16; i++) seq[i] = (i % 2) ? 0u : 0x0au; /* 衝突と 0 が交互。16 回で、あきらめる */
    seq[16] = 0xb;
    mock_rnd_set(&RND, seq, 17);
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_ID_EXHAUSTED, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_INT(16, RND.calls);
    seq[15] = 0xb; /* 16 回目が通れば成功 */
    mock_rnd_set(&RND, seq, 16);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_STRING("0000000b", id);
}

void test_id_generate_accepts_ids_next_to_the_reserved_one(void) {
    char id[9];
    const uint32_t a[] = {0x10000000u};
    mock_rnd_set(&RND, a, 1);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_STRING("10000000", id); /* 先頭の桁だけ 0 でない */
    const uint32_t b[] = {0x00000010u};
    mock_rnd_set(&RND, b, 1);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_STRING("00000010", id);
    const uint32_t c[] = {0x80000000u};
    mock_rnd_set(&RND, c, 1);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_STRING("80000000", id);
}

void test_begin_never_uses_the_reserved_id(void) {
    const uint32_t seq[] = {0, 0x5};
    mock_rnd_set(&RND, seq, 2);
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "x", 1, 100000, NULL, id));
    TEST_ASSERT_EQUAL_STRING("00000005", id);
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000000"));
    TEST_ASSERT_NULL(iso_catalog_find(&CAT, "00000000"));
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/00000005/meta.a"));
    /* 乱数が 0 しか返さないなら、begin は id を使い果たして失敗し、何も作らない */
    const uint32_t z[] = {0};
    mock_rnd_set(&RND, z, 1);
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_ID_EXHAUSTED, iso_catalog_begin(&CAT, "y", 1, 100000, NULL, id));
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
}

void test_begin_returns_id_exhausted_and_changes_nothing(void) {
    const uint32_t seq[] = {0x5};
    mock_rnd_set(&RND, seq, 1); /* いつも同じ値 */
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "a", 1, 100000, NULL, id));
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_ID_EXHAUSTED, iso_catalog_begin(&CAT, "b", 1, 100000, NULL, id));
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
    TEST_ASSERT_EQUAL_INT(16, FS.nlog); /* mkdir を 16 回試して、meta は書かない */
}

void test_begin_mkdir_error_other_than_exists_is_returned_without_retry(void) {
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_NO_SPACE);
    char id[9] = "keep";
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_SPACE, iso_catalog_begin(&CAT, "a", 1, 100000, NULL, id));
    TEST_ASSERT_EQUAL_INT(1, RND.calls);
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_EQUAL_INT(1, FS.nlog);
}

void test_id_generate_clears_the_output_on_a_mkdir_error(void) {
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_NO_SPACE);
    char id[9] = "keepkeep";
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_SPACE, iso_id_generate(&IFS, &IRND, id));
    TEST_ASSERT_EQUAL_CHAR('\0', id[0]);
}

void test_begin_meta_write_failure_cleans_up_the_directory(void) {
    fx_settle();
    mock_fs_error_at(&FS, 2, ISO_ERR_NO_SPACE); /* 2 回目の書き込み = meta.a */
    char id[9] = "keep";
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_SPACE, iso_catalog_begin(&CAT, "a", 1, 100000, NULL, id));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/11110001"));
    /* 記録: mkdir、overwrite(失敗)、片づけ(meta.a の remove、ディレクトリの remove) */
    TEST_ASSERT_EQUAL_INT(4, FS.nlog);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "remove /iso/11110001/meta.a"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "remove /iso/11110001"));
    /* 次の begin は成功する(乱数は進んでいるので別の id) */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "a", 1, 100000, NULL, id));
    TEST_ASSERT_EQUAL_STRING("11110002", id); /* 乱数列の 2 番目の値(1 番目は、失敗した begin が使った) */
}

/* ---- イベントの適用 ---- */

static iso_catalog_entry_t *new_image(const char *name, uint64_t size) {
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, name, strlen(name), size, NULL, id));
    return iso_catalog_find(&CAT, id);
}

static const iso_trans_ctx_t CTX_FREE = {false, true, false};

void test_apply_idle_timeout_writes_incomplete_to_the_other_slot(void) {
    iso_catalog_entry_t *e = new_image("A", 100000);
    char id[9];
    memcpy(id, e->id, 9);
    fx_settle();
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_IDLE_TIMEOUT, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, e->state);
    TEST_ASSERT_EQUAL_INT8(1, e->adopted);
    TEST_ASSERT_EQUAL_UINT32(2, e->meta.generation);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_INCOMPLETE, e->meta.state);
    TEST_ASSERT_EQUAL_INT(1, FS.nlog);
    char expect[80];
    snprintf(expect, sizeof(expect), "overwrite /iso/%s/meta.b len=256", id);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, expect));
    /* ディスク上の内容 */
    iso_meta_t b = read_slot_meta(id, 1);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_INCOMPLETE, b.state);
    TEST_ASSERT_EQUAL_UINT32(2, b.generation);
    iso_meta_t a = read_slot_meta(id, 0);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_UPLOADING, a.state); /* 古い方は変わらない */
    TEST_ASSERT_EQUAL_UINT32(1, a.generation);
}

void test_apply_put_resume_writes_nothing(void) {
    iso_catalog_entry_t *e = new_image("A", 100000);
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_IDLE_TIMEOUT, NULL, NULL));
    fx_settle();
    /* 枠が使われている間は再開できない */
    iso_trans_ctx_t busy = {false, false, false};
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_UPLOAD_IN_PROGRESS, iso_catalog_apply(&CAT, e, ISO_EV_PUT, &busy, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, e->state);
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_PUT, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, e->state);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog); /* meta は変えない(再起動すれば incomplete に戻る) */
    TEST_ASSERT_EQUAL_UINT32(2, e->meta.generation);
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_PUT, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
}

void test_apply_generations_alternate_slots(void) {
    iso_catalog_entry_t *e = new_image("A", 100000);
    char id[9];
    memcpy(id, e->id, 9);
    /* uploading -> incomplete -> (resume) uploading -> incomplete -> ... */
    const int expect_slot[] = {1, 0, 1, 0};
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_IDLE_TIMEOUT, NULL, NULL));
        TEST_ASSERT_EQUAL_INT8(expect_slot[i], e->adopted);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(i + 2), e->meta.generation);
        TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_PUT, &CTX_FREE, NULL));
    }
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, id, &s));
    TEST_ASSERT_EQUAL_INT8(0, s.adopted);
    TEST_ASSERT_EQUAL_UINT32(5, s.meta[0].generation);
    TEST_ASSERT_EQUAL_UINT32(4, s.meta[1].generation);
}

void test_apply_finish_and_finalize_ok(void) {
    iso_catalog_entry_t *e = new_image("A", 100000);
    char id[9];
    memcpy(id, e->id, 9);
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_IDLE_TIMEOUT, NULL, NULL));
    fx_settle();
    /* received < size */
    e->received = 99999;
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_SIZE_MISMATCH, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, e->state);
    /* received > size も一致ではない */
    e->received = 100001;
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_SIZE_MISMATCH, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    /* ちょうど。枠が使われていれば upload_in_progress */
    e->received = 100000;
    iso_trans_ctx_t busy = {false, false, false};
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_UPLOAD_IN_PROGRESS, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &busy, NULL));
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, e->state);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog); /* ready になるまで meta は変えない */
    TEST_ASSERT_EQUAL_UINT8(ISO_META_INCOMPLETE, e->meta.state);
    /* 冪等 */
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, e->state);
    /* 検査に成功: ready、SHA-256 は計算値 (フラグ 2) */
    uint8_t sha[32];
    for (int i = 0; i < 32; i++) sha[i] = (uint8_t)(0x30 + i);
    iso_apply_arg_t arg;
    memset(&arg, 0, sizeof(arg));
    arg.set_sha = true;
    arg.sha_flag = ISO_SHA_COMPUTED;
    arg.sha256 = sha;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_OK, NULL, &arg));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, e->state);
    TEST_ASSERT_EQUAL_INT(1, FS.nlog);
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, id, &s));
    TEST_ASSERT_EQUAL_INT8(e->adopted, s.adopted);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_READY, s.meta[s.adopted].state);
    TEST_ASSERT_EQUAL_UINT8(0, s.meta[s.adopted].invalid_reason);
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_COMPUTED, s.meta[s.adopted].sha_flag);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sha, s.meta[s.adopted].sha256, 32);
    TEST_ASSERT_EQUAL_UINT32(3, s.meta[s.adopted].generation);
}

void test_apply_finalize_ok_sha_variants(void) {
    /* import: 引数なし -> SHA-256 は変えない(フラグ 0 のまま) */
    iso_catalog_entry_t *e = new_image("A", 100000);
    e->received = 100000;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_OK, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_NONE, e->meta.sha_flag);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, e->state);
    /* 宣言値(フラグ 1)があった場合、引数なしならそのまま(フラグ 1 のまま) */
    uint8_t decl[32];
    memset(decl, 0x77, 32);
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "B", 1, 100000, decl, id));
    e = iso_catalog_find(&CAT, id);
    e->received = 100000;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_OK, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_DECLARED, e->meta.sha_flag);
    /* set_sha でフラグ 0 -> 値を消す */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "C", 1, 100000, decl, id));
    e = iso_catalog_find(&CAT, id);
    e->received = 100000;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    iso_apply_arg_t arg;
    memset(&arg, 0, sizeof(arg));
    arg.set_sha = true;
    arg.sha_flag = ISO_SHA_NONE;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_OK, NULL, &arg));
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_NONE, e->meta.sha_flag);
    TEST_ASSERT_EQUAL_HEX8(0, e->meta.sha256[0]);
    /* 不正な引数: フラグ 2 で値なし、フラグ 1 は不可 -> 何も変えない */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "D", 1, 100000, NULL, id));
    e = iso_catalog_find(&CAT, id);
    e->received = 100000;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    fx_settle();
    arg.sha_flag = ISO_SHA_COMPUTED;
    arg.sha256 = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_OK, NULL, &arg));
    arg.sha_flag = ISO_SHA_DECLARED;
    uint8_t sha[32] = {0};
    arg.sha256 = sha;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_OK, NULL, &arg));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, e->state);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
}

void test_apply_sha_update_only_for_finalize_ok_with_set_sha(void) {
    uint8_t decl[32];
    memset(decl, 0x77, 32);
    uint8_t other[32];
    memset(other, 0x11, 32);
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, "A", 1, 100000, decl, id));
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, id);
    iso_apply_arg_t arg;
    memset(&arg, 0, sizeof(arg));
    arg.set_sha = true;
    arg.sha_flag = ISO_SHA_COMPUTED;
    arg.sha256 = other;
    /* idle timeout は書くが、SHA-256 は変えない(引数があっても) */
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_IDLE_TIMEOUT, NULL, &arg));
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_DECLARED, e->meta.sha_flag);
    TEST_ASSERT_EQUAL_HEX8(0x77, e->meta.sha256[0]);
    e->received = 100000;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    /* finalize ok: set_sha が false なら、引数があっても SHA-256 は変えない */
    iso_apply_arg_t none;
    memset(&none, 0, sizeof(none));
    none.sha_flag = ISO_SHA_NONE;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_OK, NULL, &none));
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_DECLARED, e->meta.sha_flag);
    TEST_ASSERT_EQUAL_HEX8(0x77, e->meta.sha256[31]);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, e->state);
}

void test_apply_invalid_requires_a_valid_reason(void) {
    iso_catalog_entry_t *e = new_image("A", 100000);
    e->received = 100000;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    fx_settle();
    iso_apply_arg_t arg;
    memset(&arg, 0, sizeof(arg));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_FAIL, NULL, NULL));
    arg.invalid_reason = 0;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_FAIL, NULL, &arg));
    arg.invalid_reason = 5; /* meta_corrupt は保存しない */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_FAIL, NULL, &arg));
    arg.invalid_reason = 9;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_FAIL, NULL, &arg));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, e->state);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
}

void test_apply_each_invalid_reason_is_persisted(void) {
    for (uint8_t r = 1; r <= 4; r++) {
        char id[9];
        char name[3] = {'z', (char)('0' + r), 0};
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_begin(&CAT, name, 2, 100000, NULL, id));
        iso_catalog_entry_t *e = iso_catalog_find(&CAT, id);
        e->received = 100000;
        TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
        iso_apply_arg_t arg;
        memset(&arg, 0, sizeof(arg));
        arg.invalid_reason = r;
        TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_FAIL, NULL, &arg));
        iso_slots_t s;
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, id, &s));
        TEST_ASSERT_EQUAL_UINT8(ISO_META_INVALID, s.meta[s.adopted].state);
        TEST_ASSERT_EQUAL_UINT8(r, s.meta[s.adopted].invalid_reason);
        TEST_ASSERT_EQUAL_UINT8(r, e->invalid_reason);
    }
}

void test_apply_io_error_and_write_fail_go_to_incomplete(void) {
    iso_catalog_entry_t *e = new_image("A", 100000);
    e->received = 100000;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_IOERR, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, e->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_INCOMPLETE, e->meta.state);
    TEST_ASSERT_EQUAL_UINT8(0, e->invalid_reason);
    /* 再試行は finish */
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, e->state);
    /* write_fail は uploading から */
    iso_catalog_entry_t *f = new_image("B", 100000);
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, f, ISO_EV_WRITE_FAIL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, f->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_INCOMPLETE, f->meta.state);
}

void test_apply_index_events_on_ready(void) {
    iso_catalog_entry_t *e = new_image("A", 100000);
    e->received = 100000;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_OK, NULL, NULL));
    fx_settle();
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_INDEX_OK, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_INDEX_IOERR, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, e->state);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog); /* meta は書かない */
    iso_apply_arg_t arg;
    memset(&arg, 0, sizeof(arg));
    arg.invalid_reason = ISO_INVALID_PART_MISMATCH;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_INDEX_FAIL, NULL, &arg));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, e->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_PART_MISMATCH, e->invalid_reason);
    TEST_ASSERT_EQUAL_INT(1, FS.nlog);
    /* invalid は削除だけ */
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_INVALID, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_STATE, iso_catalog_apply(&CAT, e, ISO_EV_PUT, &CTX_FREE, NULL));
}

void test_apply_delete_clears_reason_and_resume_mark(void) {
    iso_meta_t inv = mk_meta("I", 100000, ISO_META_INVALID, 3);
    inv.invalid_reason = ISO_INVALID_PART_MISMATCH;
    put_slot("00000001", 0, &inv);
    iso_meta_t del = mk_meta("D", 100000, ISO_META_DELETING, 3);
    put_slot("00000002", 0, &del);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_catalog_entry_t *a = iso_catalog_find(&CAT, "00000001");
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_PART_MISMATCH, a->invalid_reason);
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, a, ISO_EV_DELETE, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, a->state);
    TEST_ASSERT_EQUAL_UINT8(0, a->invalid_reason); /* deleting の理由は 0 */
    iso_catalog_entry_t *b = iso_catalog_find(&CAT, "00000002");
    TEST_ASSERT_TRUE(b->resume_delete); /* スキャンが付けた「続ける」印 */
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, b, ISO_EV_DELETE, &CTX_FREE, NULL)); /* ジョブを投入したら、印を下ろす */
    TEST_ASSERT_FALSE(b->resume_delete);
}

void test_apply_delete_and_delete_done(void) {
    iso_catalog_entry_t *e = new_image("A", 100000);
    char id[9];
    memcpy(id, e->id, 9);
    e->received = 100000;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_FINALIZE_OK, NULL, NULL));
    fx_settle();
    iso_trans_ctx_t mounted = {false, true, true};
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_MOUNTED, iso_catalog_apply(&CAT, e, ISO_EV_DELETE, &mounted, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, e->state);
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_DELETE, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, e->state);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog); /* meta の deleting は、削除のステップの最初が書く */
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_DELETE, &CTX_FREE, NULL)); /* 冪等 */
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_DELETE_DONE, NULL, NULL));
    TEST_ASSERT_NULL(iso_catalog_find(&CAT, id)); /* カタログから消えた */
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
}

void test_apply_meta_write_failure_changes_nothing_and_retry_uses_the_same_slot(void) {
    iso_catalog_entry_t *e = new_image("A", 100000);
    char id[9];
    memcpy(id, e->id, 9);
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_IO);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_apply(&CAT, e, ISO_EV_IDLE_TIMEOUT, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, e->state);
    TEST_ASSERT_EQUAL_INT8(0, e->adopted);
    TEST_ASSERT_EQUAL_UINT32(1, e->meta.generation);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_UPLOADING, e->meta.state);
    /* 再試行 */
    fx_settle();
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_IDLE_TIMEOUT, NULL, NULL));
    TEST_ASSERT_EQUAL_INT8(1, e->adopted);
    TEST_ASSERT_EQUAL_UINT32(2, e->meta.generation);
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, id, &s));
    TEST_ASSERT_EQUAL_INT8(1, s.adopted);
}

void test_apply_generation_wraps_from_uint32_max_to_zero(void) {
    iso_meta_t m = mk_meta("W", 100000, ISO_META_UPLOADING, 0xFFFFFFFFu);
    put_slot("00000001", 0, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, "00000001");
    TEST_ASSERT_NOT_NULL(e);
    /* スキャンでは incomplete。resume してから idle へ */
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_PUT, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_IDLE_TIMEOUT, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, e->meta.generation);
    TEST_ASSERT_EQUAL_INT8(1, e->adopted);
    /* 再スキャンで、世代 0 の b が新しいものとして採用される */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    e = iso_catalog_find(&CAT, "00000001");
    TEST_ASSERT_EQUAL_INT8(1, e->adopted);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_INCOMPLETE, e->meta.state);
    /* さらに 1 回: a に世代 1 */
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_PUT, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_IDLE_TIMEOUT, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, e->meta.generation);
    TEST_ASSERT_EQUAL_INT8(0, e->adopted);
}

void test_write_meta_rejects_an_invalid_meta_without_writing(void) {
    iso_catalog_entry_t *e = new_image("A", 100000);
    fx_settle();
    iso_meta_t bad = e->meta;
    bad.state = 9; /* 範囲外 */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_write_meta(&CAT, e, &bad));
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    TEST_ASSERT_EQUAL_INT8(0, e->adopted);
    TEST_ASSERT_EQUAL_UINT32(1, e->meta.generation);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_UPLOADING, e->meta.state);
}

void test_apply_on_corrupt_entry(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000001"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, "00000001");
    TEST_ASSERT_FALSE(e->meta_valid);
    /* 削除以外は受け付けない */
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_INVALID, iso_catalog_apply(&CAT, e, ISO_EV_FINISH, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_STATE, iso_catalog_apply(&CAT, e, ISO_EV_PUT, &CTX_FREE, NULL));
    iso_meta_t m = mk_meta("X", 100000, ISO_META_READY, 1);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_write_meta(&CAT, e, &m)); /* 採用できるスロットがない */
    fx_settle();
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_DELETE, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, e->state);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
}

void test_apply_reboot_keeps_the_invalid_reason(void) {
    /* meta_corrupt (理由 5) と、保存された理由 (part_mismatch) は、REBOOT で消えない */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000001"));
    iso_meta_t inv = mk_meta("I", 100000, ISO_META_INVALID, 3);
    inv.invalid_reason = ISO_INVALID_PART_MISMATCH;
    put_slot("00000002", 0, &inv);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_catalog_entry_t *c = iso_catalog_find(&CAT, "00000001");
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_META_CORRUPT, c->invalid_reason);
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, c, ISO_EV_REBOOT, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, c->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_META_CORRUPT, c->invalid_reason);
    iso_catalog_entry_t *v = iso_catalog_find(&CAT, "00000002");
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, v, ISO_EV_REBOOT, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_PART_MISMATCH, v->invalid_reason);
    /* ready に REBOOT しても理由は 0 */
    iso_catalog_entry_t *r = new_image("R", 100000);
    r->received = 100000;
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, r, ISO_EV_FINISH, &CTX_FREE, NULL));
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, r, ISO_EV_FINALIZE_OK, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, r, ISO_EV_REBOOT, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(0, r->invalid_reason);
}

void test_apply_reboot_event_does_not_write(void) {
    iso_catalog_entry_t *e = new_image("A", 100000);
    fx_settle();
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_REBOOT, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, e->state);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_STATE, iso_catalog_apply(&CAT, e, ISO_EV_BEGIN, NULL, NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_begin_creates_directory_then_meta_a);
    RUN_TEST(test_begin_with_declared_sha256);
    RUN_TEST(test_begin_part_count_for_sizes);
    RUN_TEST(test_begin_validates_before_touching_the_card);
    RUN_TEST(test_begin_same_name_is_rejected_by_exact_bytes);
    RUN_TEST(test_name_of_deleting_image_does_not_block_and_corrupt_has_no_name);
    RUN_TEST(test_begin_image_limit_and_precedence);
    RUN_TEST(test_begin_retries_the_id_on_collision);
    RUN_TEST(test_id_generate_formats_lowercase_hex_with_padding);
    RUN_TEST(test_id_generate_gives_up_after_16_tries);
    RUN_TEST(test_id_generate_redraws_when_the_random_value_is_zero);
    RUN_TEST(test_id_generate_zero_draws_count_as_tries_and_never_create_the_reserved_directory);
    RUN_TEST(test_id_generate_zero_draws_and_collisions_share_the_same_16_tries);
    RUN_TEST(test_id_generate_accepts_ids_next_to_the_reserved_one);
    RUN_TEST(test_begin_never_uses_the_reserved_id);
    RUN_TEST(test_begin_returns_id_exhausted_and_changes_nothing);
    RUN_TEST(test_begin_mkdir_error_other_than_exists_is_returned_without_retry);
    RUN_TEST(test_id_generate_clears_the_output_on_a_mkdir_error);
    RUN_TEST(test_begin_meta_write_failure_cleans_up_the_directory);
    RUN_TEST(test_apply_idle_timeout_writes_incomplete_to_the_other_slot);
    RUN_TEST(test_apply_put_resume_writes_nothing);
    RUN_TEST(test_apply_generations_alternate_slots);
    RUN_TEST(test_apply_finish_and_finalize_ok);
    RUN_TEST(test_apply_finalize_ok_sha_variants);
    RUN_TEST(test_apply_sha_update_only_for_finalize_ok_with_set_sha);
    RUN_TEST(test_apply_invalid_requires_a_valid_reason);
    RUN_TEST(test_apply_each_invalid_reason_is_persisted);
    RUN_TEST(test_apply_io_error_and_write_fail_go_to_incomplete);
    RUN_TEST(test_apply_index_events_on_ready);
    RUN_TEST(test_apply_delete_clears_reason_and_resume_mark);
    RUN_TEST(test_apply_delete_and_delete_done);
    RUN_TEST(test_apply_meta_write_failure_changes_nothing_and_retry_uses_the_same_slot);
    RUN_TEST(test_apply_generation_wraps_from_uint32_max_to_zero);
    RUN_TEST(test_write_meta_rejects_an_invalid_meta_without_writing);
    RUN_TEST(test_apply_on_corrupt_entry);
    RUN_TEST(test_apply_reboot_keeps_the_invalid_reason);
    RUN_TEST(test_apply_reboot_event_does_not_write);
    return UNITY_END();
}

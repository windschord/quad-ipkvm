/*
 * iso_upload_write_chunk(正常系と境界): offset == received のときだけ書く、チャンクの上限、パーツの境界での分割、sync の間隔、
 * SHA-256 の逐次、再開、アイドル、中止。期待値は手書き。パーツは PB_SMALL(17 ブロック = 34816 バイト)で小さくしている。
 * 書き込みの失敗・電源断は test_up_failures.c / test_up_powercut.c。
 */
#include "up_common.h"

void setUp(void) { fx_up_setup(0, 0); }
void tearDown(void) { fx_up_teardown(); }

static void reconf(uint32_t chunk, uint32_t sync) {
    fx_up_teardown();
    fx_up_setup(chunk, sync);
}

/* 1 GiB を書くテスト用: SHA-256 を、何もしない軽い実装に替える(UP は、この SHA の領域を指している) */
static void noop_update(void *st, const void *d, size_t n) {
    (void)st;
    (void)d;
    (void)n;
}
static void use_fake_sha(void) { SHA.update = noop_update; }

#define SZ 100000ull

static char ID[ISO_ID_LEN + 1];
static const char *P0 = "/iso/11110001/p000";
static const char *P1 = "/iso/11110001/p001";
static const char *P2 = "/iso/11110001/p002";

static void begin_small(uint64_t size) {
    img_begin_pb("img", size, PB_SMALL, NULL, ID);
    TEST_ASSERT_EQUAL_STRING("11110001", ID);
    fx_settle();
}

static iso_catalog_entry_t *entry(void) { return iso_catalog_find(&CAT, ID); }

/* ---- 基本 --------------------------------------------------------------------------------- */

void test_first_chunk_creates_p000_and_does_not_sync(void) {
    begin_small(SZ);
    int v = VIEW_CALLS;
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 0, 1000, &w));
    TEST_ASSERT_EQUAL_UINT64(1000, w.received);
    TEST_ASSERT_EQUAL_UINT64(1000, entry()->received);
    TEST_ASSERT_EQUAL_INT(2, FS.nlog);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "open_w /iso/11110001/p000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "write /iso/11110001/p000 len=1000"));
    TEST_ASSERT_EQUAL_INT(0, FS.n_sync);
    TEST_ASSERT_EQUAL_INT(1, FS.open_files);                 /* ハンドルは開いたまま */
    TEST_ASSERT_EQUAL_INT64(0, mock_fs_size(&FS, P0));        /* ディレクトリ上の大きさは、sync まで 0 */
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, entry()->state);
}

void test_sequential_chunks_append_to_the_open_file(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    TEST_ASSERT_EQUAL_INT(1, FS.n_open_w); /* 開くのは 1 回 */
    TEST_ASSERT_EQUAL_INT(3, FS.n_write);
    TEST_ASSERT_EQUAL_UINT64(3000, entry()->received);
    TEST_ASSERT_EQUAL_UINT64(3000, UP.consumed_since);
    TEST_ASSERT_EQUAL_UINT64(3000, UP.sha_bytes);
}

void test_time_of_the_last_put_is_recorded(void) {
    begin_small(SZ);
    NOW_MS = 777000;
    put_all(ID, 0, 10, 10);
    TEST_ASSERT_EQUAL_UINT64(777000, UP.last_put_ms);
}

/* ---- offset ------------------------------------------------------------------------------- */

void test_offset_mismatch_writes_nothing_and_reports_received(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    fx_settle();
    static const uint64_t bad[] = {0, 1, 2999, 3001, 4000, 100000};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        iso_upload_write_t w;
        uint8_t b[10] = {0};
        TEST_ASSERT_EQUAL_INT(ISO_RES_OFFSET_MISMATCH, iso_upload_write_chunk(&UP, ID, bad[i], b, sizeof(b), &w));
        TEST_ASSERT_EQUAL_UINT64(3000, w.received);
    }
    TEST_ASSERT_EQUAL_INT(0, FS.nlog); /* 何も書かない */
    TEST_ASSERT_EQUAL_UINT64(3000, entry()->received);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, entry()->state);
    TEST_ASSERT_EQUAL_UINT64(3000, UP.sha_bytes);
}

void test_offset_mismatch_takes_priority_over_exceeds_size(void) {
    begin_small(34816);
    put_all(ID, 0, 1000, 1000);
    uint8_t b[40000] = {0};
    iso_upload_write_t w;
    /* offset が違い、かつ、size も超える: offset_mismatch(WEB_API の判定の 10 が 11 より先) */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OFFSET_MISMATCH, iso_upload_write_chunk(&UP, ID, 5, b, 34816, &w));
    TEST_ASSERT_EQUAL_UINT64(1000, w.received);
}

/* ---- チャンクの大きさ、size ---------------------------------------------------------------- */

void test_chunk_size_limit(void) {
    reconf(1000, 0);
    begin_small(SZ);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 0, 1000, &w)); /* ちょうど上限 */
    TEST_ASSERT_EQUAL_INT(ISO_RES_CHUNK_TOO_LARGE, put_range(ID, 1000, 1001, &w));
    TEST_ASSERT_EQUAL_UINT64(0, w.received);
    TEST_ASSERT_EQUAL_UINT64(1000, entry()->received);
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 1000, 999, &w));
    TEST_ASSERT_EQUAL_UINT64(1999, w.received);
    TEST_ASSERT_EQUAL_UINT32(1000, iso_upload_chunk_bytes(&UP));
}

void test_default_chunk_size_is_128_kib(void) {
    begin_small(500000);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_CHUNK_TOO_LARGE, put_range(ID, 0, 131073, &w));
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 0, 131072, &w));
    TEST_ASSERT_EQUAL_UINT64(131072, w.received);
}

void test_empty_and_null_chunks_are_invalid(void) {
    begin_small(SZ);
    iso_upload_write_t w;
    uint8_t b[4] = {0};
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_write_chunk(&UP, ID, 0, b, 0, &w));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_write_chunk(&UP, ID, 0, NULL, 4, &w));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_write_chunk(&UP, NULL, 0, b, 4, &w));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_write_chunk(NULL, ID, 0, b, 4, &w));
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
}

void test_chunk_must_not_pass_size(void) {
    reconf(40000, 0);
    begin_small(34816);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_EXCEEDS_SIZE, put_range(ID, 0, 34817, &w));
    TEST_ASSERT_EQUAL_UINT64(0, w.received);
    TEST_ASSERT_EQUAL_INT(0, FS.n_write);
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 0, 34816, &w)); /* ちょうど size まで */
    TEST_ASSERT_EQUAL_UINT64(34816, w.received);
    TEST_ASSERT_EQUAL_INT(ISO_RES_EXCEEDS_SIZE, put_range(ID, 34816, 1, &w)); /* size に達した後は、1 バイトでも超える */
    TEST_ASSERT_EQUAL_UINT64(34816, w.received);
    TEST_ASSERT_EQUAL_UINT64(34816, entry()->received);
}

void test_exceeds_size_by_one_byte_in_the_middle(void) {
    begin_small(34816);
    put_all(ID, 0, 34000, 1000);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_EXCEEDS_SIZE, put_range(ID, 34000, 817, &w)); /* 34000 + 817 = 34817 */
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 34000, 816, &w));
}

/* ---- 状態 --------------------------------------------------------------------------------- */

void test_state_checks(void) {
    begin_small(SZ);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_FOUND, iso_upload_write_chunk(&UP, "ffffffff", 0, "x", 1, &w));
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_FOUND, iso_upload_write_chunk(&UP, "../../x", 0, "x", 1, &w));
    iso_catalog_entry_t *e = entry();
    static const struct {
        iso_state_t st;
        int want;
    } T[] = {
        {ISO_STATE_READY, ISO_RES_NOT_UPLOADING}, {ISO_STATE_INVALID, ISO_RES_NOT_UPLOADING},
        {ISO_STATE_FINALIZING, ISO_RES_NOT_UPLOADING}, {ISO_STATE_DELETING, ISO_RES_NOT_FOUND},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        e->state = T[i].st;
        TEST_ASSERT_EQUAL_INT(T[i].want, put_range(ID, 0, 10, &w));
    }
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
}

void test_meta_corrupt_image_is_not_uploading(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/0000000f"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_UPLOADING, put_range("0000000f", 0, 10, &w));
}

/* ---- パーツの境界 ------------------------------------------------------------------------- */

void test_chunk_across_a_part_boundary_is_split(void) {
    reconf(40000, 0);
    begin_small(SZ);
    put_all(ID, 0, 30000, 30000);
    fx_settle();
    iso_upload_write_t w;
    /* [30000, 40000): p000 の残り 4816 バイト、p001 の 5184 バイト */
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 30000, 10000, &w));
    TEST_ASSERT_EQUAL_UINT64(40000, w.received);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "write /iso/11110001/p000 len=4816"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "sync /iso/11110001/p000"));  /* パーツの終わりで sync */
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "open_w /iso/11110001/p001")); /* 次のパーツは新規作成 */
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "write /iso/11110001/p001 len=5184"));
    TEST_ASSERT_EQUAL_INT(4, FS.nlog);
    TEST_ASSERT_EQUAL_INT64(34816, mock_fs_size(&FS, P0)); /* p000 は同期済み */
    TEST_ASSERT_EQUAL_INT64(0, mock_fs_size(&FS, P1));     /* p001 は未同期 */
    TEST_ASSERT_EQUAL_INT(1, FS.open_files);                /* p000 は閉じ、p001 が開いている */
    TEST_ASSERT_EQUAL_UINT64(40000, UP.sha_bytes);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    expect_disk_prefix(ID, PB_SMALL, 40000);
    TEST_ASSERT_EQUAL_INT64(5184, mock_fs_size(&FS, P1));
}

void test_chunk_ending_on_the_last_byte_of_a_part(void) {
    reconf(40000, 0);
    begin_small(SZ);
    put_all(ID, 0, 34000, 34000);
    fx_settle();
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 34000, 816, &w)); /* ちょうど 34816 */
    TEST_ASSERT_EQUAL_UINT64(34816, w.received);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "write /iso/11110001/p000 len=816"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "sync /iso/11110001/p000"));
    TEST_ASSERT_EQUAL_INT(2, FS.nlog);
    TEST_ASSERT_EQUAL_INT(0, FS.open_files); /* 閉じた。次のパーツはまだ作らない */
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, P1));
    TEST_ASSERT_EQUAL_INT64(34816, mock_fs_size(&FS, P0));
    /* 次のチャンクの 1 バイト目が p001 の先頭 */
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 34816, 1, &w));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "open_w /iso/11110001/p001"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "write /iso/11110001/p001 len=1"));
    TEST_ASSERT_EQUAL_UINT64(34817, w.received);
}

void test_chunk_one_byte_before_a_part_boundary_stays_in_the_part(void) {
    reconf(40000, 0);
    begin_small(SZ);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 0, 34815, &w));
    TEST_ASSERT_EQUAL_INT(0, FS.n_sync);
    TEST_ASSERT_EQUAL_INT(1, FS.open_files);
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 34815, 2, &w)); /* 1 バイト + 1 バイト */
    TEST_ASSERT_EQUAL_INT(1, FS.n_sync);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    TEST_ASSERT_EQUAL_INT64(34816, mock_fs_size(&FS, P0));
    TEST_ASSERT_EQUAL_INT64(1, mock_fs_size(&FS, P1));
}

void test_one_chunk_covering_three_parts(void) {
    reconf(100000, 0);
    begin_small(SZ);
    fx_settle();
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 0, SZ, &w));
    TEST_ASSERT_EQUAL_UINT64(SZ, w.received);
    const char *want[] = {"open_w /iso/11110001/p000", "write /iso/11110001/p000 len=34816", "sync /iso/11110001/p000",
                          "open_w /iso/11110001/p001", "write /iso/11110001/p001 len=34816", "sync /iso/11110001/p001",
                          "open_w /iso/11110001/p002", "write /iso/11110001/p002 len=30368"};
    TEST_ASSERT_EQUAL_INT(8, FS.nlog);
    for (int i = 0; i < 8; i++) TEST_ASSERT_TRUE(mock_fs_log_is(&FS, i, want[i]));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    expect_disk_prefix(ID, PB_SMALL, SZ);
    TEST_ASSERT_EQUAL_INT64(30368, mock_fs_size(&FS, P2));
}

void test_chunk_starting_in_the_middle_and_crossing_two_boundaries(void) {
    reconf(100000, 0);
    begin_small(SZ);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 0, 10, &w));
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 10, 80000, &w)); /* 10 .. 80010 */
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "write /iso/11110001/p000 len=34806"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "write /iso/11110001/p001 len=34816"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 6, "write /iso/11110001/p002 len=10378"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    expect_disk_prefix(ID, PB_SMALL, 80010);
}

void test_default_part_size_boundary_at_1_gib(void) {
    /* 既定のパーツ(1 GiB = 524288 ブロック)の境界。中身は持たない(大きさだけ)ので、SHA-256 は、偽の軽い実装に替える */
    reconf(0, 0);
    use_fake_sha();
    mock_fs_set_discard(&FS, true);
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "big", 3, 2 * GIB + 100, NULL, id));
    static uint8_t chunk[131072];
    iso_upload_write_t w;
    uint64_t off = 0;
    while (off + sizeof(chunk) <= GIB) {
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_write_chunk(&UP, id, off, chunk, sizeof(chunk), &w));
        off += sizeof(chunk);
    }
    TEST_ASSERT_EQUAL_UINT64(GIB, off);
    TEST_ASSERT_EQUAL_INT(0, FS.open_files); /* p000 は満杯で閉じた */
    TEST_ASSERT_EQUAL_INT64(GIB, mock_fs_size(&FS, "/iso/11110001/p000"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/11110001/p001"));
    /* パーツの最後の 1 バイトの手前: 1 GiB - 1 バイトのチャンクを別のイメージで確かめるのは重いので、境界をまたぐチャンクを送る */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_write_chunk(&UP, id, off, chunk, 1000, &w));
    TEST_ASSERT_EQUAL_UINT64(GIB + 1000, w.received);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/11110001/p001"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, id));
    TEST_ASSERT_EQUAL_INT64(1000, mock_fs_size(&FS, "/iso/11110001/p001"));
}

void test_default_part_size_boundary_chunk_straddles_1_gib(void) {
    reconf(0, 0);
    use_fake_sha();
    mock_fs_set_discard(&FS, true);
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "big", 3, 2 * GIB, NULL, id));
    static uint8_t chunk[131072];
    iso_upload_write_t w;
    uint64_t off = 0;
    while (off + sizeof(chunk) <= GIB - 1000) {
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_write_chunk(&UP, id, off, chunk, sizeof(chunk), &w));
        off += sizeof(chunk);
    }
    uint64_t rest = GIB - off; /* パーツの終わりまでの残り(131072 未満) */
    TEST_ASSERT_EQUAL_UINT64(sizeof(chunk), rest);
    /* パーツの最後の 1 バイトだけ手前まで */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_write_chunk(&UP, id, off, chunk, (size_t)rest - 1, &w));
    TEST_ASSERT_EQUAL_INT(1, FS.open_files);
    TEST_ASSERT_EQUAL_UINT64(GIB - 1, w.received);
    /* 最後の 1 バイトと、次のパーツの 99 バイト */
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_write_chunk(&UP, id, GIB - 1, chunk, 100, &w));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "write /iso/11110001/p000 len=1"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "sync /iso/11110001/p000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "open_w /iso/11110001/p001"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "write /iso/11110001/p001 len=99"));
    TEST_ASSERT_EQUAL_INT64(GIB, mock_fs_size(&FS, "/iso/11110001/p000"));
}

/* ---- sync の間隔 -------------------------------------------------------------------------- */

void test_sync_every_sync_bytes_boundary_is_inclusive(void) {
    reconf(0, 10000);
    begin_small(SZ);
    put_all(ID, 0, 5000, 5000);
    TEST_ASSERT_EQUAL_INT(0, FS.n_sync);
    put_all(ID, 5000, 10000, 5000); /* 合計ちょうど 10000: sync */
    TEST_ASSERT_EQUAL_INT(1, FS.n_sync);
    TEST_ASSERT_EQUAL_INT64(10000, mock_fs_size(&FS, P0));
    put_all(ID, 10000, 19999, 9999); /* 9999 バイト: まだ */
    TEST_ASSERT_EQUAL_INT(1, FS.n_sync);
    TEST_ASSERT_EQUAL_INT64(10000, mock_fs_size(&FS, P0));
    put_all(ID, 19999, 20000, 1); /* 10000 になる: sync */
    TEST_ASSERT_EQUAL_INT(2, FS.n_sync);
    TEST_ASSERT_EQUAL_INT64(20000, mock_fs_size(&FS, P0));
}

void test_sync_count_resets_after_each_sync_and_at_part_end(void) {
    reconf(0, 20000);
    begin_small(SZ);
    put_all(ID, 0, 30000, 10000); /* 20000 で sync(2 チャンク目)、残り 10000 */
    TEST_ASSERT_EQUAL_INT(1, FS.n_sync);
    put_all(ID, 30000, 34816, 4816); /* パーツの終わり: sync(カウントは 14816 だが、終わりなので) */
    TEST_ASSERT_EQUAL_INT(2, FS.n_sync);
    TEST_ASSERT_EQUAL_UINT64(0, UP.since_sync);
    put_all(ID, 34816, 44816, 10000); /* 新しいパーツ: 10000 までは sync なし */
    TEST_ASSERT_EQUAL_INT(2, FS.n_sync);
    TEST_ASSERT_EQUAL_UINT64(10000, UP.since_sync);
}

void test_default_sync_is_4_mib(void) {
    reconf(0, 0);
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "x", 1, 8 * MIB, NULL, id));
    static uint8_t chunk[131072];
    iso_upload_write_t w;
    uint64_t off = 0;
    while (off < 4 * MIB - 131072) {
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_write_chunk(&UP, id, off, chunk, sizeof(chunk), &w));
        off += sizeof(chunk);
    }
    TEST_ASSERT_EQUAL_INT(0, FS.n_sync);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_write_chunk(&UP, id, off, chunk, sizeof(chunk), &w)); /* 4 MiB ちょうど */
    TEST_ASSERT_EQUAL_INT(1, FS.n_sync);
}

/* ---- SHA-256 の逐次 ----------------------------------------------------------------------- */

void test_sha_state_follows_the_received_bytes_across_parts(void) {
    reconf(7000, 0);
    begin_small(SZ);
    put_all(ID, 0, SZ, 7000);
    TEST_ASSERT_EQUAL_UINT64(SZ, UP.sha_bytes);
    TEST_ASSERT_TRUE(UP.sha_valid);
    uint8_t got[32], want[32];
    up_sha256_final(&SHA_STATE, got);
    gen_sha(SZ, want);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got, 32);
}

/* ---- アイドル、中止 ------------------------------------------------------------------------ */

void test_idle_close_syncs_closes_writes_meta_and_frees_the_slot(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "sync /iso/11110001/p000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "overwrite /iso/11110001/meta.a len=256")); /* 採用は meta.b(世代 2)。採用していない meta.a へ世代 3 */
    TEST_ASSERT_EQUAL_INT(2, FS.nlog);
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);
    TEST_ASSERT_EQUAL_INT64(3000, mock_fs_size(&FS, P0));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state);
    TEST_ASSERT_EQUAL_UINT64(3000, entry()->received);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_NULL(iso_upload_slot_id(&UP));
    /* SHA-256 の状態は保つ */
    TEST_ASSERT_TRUE(UP.sha_valid);
    TEST_ASSERT_EQUAL_UINT64(3000, UP.sha_bytes);
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, ID, &s));
    TEST_ASSERT_EQUAL_INT8(0, s.adopted);
    TEST_ASSERT_EQUAL_UINT32(3, s.meta[0].generation);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_INCOMPLETE, s.meta[s.adopted].state);
}

void test_idle_close_is_a_noop_for_other_images_and_states(void) {
    begin_small(SZ);
    put_all(ID, 0, 1000, 1000);
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, "deadbeef")); /* 枠を持たない id */
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    TEST_ASSERT_TRUE(iso_upload_slot_busy(&UP));
    entry()->state = ISO_STATE_FINALIZING; /* finish が先に来た */
    UP.slot_finalizing = true;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, entry()->state);
    TEST_ASSERT_TRUE(iso_upload_slot_busy(&UP));
    UP.slot_finalizing = false;
    entry()->state = ISO_STATE_INCOMPLETE; /* すでに incomplete */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    entry()->state = ISO_STATE_UPLOADING;
}

void test_abort_closes_the_file_drops_sha_and_frees_the_slot(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    TEST_ASSERT_EQUAL_INT(1, FS.open_files);
    fx_settle();
    iso_upload_abort(&UP, "deadbeef"); /* 他の id には何もしない */
    TEST_ASSERT_TRUE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_INT(1, FS.open_files);
    TEST_ASSERT_TRUE(UP.sha_valid);
    iso_upload_abort(&UP, ID);
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_FALSE(UP.sha_valid);
    TEST_ASSERT_EQUAL_INT(0, FS.n_sync); /* 明示的な `fs->sync` は呼ばない(消すデータ) */
}

void test_activity_boundaries(void) {
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_RECEIVING, iso_upload_activity_from(1000, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_RECEIVING, iso_upload_activity_from(1000, 5999));
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_RECEIVING, iso_upload_activity_from(1000, 6000)); /* 5000 ms ちょうど = 5 秒以内 */
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_IDLE, iso_upload_activity_from(1000, 6001));
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_RECEIVING, iso_upload_activity_from(1000, 999)); /* 巻き戻り = 経過 0 */
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_RECEIVING, iso_upload_activity_from(UINT64_MAX, 0));
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_IDLE, iso_upload_activity_from(0, UINT64_MAX));
}

void test_activity_of_the_slot(void) {
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_NONE, iso_upload_activity(&UP, 5));
    begin_small(SZ);
    NOW_MS = 1000000;
    put_all(ID, 0, 10, 10);
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_RECEIVING, iso_upload_activity(&UP, 1005000));
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_IDLE, iso_upload_activity(&UP, 1005001));
    UP.slot_finalizing = true;
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_FINALIZING, iso_upload_activity(&UP, 1005001));
}

void test_info_reports_the_state(void) {
    iso_upload_info_t info;
    iso_upload_info(&UP, "deadbeef", &info);
    TEST_ASSERT_FALSE(info.found);
    TEST_ASSERT_TRUE(info.slot_free);
    begin_small(SZ);
    put_all(ID, 0, 1234, 1234);
    iso_upload_info(&UP, ID, &info);
    TEST_ASSERT_TRUE(info.found);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, info.state);
    TEST_ASSERT_EQUAL_UINT64(SZ, info.size_bytes);
    TEST_ASSERT_EQUAL_UINT64(1234, info.received);
    TEST_ASSERT_TRUE(info.meta_valid);
    TEST_ASSERT_FALSE(info.slot_free);
    TEST_ASSERT_TRUE(info.slot_owner);
    TEST_ASSERT_EQUAL_UINT32(131072, info.chunk_bytes);
    entry()->state = ISO_STATE_DELETING;
    iso_upload_info(&UP, ID, &info);
    TEST_ASSERT_FALSE(info.found);
}

/* ---- 再開 --------------------------------------------------------------------------------- */

void test_resume_after_idle_appends_without_writing_meta(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    fx_settle();
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 3000, 1000, &w));
    TEST_ASSERT_EQUAL_UINT64(4000, w.received);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "open_a /iso/11110001/p000")); /* 途中のパーツは追記で開く */
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "write /iso/11110001/p000 len=1000"));
    TEST_ASSERT_EQUAL_INT(2, FS.nlog); /* meta は書かない(C-10a の決定) */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, entry()->state);
    TEST_ASSERT_TRUE(iso_upload_slot_busy(&UP));
    /* SHA-256 は続いている */
    TEST_ASSERT_TRUE(UP.sha_valid);
    TEST_ASSERT_EQUAL_UINT64(4000, UP.sha_bytes);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    expect_disk_prefix(ID, PB_SMALL, 4000);
}

void test_resume_exactly_at_a_part_boundary_creates_the_next_part(void) {
    begin_small(SZ);
    put_all(ID, 0, 34816, 34816 / 2);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    fx_settle();
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 34816, 100, &w));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "open_w /iso/11110001/p001"));
    TEST_ASSERT_EQUAL_UINT64(34916, w.received);
}

void test_resume_recomputes_received_from_the_disk(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    entry()->received = 123; /* メモリの値が狂っていても、ディスクの大きさから決め直す */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OFFSET_MISMATCH, put_range(ID, 123, 10, &w));
    TEST_ASSERT_EQUAL_UINT64(3000, w.received);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state); /* 拒否したら、状態は変わらない */
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 3000, 10, &w));
}

/* 再開の PUT が、ディスクの大きさで received を訂正したときは、拒否しても、ビューを更新する(受理の場合は、1 回だけ) */
void test_a_corrected_received_notifies_the_view_even_when_the_put_is_rejected(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    entry()->received = 123;
    iso_upload_write_t w;
    int v = VIEW_CALLS;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OFFSET_MISMATCH, put_range(ID, 123, 10, &w));
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS); /* 3000 に訂正した */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OFFSET_MISMATCH, put_range(ID, 123, 10, &w));
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS); /* 訂正がなければ、更新しない */
    entry()->received = 123;
    TEST_ASSERT_EQUAL_INT(ISO_RES_EXCEEDS_SIZE, put_range(ID, 3000, (size_t)SZ, &w));
    TEST_ASSERT_EQUAL_INT(v + 2, VIEW_CALLS);
    entry()->received = 123;
    v = VIEW_CALLS;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 3000, 10, &w));
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS); /* 受理: 枠を取って、1 回だけ */
}

void test_a_corrected_received_notifies_the_view_when_the_storage_check_rejects(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    entry()->received = 123;
    iso_upload_set_free(&UP, 64 * GIB, SZ - 3000 + 64 * MIB - 1); /* 訂正後の残り(SZ - 3000)+ 予備に、1 バイト足りない */
    iso_upload_write_t w;
    int v = VIEW_CALLS;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 3000, 10, &w));
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS);
    TEST_ASSERT_EQUAL_UINT64(3000, entry()->received);
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS);
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 3000, 10, &w));
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS); /* 訂正がなければ、更新しない */
}

void test_a_rejected_put_to_an_uploading_image_does_not_notify_the_view(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    iso_upload_write_t w;
    int v = VIEW_CALLS;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OFFSET_MISMATCH, put_range(ID, 5, 10, &w));
    TEST_ASSERT_EQUAL_INT(ISO_RES_EXCEEDS_SIZE, put_range(ID, 3000, (size_t)SZ, &w));
    TEST_ASSERT_EQUAL_INT(v, VIEW_CALLS); /* uploading の進行中は、メモリの値を信じる。訂正はなく、何も変わらない */
}

void test_chunk_accepted_hook_is_called_once_per_accepted_chunk_only(void) {
    begin_small(SZ);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(0, CHUNK_CALLS);
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 0, 1000, &w));
    TEST_ASSERT_EQUAL_INT(1, CHUNK_CALLS);
    TEST_ASSERT_EQUAL_STRING(ID, CHUNK_ID);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OFFSET_MISMATCH, put_range(ID, 5, 10, &w));
    TEST_ASSERT_EQUAL_INT(1, CHUNK_CALLS); /* 拒否: 呼ばない */
    mock_fs_fail_write(&FS, 1, 0, ISO_ERR_IO);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 1000, 10, &w));
    TEST_ASSERT_EQUAL_INT(1, CHUNK_CALLS); /* 書き込みの失敗: 呼ばない */
    mock_fs_fail_write(&FS, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 1000, 10, &w)); /* 再開 */
    TEST_ASSERT_EQUAL_INT(2, CHUNK_CALLS);
}

void test_resume_is_refused_while_another_image_uses_the_slot(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    char id2[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "other", 5, SZ, NULL, id2));
    fx_settle();
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_UPLOAD_IN_PROGRESS, put_range(ID, 3000, 10, &w));
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state);
    TEST_ASSERT_EQUAL_STRING(id2, iso_upload_slot_id(&UP));
}

void test_resume_storage_check_needs_remaining_plus_reserve(void) {
    reconf(0, 0);
    const uint64_t size = 5 * GIB;
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "big", 3, size, NULL, id));
    iso_upload_write_t w;
    static uint8_t b[1000];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_write_chunk(&UP, id, 0, b, 1000, &w));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, id));
    /* 残り = size - 1000。必要 = 残り + 64 MiB */
    iso_upload_set_free(&UP, 64 * GIB, size - 1000 + 64 * MIB - 1);
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, iso_upload_write_chunk(&UP, id, 1000, b, 10, &w));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, iso_catalog_find(&CAT, id)->state);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    iso_upload_set_free(&UP, 64 * GIB, size - 1000 + 64 * MIB);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_write_chunk(&UP, id, 1000, b, 10, &w));
}

void test_resume_storage_check_counts_other_images_but_not_itself(void) {
    reconf(0, 0);
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "a", 1, 2 * GIB, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, id));
    char id2[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "b", 1, 3 * GIB, NULL, id2));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, id2));
    /* a を再開: 必要 = 2 GiB + 64 MiB。b の残り 3 GiB は引く。a 自身の残りは二重に引かない */
    iso_upload_set_free(&UP, 64 * GIB, 5 * GIB + 64 * MIB - 1);
    iso_upload_write_t w;
    static uint8_t b[10];
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, iso_upload_write_chunk(&UP, id, 0, b, 10, &w));
    iso_upload_set_free(&UP, 64 * GIB, 5 * GIB + 64 * MIB);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_write_chunk(&UP, id, 0, b, 10, &w));
}

void test_continuing_upload_has_no_storage_precheck(void) {
    begin_small(SZ);
    put_all(ID, 0, 1000, 1000);
    iso_upload_set_free(&UP, 64 * GIB, 0); /* キャッシュが 0 でも、進行中のアップロードは続く(実際の空きなしは書き込みで検出) */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 1000, 10, &w));
}

void test_consumed_since_accumulates_and_lowers_the_estimate(void) {
    iso_upload_set_free(&UP, 64 * GIB, 10 * GIB);
    begin_small(SZ);
    put_all(ID, 0, 1000, 1000);
    TEST_ASSERT_EQUAL_UINT64(1000, UP.consumed_since);
    /* 10 GiB - 1000(書いた分)- (SZ - 1000)(これから使う分) = 10 GiB - SZ */
    TEST_ASSERT_EQUAL_UINT64(10 * GIB - SZ, iso_upload_free_estimate(&UP));
}


void test_resume_one_byte_into_the_part_appends_without_truncating(void) {
    begin_small(SZ);
    put_all(ID, 0, 1, 1); /* パーツの 1 バイト目だけ */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    fx_settle();
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 1, 1000, &w));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "open_a /iso/11110001/p000")); /* 空でないファイルは、追記で開く(EXISTS で 0 に切り詰めない) */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    expect_disk_prefix(ID, PB_SMALL, 1001);
}

void test_state_changes_notify_the_view(void) {
    begin_small(SZ);
    put_all(ID, 0, 1000, 1000);
    int v = VIEW_CALLS;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS); /* アイドル: 枠を空けた */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID)); /* もう uploading でない: 何もしない */
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 1000, 10, &w)); /* 再開 */
    TEST_ASSERT_EQUAL_INT(v + 2, VIEW_CALLS);
    iso_upload_abort(&UP, "deadbeef"); /* 枠を持たない id: 何もしない */
    TEST_ASSERT_EQUAL_INT(v + 2, VIEW_CALLS);
    iso_upload_abort(&UP, ID);
    TEST_ASSERT_EQUAL_INT(v + 3, VIEW_CALLS);
}

void test_info_of_an_image_with_corrupt_meta(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/0000000f"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_upload_info_t info;
    iso_upload_info(&UP, "0000000f", &info);
    TEST_ASSERT_TRUE(info.found);
    TEST_ASSERT_FALSE(info.meta_valid);
    TEST_ASSERT_EQUAL_UINT64(0, info.size_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, info.received);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, info.state);
}

void test_reinit_drops_the_file_the_sha_state_and_the_slot(void) {
    reconf(0, 10000);
    begin_small(SZ);
    put_all(ID, 0, 6000, 6000); /* since_sync = 6000 */
    TEST_ASSERT_EQUAL_INT(1, FS.open_files);
    iso_upload_reinit(&UP);
    TEST_ASSERT_NULL(UP.f);
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_FALSE(UP.sha_valid);
    TEST_ASSERT_EQUAL_UINT64(0, UP.since_sync);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state); /* uploading は incomplete(meta は書かない) */
    /* finalizing も incomplete */
    entry()->state = ISO_STATE_FINALIZING;
    iso_upload_reinit(&UP);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state);
    entry()->state = ISO_STATE_READY; /* ほかの状態は変えない */
    iso_upload_reinit(&UP);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, entry()->state);
    entry()->state = ISO_STATE_INCOMPLETE;
    /* 別のアップロード: sync の数え始めは 0(前の分を引きずらない)。6000 バイトでは sync しない */
    char id2[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "second", 6, SZ, NULL, id2));
    put_all(id2, 0, 6000, 6000);
    TEST_ASSERT_EQUAL_INT(0, FS.n_sync);
}

void test_reinit_closes_the_handle_even_if_close_fails(void) {
    begin_small(SZ);
    put_all(ID, 0, 100, 100);
    mock_fs_set_fail_close(&FS, true);
    iso_upload_reinit(&UP);
    mock_fs_set_fail_close(&FS, false);
    TEST_ASSERT_NULL(UP.f);
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_chunk_creates_p000_and_does_not_sync);
    RUN_TEST(test_sequential_chunks_append_to_the_open_file);
    RUN_TEST(test_time_of_the_last_put_is_recorded);
    RUN_TEST(test_offset_mismatch_writes_nothing_and_reports_received);
    RUN_TEST(test_offset_mismatch_takes_priority_over_exceeds_size);
    RUN_TEST(test_chunk_size_limit);
    RUN_TEST(test_default_chunk_size_is_128_kib);
    RUN_TEST(test_empty_and_null_chunks_are_invalid);
    RUN_TEST(test_chunk_must_not_pass_size);
    RUN_TEST(test_exceeds_size_by_one_byte_in_the_middle);
    RUN_TEST(test_state_checks);
    RUN_TEST(test_meta_corrupt_image_is_not_uploading);
    RUN_TEST(test_chunk_across_a_part_boundary_is_split);
    RUN_TEST(test_chunk_ending_on_the_last_byte_of_a_part);
    RUN_TEST(test_chunk_one_byte_before_a_part_boundary_stays_in_the_part);
    RUN_TEST(test_one_chunk_covering_three_parts);
    RUN_TEST(test_chunk_starting_in_the_middle_and_crossing_two_boundaries);
    RUN_TEST(test_default_part_size_boundary_at_1_gib);
    RUN_TEST(test_default_part_size_boundary_chunk_straddles_1_gib);
    RUN_TEST(test_sync_every_sync_bytes_boundary_is_inclusive);
    RUN_TEST(test_sync_count_resets_after_each_sync_and_at_part_end);
    RUN_TEST(test_default_sync_is_4_mib);
    RUN_TEST(test_sha_state_follows_the_received_bytes_across_parts);
    RUN_TEST(test_idle_close_syncs_closes_writes_meta_and_frees_the_slot);
    RUN_TEST(test_idle_close_is_a_noop_for_other_images_and_states);
    RUN_TEST(test_abort_closes_the_file_drops_sha_and_frees_the_slot);
    RUN_TEST(test_activity_boundaries);
    RUN_TEST(test_activity_of_the_slot);
    RUN_TEST(test_info_reports_the_state);
    RUN_TEST(test_resume_after_idle_appends_without_writing_meta);
    RUN_TEST(test_resume_exactly_at_a_part_boundary_creates_the_next_part);
    RUN_TEST(test_resume_recomputes_received_from_the_disk);
    RUN_TEST(test_a_corrected_received_notifies_the_view_even_when_the_put_is_rejected);
    RUN_TEST(test_a_corrected_received_notifies_the_view_when_the_storage_check_rejects);
    RUN_TEST(test_a_rejected_put_to_an_uploading_image_does_not_notify_the_view);
    RUN_TEST(test_chunk_accepted_hook_is_called_once_per_accepted_chunk_only);
    RUN_TEST(test_resume_is_refused_while_another_image_uses_the_slot);
    RUN_TEST(test_resume_storage_check_needs_remaining_plus_reserve);
    RUN_TEST(test_resume_storage_check_counts_other_images_but_not_itself);
    RUN_TEST(test_continuing_upload_has_no_storage_precheck);
    RUN_TEST(test_consumed_since_accumulates_and_lowers_the_estimate);
    RUN_TEST(test_resume_one_byte_into_the_part_appends_without_truncating);
    RUN_TEST(test_state_changes_notify_the_view);
    RUN_TEST(test_info_of_an_image_with_corrupt_meta);
    RUN_TEST(test_reinit_drops_the_file_the_sha_state_and_the_slot);
    RUN_TEST(test_reinit_closes_the_handle_even_if_close_fails);
    return UNITY_END();
}

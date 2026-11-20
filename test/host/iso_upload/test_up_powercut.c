/*
 * 電源断の網羅(設計 3.4、HW_VERIFY I11): アップロードの全工程(begin → チャンク → アイドル → 再開 → finish)の、書き込み(open / write / sync /
 * truncate / meta の書き込み / close)の 1 つ 1 つで電源を落とし(落とし方は NONE / HALF / FULL の 3 通り)、再起動(スキャン)して確かめる:
 *   - イメージは、ない、meta_corrupt(削除できる)、incomplete、(finish の最後を過ぎていれば)ready のどれか。uploading / finalizing は残らない。
 *   - incomplete の received は、「落ちる前に同期が済んでいたことが保証されるバイト数」以上、「受け取ったバイト数 + 実行中のチャンク」以下。
 *   - ディスク上のパーツの中身は、received までソースの先頭と一致する(穴、ずれ、別のチャンクの混入がない)。
 *   - received から再開して送り切り(SHA-256 の状態は失われている)、finish が ready になり、SHA-256 がソースと一致する。
 * 「同期が済んでいたことが保証されるバイト数」は、設計 3.1 の規則(sync_bytes ごと、パーツの終わり、アイドル、finish)から、テストの中で別に数える。
 */
#include "up_common.h"

void setUp(void) {}
void tearDown(void) {}

#define SZ 80000ull
#define CHUNK 9000u
#define PHASE1_END 50000ull

typedef struct {
    uint64_t acked;   /* OK を返したチャンクの終わり */
    uint64_t durable; /* 同期が済んだことが保証される位置(進行中の操作の前の値) */
    uint64_t since;
    uint64_t inflight; /* 実行中のチャンクの大きさ(OK を返したら 0) */
    uint32_t pbytes;   /* パーツの大きさ(バイト)。begin の直後の meta の書き直しの後 */
    uint32_t sync;
    bool resized;      /* パーツの大きさの書き直しが済んだ */
} model_t;
static model_t M;

static char ID[ISO_ID_LEN + 1];
static int n_none, n_corrupt, n_incomplete, n_ready, n_lost, n_full; /* 掃引で、どの場合に当たったか */
static int n_cut_after_idle_sync, n_cut_after_finish_sync; /* sync が成功した後の meta の書き込みで落ちた回数(下限の検査が強くなる場合) */

static void model_chunk(uint64_t from, uint64_t len) {
    uint64_t pos = from, rem = len;
    while (rem > 0) {
        uint64_t pb = M.resized ? M.pbytes : (uint64_t)ISO_PART_BLOCKS_DEFAULT * 2048u;
        uint64_t seg = pb - pos % pb;
        if (seg > rem) seg = rem;
        pos += seg;
        rem -= seg;
        M.since += seg;
        if (pos % pb == 0) {
            M.durable = pos; /* パーツの終わり: sync */
            M.since = 0;
        } else if (M.since >= M.sync) {
            M.durable = pos; /* sync_bytes ごと */
            M.since = 0;
        }
    }
    M.acked = from + len;
}

/* 1 つの処理。失敗(電源断)なら、最初のエラー(非 0)を返す。後始末(作業領域の解放)は、ここで済ませる */
static int workload(void) {
    int rc = iso_upload_begin(&UP, "img", 3, SZ, NULL, ID);
    if (rc != 0) return rc ? rc : -1;
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, ID);
    iso_meta_t m = e->meta;
    m.part_blocks = PB_SMALL;
    m.part_count = (uint16_t)iso_extent_part_count(SZ, PB_SMALL);
    rc = iso_catalog_write_meta(&CAT, e, &m);
    if (rc != 0) return rc;
    M.resized = true;
    for (uint64_t off = 0; off < PHASE1_END;) {
        size_t n = (PHASE1_END - off < CHUNK) ? (size_t)(PHASE1_END - off) : CHUNK;
        M.inflight = n;
        rc = put_range(ID, off, n, NULL);
        if (rc != 0) return rc;
        M.inflight = 0;
        model_chunk(off, n);
        off += n;
    }
    /* アイドルの閉じ: 書き込みは、sync、meta の 2 つ(この順)。sync の前(または sync の最中)に落ちたなら、同期済みは保証されない。
     * sync が成功した後(meta の書き込みの最中)に落ちたなら、ここまでのデータは、すべて同期済みで、残る(下限は 50000) */
    int sync_index = FS.mut_count + 1;
    rc = iso_upload_idle_close(&UP, ID);
    if (rc != 0) {
        if (FS.cut_at > sync_index) {
            M.durable = M.acked;
            n_cut_after_idle_sync++;
        }
        return rc;
    }
    M.durable = M.acked;
    M.since = 0;
    for (uint64_t off = PHASE1_END; off < SZ;) {
        size_t n = (SZ - off < CHUNK) ? (size_t)(SZ - off) : CHUNK;
        M.inflight = n;
        rc = put_range(ID, off, n, NULL);
        if (rc != 0) return rc;
        M.inflight = 0;
        model_chunk(off, n);
        off += n;
    }
    rc = iso_upload_finish_begin(&UP, ID, NULL);
    if (rc != 0) return rc;
    iso_finish_job_t job;
    rc = iso_finish_job_init(&job, ID);
    if (rc != 0) return rc;
    /* finish の書き込みは、閉じる前の sync、ready の meta の 2 つ(この順)。meta の最中に落ちたなら、全データは同期済み */
    int fin_sync_index = FS.mut_count + 1;
    iso_step_result_t r;
    do {
        r = iso_upload_finish_step(&UP, &job, 0, false);
    } while (r.kind == ISO_STEP_CONTINUE);
    iso_finish_job_release(&UP, &job);
    if (r.kind == ISO_STEP_ERROR && FS.cut_at > fin_sync_index) {
        M.durable = M.acked;
        n_cut_after_finish_sync++;
    }
    return r.kind == ISO_STEP_ERROR ? (r.err ? r.err : -1) : 0;
}

/* 電源断の後の「別のプロセス」: UP を作り直し、カタログをスキャンする */
static void restart(void) {
    iso_upload_cfg_t cfg;
    fx_up_cfg(&cfg, 0, M.sync);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_init(&UP, &cfg));
    iso_upload_set_free(&UP, 64 * GIB, 60 * GIB);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
}

static void finish_and_check(iso_catalog_entry_t *e) {
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r;
    int guard = 0;
    do {
        r = iso_upload_finish_step(&UP, &job, 0, false);
        TEST_ASSERT_TRUE(++guard < 100000);
    } while (r.kind == ISO_STEP_CONTINUE);
    iso_finish_job_release(&UP, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, e->state);
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, ID, &s));
    TEST_ASSERT_TRUE(s.adopted >= 0);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_READY, s.meta[s.adopted].state);
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_COMPUTED, s.meta[s.adopted].sha_flag);
    uint8_t want[32];
    gen_sha(SZ, want);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, s.meta[s.adopted].sha256, 32);
    expect_disk_prefix(ID, e->meta.part_blocks, SZ);
    TEST_ASSERT_NOT_NULL(LAST_TABLE);
    TEST_ASSERT_EQUAL_UINT64(SZ, iso_extent_table_size_bytes(LAST_TABLE));
}

static void check_after_cut(int k, mock_tear_t tear) {
    char msg[96];
    snprintf(msg, sizeof(msg), "cut at write %d, tear %d", k, (int)tear);
    mock_fs_power_on(&FS);
    if (LAST_TABLE) iso_extent_table_unref(LAST_TABLE);
    LAST_TABLE = NULL;
    restart();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, FS.open_files, msg);
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, ID);
    if (!e) {
        /* mkdir の前(または、効かなかった)。何も残っていない */
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, CAT.count, msg);
        n_none++;
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(e->state != ISO_STATE_UPLOADING && e->state != ISO_STATE_FINALIZING, msg);
    if (!e->meta_valid) {
        /* mkdir と meta の間、または meta の書き込みの途中: meta_corrupt。削除できる */
        TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_STATE_INVALID, e->state, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ISO_INVALID_META_CORRUPT, e->invalid_reason, msg);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(0, M.acked, msg);
        n_corrupt++;
        iso_delete_job_t dj;
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&dj, e->id));
        iso_step_result_t r;
        do {
            r = iso_catalog_delete_step(&CAT, &dj);
        } while (r.kind == ISO_STEP_CONTINUE);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_STEP_DONE, r.kind, msg);
        TEST_ASSERT_FALSE_MESSAGE(mock_fs_exists(&FS, "/iso/11110001"), msg);
        return;
    }
    if (e->state == ISO_STATE_READY) {
        /* finish の最後の meta の書き込みが済んでいた(FULL)。INDEX で表が作れる */
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(SZ, M.acked, msg);
        n_ready++;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ISO_SHA_COMPUTED, e->meta.sha_flag, msg);
        uint8_t want[32];
        gen_sha(SZ, want);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(want, e->meta.sha256, 32);
        iso_index_job_t ij;
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&ij, e->id));
        iso_step_result_t r;
        do {
            r = iso_index_step(&UP, &ij);
        } while (r.kind == ISO_STEP_CONTINUE);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_OUTCOME_OK, ij.outcome, msg);
        iso_index_job_release(&UP, &ij);
        expect_disk_prefix(ID, e->meta.part_blocks, SZ);
        return;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_STATE_INCOMPLETE, e->state, msg);
    uint64_t R = e->received;
    n_incomplete++;
    if (R < M.acked) n_lost++;      /* 未同期の分が失われた */
    if (R == M.acked + M.inflight && M.inflight) n_full++; /* 実行中のチャンクが丸ごと残った */
    /* 同期済みが保証される分は残り、受け取った分(と実行中のチャンク)を超えない */
    TEST_ASSERT_TRUE_MESSAGE(R >= M.durable, msg);
    TEST_ASSERT_TRUE_MESSAGE(R <= M.acked + M.inflight, msg);
    expect_disk_prefix(ID, e->meta.part_blocks, R);
    /* received から再開して、送り切り、finish できる */
    for (uint64_t off = R; off < SZ;) {
        size_t n = (SZ - off < CHUNK) ? (size_t)(SZ - off) : CHUNK;
        iso_upload_write_t w;
        TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_OK, put_range(ID, off, n, &w), msg);
        off += n;
    }
    finish_and_check(e);
}

static int g_total;

static void run_one(uint32_t sync, int k, mock_tear_t tear) {
    fx_up_setup(0, sync);
    memset(&M, 0, sizeof(M));
    M.sync = sync ? sync : 4u * 1024u * 1024u;
    M.pbytes = PB_SMALL * 2048u;
    snprintf(ID, sizeof(ID), "11110001"); /* 乱数の列の先頭 */
    fx_settle();
    mock_fs_cut_at(&FS, k, tear);
    int rc = workload();
    if (k == 0) {
        TEST_ASSERT_EQUAL_INT(0, rc);
        g_total = FS.mut_count;
        TEST_ASSERT_EQUAL_UINT64(SZ, M.acked);
    } else {
        TEST_ASSERT_TRUE_MESSAGE(rc != 0, "the cut must have hit a write");
        check_after_cut(k, tear);
    }
    fx_up_teardown();
}

static void sweep(uint32_t sync, bool expect_loss) {
    n_none = n_corrupt = n_incomplete = n_ready = n_lost = n_full = 0;
    n_cut_after_idle_sync = n_cut_after_finish_sync = 0;
    run_one(sync, 0, MOCK_TEAR_NONE); /* 電源断なし: 書き込みの総数を数える */
    int total = g_total;
    TEST_ASSERT_TRUE(total > 20);
    for (int k = 1; k <= total; k++) {
        for (int t = 0; t < 3; t++) run_one(sync, k, (mock_tear_t)t);
    }
    /* 掃引が、すべての場合に当たっている(何も検査していない掃引を避ける) */
    TEST_ASSERT_TRUE(n_none > 0);
    TEST_ASSERT_TRUE(n_corrupt > 0);
    TEST_ASSERT_TRUE(n_incomplete > 20);
    TEST_ASSERT_TRUE(n_ready > 0);
    TEST_ASSERT_EQUAL_INT(3, n_cut_after_idle_sync);    /* アイドルの meta の書き込みで落ちる 3 通り(NONE / HALF / FULL) */
    TEST_ASSERT_EQUAL_INT(3, n_cut_after_finish_sync);  /* finish の ready の meta の書き込みで落ちる 3 通り */
    TEST_ASSERT_EQUAL_INT(expect_loss, n_lost > 0); /* 毎回 sync するなら、失われる分はない */
    TEST_ASSERT_EQUAL_INT(total * 3, n_none + n_corrupt + n_incomplete + n_ready);
}

void test_power_cut_at_every_write_with_periodic_syncs(void) { sweep(12000, true); }
void test_power_cut_at_every_write_with_the_default_sync_interval(void) { sweep(0, true); }
void test_power_cut_at_every_write_with_a_tiny_sync_interval(void) { sweep(1, false); }

/* 書き込みの総数は、手順から手で数えられる(sync_bytes = 12000、チャンク 9000、パーツ 34816):
 *   begin 3(mkdir、meta.a、meta.b への書き直し)
 *   前半 [0,50000): open 1、write 6 回(+ 境界をまたぐ 1 回 = 7)、sync 3(12000 バイト到達 2 回 + パーツの終わり 1 回)、open 1(p001)
 *   アイドル 2(sync、meta)
 *   後半: open_a 1、write 4 回 + 境界 1 回 = 5、sync 2(12000 到達 1 回 + パーツの終わり 1 回)、open 1(p002)
 *   finish 2(close の sync、ready の meta)
 * 3 + (1 + 7 + 3 + 1) + 2 + (1 + 5 + 2 + 1) + 2 = 28 */
void test_total_number_of_writes_matches_the_hand_count(void) {
    run_one(12000, 0, MOCK_TEAR_NONE);
    TEST_ASSERT_EQUAL_INT(28, g_total);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_power_cut_at_every_write_with_periodic_syncs);
    RUN_TEST(test_power_cut_at_every_write_with_the_default_sync_interval);
    RUN_TEST(test_power_cut_at_every_write_with_a_tiny_sync_interval);
    RUN_TEST(test_total_number_of_writes_matches_the_hand_count);
    return UNITY_END();
}

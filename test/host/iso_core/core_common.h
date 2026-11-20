#ifndef CORE_COMMON_H
#define CORE_COMMON_H

/*
 * iso_core のテスト共通の部品(実行ファイルごとに別のコピー)。up_common.h(モックの fs、偽の時計、SHA-256、データの生成)に、
 * iso_core の設定、フック(ビューの記録、マウント中の id)、ワーカーのループの代わりをする drive() を足す。
 */

#include "up_common.h"
#include "iso_core.h"

static iso_core_t CORE;
static iso_view_t VIEW_COPY;
static int VIEW_UPDATES;
static char MOUNTED[ISO_ID_LEN + 1];
static iso_sd_state_t SD_STATE;
static iso_sd_status_t SDS;

static void core_view_cb(void *ctx, const iso_view_t *v) {
    (void)ctx;
    VIEW_COPY = *v;
    VIEW_UPDATES++;
}

static bool core_mounted_cb(void *ctx, const char *id) {
    (void)ctx;
    return MOUNTED[0] && strcmp(MOUNTED, id) == 0;
}

static iso_sd_state_t sd_state_fn(void *ctx) {
    (void)ctx;
    return SD_STATE;
}

static inline void fx_core_cfg(iso_core_cfg_t *cfg, uint32_t chunk, uint32_t sync) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->cat = &CAT;
    cfg->bdev = &BDEV;
    cfg->clock = &CLK;
    cfg->sha = &SHA;
    cfg->sha_state = &SHA_STATE;
    cfg->io_buf = IOBUF;
    cfg->io_buf_size = sizeof(IOBUF);
    cfg->alloc = &ALLOC;
    cfg->sd = &SDS;
    cfg->chunk_bytes = chunk;
    cfg->sync_bytes = sync;
    cfg->hooks.view_update = core_view_cb;
    cfg->hooks.is_mounted = core_mounted_cb;
}

static inline void fx_core_setup(uint32_t chunk, uint32_t sync) {
    fx_base();
    MOUNTED[0] = '\0';
    VIEW_UPDATES = 0;
    memset(&VIEW_COPY, 0, sizeof(VIEW_COPY));
    SD_STATE = ISO_SD_READY;
    SDS.ctx = NULL;
    SDS.state = sd_state_fn;
    iso_core_cfg_t cfg;
    fx_core_cfg(&cfg, chunk, sync);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_init(&CORE, &cfg));
    UPP = &CORE.up;
    iso_upload_set_free(iso_core_upload(&CORE), 64 * GIB, 60 * GIB);
}

/* 後始末: ジョブ、表、ファイルを捨てる(boot がそれを行う)。資源の漏れを検査する */
static inline void fx_core_teardown(void) {
    mock_fs_power_on(&FS);
    mock_fs_reset_counter(&FS);
    (void)iso_core_boot(&CORE);
    int live = ALLOC_LIVE;
    int open_files = FS.open_files, open_dirs = FS.open_dirs;
    mock_fs_free(&FS);
    TEST_ASSERT_EQUAL_INT(0, live);
    TEST_ASSERT_EQUAL_INT(0, open_files);
    TEST_ASSERT_EQUAL_INT(0, open_dirs);
}

/* ---- ワーカーのループの代わり ------------------------------------------------------------------ */

typedef struct {
    iso_job_kind_t kind;
    char id[ISO_ID_LEN + 1];
    iso_step_t result;
    int err;
    int steps;
} job_log_t;

#define JOB_LOG_MAX 64
static job_log_t JOBS[JOB_LOG_MAX];
static int N_JOBS;

/* 1 つのジョブを最後まで進める。参照待ちの CONTINUE では、時計を wait_ms 進める。max_steps を超えたら失敗 */
static inline iso_step_result_t run_job(iso_job_t *job, uint32_t budget, uint64_t wait_ms, int max_steps, int *steps_out) {
    iso_step_result_t r;
    int steps = 0;
    do {
        r = iso_job_step(&CORE, job, budget);
        steps++;
        if (r.kind == ISO_STEP_CONTINUE && iso_job_waiting(job)) NOW_MS += wait_ms;
        TEST_ASSERT_TRUE_MESSAGE(steps <= max_steps, "job did not end");
    } while (r.kind == ISO_STEP_CONTINUE);
    if (steps_out) *steps_out = steps;
    return r;
}

/* pick できるジョブを、すべて最後まで。記録は JOBS に。戻り値は、動かしたジョブの数 */
static inline int drive(uint32_t budget, uint64_t wait_ms) {
    int n = 0;
    iso_job_t *job;
    N_JOBS = 0;
    while ((job = iso_core_pick(&CORE)) != NULL) {
        TEST_ASSERT_TRUE_MESSAGE(n < 1000, "too many jobs");
        job_log_t *l = &JOBS[N_JOBS < JOB_LOG_MAX ? N_JOBS : JOB_LOG_MAX - 1];
        l->kind = job->kind;
        memcpy(l->id, job->id, ISO_ID_LEN + 1);
        iso_step_result_t r = run_job(job, budget, wait_ms, 100000, &l->steps);
        l->result = r.kind;
        l->err = r.err;
        if (N_JOBS < JOB_LOG_MAX) N_JOBS++;
        n++;
    }
    return n;
}

/* ---- イメージの準備 ----------------------------------------------------------------------- */

/* ディスクにイメージを置く: state = meta の状態、parts_bytes = 置くパーツのデータの長さ(画像の先頭から) */
static inline void make_image(const char *id, const char *name, uint64_t size, uint32_t pb, uint8_t state, uint64_t parts_bytes) {
    TEST_ASSERT_TRUE(pb != 0);
    iso_meta_t m = mk_meta(name, size, pb, state, 5);
    put_meta(id, &m);
    if (parts_bytes) put_parts_data(id, pb, parts_bytes);
}

#endif /* CORE_COMMON_H */

#include "unity.h"
#include "iso_worker_logic.h"
#include "up_common.h"

#include <string.h>

/*
 * iso_worker_logic の追加のテスト(C-15。ミューテーションテストの生き残りを殺すためのもの。docs/ISO_WORKER_TESTS.md)。
 * test_iso_worker_logic.c(A-13 の PR)は書き換えない。部品(w_setup など)は、同じ形のコピー(実行ファイルごとに別)。
 * 違い: w_setup の前に OPT で、省略できるコールバック(wake_worker、set_hw_sd_state、on_view_published、on_job_error、
 * bdev、bdev->epoch_bump)を NULL にできる。ビューのコールバックが受け取った値を E に記録する。
 */

/* w_prepare / w_setup が cfg に反映する、省略の指定(setUp で 0 に戻る) */
enum {
    OPT_NO_WAKE = 1u,
    OPT_NO_HWSD = 2u,
    OPT_NO_VIEWCB = 4u,
    OPT_NO_JOBERR = 8u,
    OPT_NO_EPOCH_BUMP = 16u,
    OPT_NO_BDEV = 32u,
    OPT_BAD_CLOCK = 64u, /* iso_core_init が失敗する(now_ms なし) */
    OPT_NO_SD = 128u      /* cfg.sd = NULL */
};
#define CHECK_CTX(c) TEST_ASSERT_TRUE((c) == (void *)&E) /* os.ctx と remount_ctx は、&E を渡す(取り違えを検出する) */
static unsigned OPT;
static iso_clock_t BAD_CLK;
static iso_worker_logic_cfg_t CFG;

/* w_setup で true、w_teardown で false。アサーションの失敗で w_teardown が呼ばれなかったときは、tearDown が後始末する */
static bool W_LIVE = false;

void setUp(void) { OPT = 0u; }

typedef struct {
    iso_sd_state_t last_view_sd;
    size_t last_view_count;
    uint64_t last_consumed;
    uint32_t last_wait_ms;
    bool race_fault_once; /* remount の中で、新しい障害を通知する(再初期化の最中の障害) */
    bool op_held;
    bool state_held;
    int wake_calls;
    int view_cb_calls;
    iso_sd_state_t sd_state;
    uint32_t mount_epoch;
    bool enforce_write_fence;
    bool bump_epoch_before_next_overwrite;
    int remount_rc;
    int remount_calls;
    iso_sd_state_t remount_state;
    bool clear_fault_on_remount;
    int fail_mkdir_rc;

    int job_err_calls;
    iso_job_kind_t last_err_kind;
    char last_err_id[ISO_ID_LEN + 1u];
    int last_err_code;

    /* Trace flags captured during Step (1) critical section */
    bool fault_seq_ok;
    bool hw_state_set_in_state_lock;
    bool view_cb_saw_state_lock;
} test_env_t;

static test_env_t E;
static iso_worker_logic_t W;
static iso_sd_status_t WSD;
static iso_fs_t WFS;

static void w_state_lock(void *ctx) {
    CHECK_CTX(ctx);
    TEST_ASSERT_FALSE(E.state_held);
    E.state_held = true;
}

static void w_state_unlock(void *ctx) {
    CHECK_CTX(ctx);
    TEST_ASSERT_TRUE(E.state_held);
    E.state_held = false;
}

static void w_op_lock(void *ctx) {
    CHECK_CTX(ctx);
    /* §4.1 lock order: never acquire op_mutex while holding state_mutex. */
    TEST_ASSERT_FALSE(E.state_held);
    TEST_ASSERT_FALSE(E.op_held);
    E.op_held = true;
}

static bool w_op_trylock(void *ctx, uint32_t wait_ms) {
    CHECK_CTX(ctx);
    E.last_wait_ms = wait_ms;
    TEST_ASSERT_FALSE(E.state_held);
    if (E.op_held) {
        return false;
    }
    E.op_held = true;
    return true;
}

static void w_op_unlock(void *ctx) {
    CHECK_CTX(ctx);
    TEST_ASSERT_TRUE(E.op_held);
    E.op_held = false;
}

static void w_wake(void *ctx) {
    CHECK_CTX(ctx);
    /* wake_worker must always be invoked with NO locks held. */
    TEST_ASSERT_FALSE(E.op_held);
    TEST_ASSERT_FALSE(E.state_held);
    E.wake_calls++;
}

static void w_set_hw_state(void *ctx, iso_sd_state_t st) {
    CHECK_CTX(ctx);
    TEST_ASSERT_TRUE(E.state_held);
    TEST_ASSERT_FALSE(E.op_held);
    E.hw_state_set_in_state_lock = true;
    E.sd_state = st;
}

static void w_on_view_pub(void *ctx, const iso_view_t *view, uint64_t consumed_since) {
    CHECK_CTX(ctx);
    TEST_ASSERT_TRUE(E.state_held);
    E.view_cb_calls++;
    E.view_cb_saw_state_lock = true;
    TEST_ASSERT_NOT_NULL(view);
    E.last_view_sd = view->sd_state;
    E.last_view_count = view->count;
    E.last_consumed = consumed_since;
}

static void w_on_job_err(void *ctx, iso_job_kind_t kind, const char *id, int err) {
    CHECK_CTX(ctx);
    TEST_ASSERT_FALSE(E.op_held);
    TEST_ASSERT_FALSE(E.state_held);
    E.job_err_calls++;
    E.last_err_kind = kind;
    snprintf(E.last_err_id, sizeof(E.last_err_id), "%s", id ? id : "");
    E.last_err_code = err;
}

static iso_sd_state_t w_sd_state(void *ctx) {
    (void)ctx;
    return E.sd_state;
}

static void w_epoch_bump_checked(void *ctx) {
    (void)ctx;
    if (W.initialized && E.sd_state != ISO_SD_READY && !E.state_held) {
        TEST_FAIL_MESSAGE("force_eject bumped epoch outside state_lock");
    }
    E.fault_seq_ok = E.state_held && !E.op_held && E.hw_state_set_in_state_lock && !iso_media_is_loaded(&W.media);
    EPOCH++;
}

static int fence_mkdir(void *ctx, const char *path) {
    (void)ctx;
    if (E.fail_mkdir_rc != 0) return E.fail_mkdir_rc;
    if (E.enforce_write_fence && EPOCH != E.mount_epoch) return ISO_ERR_EPOCH;
    return IFS.mkdir(IFS.ctx, path);
}
static int fence_remove(void *ctx, const char *path) {
    (void)ctx;
    if (E.enforce_write_fence && EPOCH != E.mount_epoch) return ISO_ERR_EPOCH;
    return IFS.remove(IFS.ctx, path);
}
static int fence_rename(void *ctx, const char *from, const char *to) {
    (void)ctx;
    if (E.enforce_write_fence && EPOCH != E.mount_epoch) return ISO_ERR_EPOCH;
    return IFS.rename(IFS.ctx, from, to);
}
static int fence_open_w(void *ctx, const char *path, bool append, iso_file_t **out) {
    (void)ctx;
    if (E.enforce_write_fence && EPOCH != E.mount_epoch) return ISO_ERR_EPOCH;
    return IFS.open_for_write(IFS.ctx, path, append, out);
}
static int fence_write(void *ctx, iso_file_t *f, const void *data, size_t len) {
    (void)ctx;
    if (E.enforce_write_fence && EPOCH != E.mount_epoch) return ISO_ERR_EPOCH;
    return IFS.write(IFS.ctx, f, data, len);
}
static int fence_sync(void *ctx, iso_file_t *f) {
    (void)ctx;
    if (E.enforce_write_fence && EPOCH != E.mount_epoch) return ISO_ERR_EPOCH;
    return IFS.sync(IFS.ctx, f);
}
static int fence_truncate(void *ctx, const char *path, uint64_t size) {
    (void)ctx;
    if (E.enforce_write_fence && EPOCH != E.mount_epoch) return ISO_ERR_EPOCH;
    return IFS.truncate(IFS.ctx, path, size);
}
static int fence_overwrite(void *ctx, const char *path, const void *data, size_t len) {
    (void)ctx;
    if (E.bump_epoch_before_next_overwrite) {
        E.bump_epoch_before_next_overwrite = false;
        EPOCH++;
    }
    if (E.enforce_write_fence && EPOCH != E.mount_epoch) return ISO_ERR_EPOCH;
    return IFS.overwrite(IFS.ctx, path, data, len);
}

static int w_remount_cb(void *ctx) {
    CHECK_CTX(ctx);
    TEST_ASSERT_TRUE(E.op_held);
    E.remount_calls++;
    if (E.race_fault_once) {
        E.race_fault_once = false;
        iso_worker_logic_notify_sd_fault(&W, ISO_SD_NO_CARD);
    }
    if (E.clear_fault_on_remount) {
        mock_fs_power_on(&FS);
    }
    if (E.remount_rc == ISO_OK) {
        EPOCH++;
        E.mount_epoch = EPOCH;
        E.sd_state = ISO_SD_READY;
        return iso_catalog_ensure_dirs(&W.cat);
    }
    E.sd_state = E.remount_state;
    return E.remount_rc;
}

/* 準備だけ(init はしない)。cfg は CFG に作る。OPT の省略の指定を反映する */
static void w_prepare(uint32_t budget_bytes) {
    fx_base();
    memset(&E, 0, sizeof(E));
    E.sd_state = ISO_SD_READY;
    E.mount_epoch = EPOCH;
    E.remount_rc = ISO_OK;
    E.remount_state = ISO_SD_NO_CARD;

    WSD.ctx = NULL;
    WSD.state = w_sd_state;

    WFS = IFS;
    WFS.mkdir = fence_mkdir;
    WFS.remove = fence_remove;
    WFS.rename = fence_rename;
    WFS.open_for_write = fence_open_w;
    WFS.write = fence_write;
    WFS.sync = fence_sync;
    WFS.truncate = fence_truncate;
    WFS.overwrite = fence_overwrite;

    BDEV.epoch_bump = (OPT & OPT_NO_EPOCH_BUMP) ? NULL : w_epoch_bump_checked;
    memset(&BAD_CLK, 0, sizeof(BAD_CLK));

    const iso_worker_logic_cfg_t cfg = {
        .fs = &WFS,
        .bdev = (OPT & OPT_NO_BDEV) ? NULL : &BDEV,
        .clock = (OPT & OPT_BAD_CLOCK) ? &BAD_CLK : &CLK,
        .rnd = &IRND,
        .sha = &SHA,
        .sha_state = &SHA_STATE,
        .io_buf = IOBUF,
        .io_buf_size = sizeof(IOBUF),
        .alloc = &ALLOC,
        .sd = (OPT & OPT_NO_SD) ? NULL : &WSD,
        .chunk_bytes = 4096u,
        .sync_bytes = 8192u,
        .step_budget_bytes = budget_bytes,
        .os = {
            .ctx = &E,
            .state_lock = w_state_lock,
            .state_unlock = w_state_unlock,
            .op_lock = w_op_lock,
            .op_trylock = w_op_trylock,
            .op_unlock = w_op_unlock,
            .wake_worker = (OPT & OPT_NO_WAKE) ? NULL : w_wake,
            .set_hw_sd_state = (OPT & OPT_NO_HWSD) ? NULL : w_set_hw_state,
            .on_view_published = (OPT & OPT_NO_VIEWCB) ? NULL : w_on_view_pub,
            .on_job_error = (OPT & OPT_NO_JOBERR) ? NULL : w_on_job_err,
        },
    };
    CFG = cfg;
}

static void w_setup(uint32_t budget_bytes) {
    w_prepare(budget_bytes);
    W_LIVE = true;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_init(&W, &CFG));
    UPP = iso_core_upload(iso_worker_logic_core(&W));
}

static void w_teardown(void) {
    W_LIVE = false;
    mock_fs_power_on(&FS);
    iso_worker_logic_deinit(&W);
    TEST_ASSERT_FALSE(E.op_held);
    TEST_ASSERT_FALSE(E.state_held);
    int live = ALLOC_LIVE;
    int open_files = FS.open_files;
    int open_dirs = FS.open_dirs;
    mock_fs_free(&FS);
    TEST_ASSERT_EQUAL_INT(0, live);
    TEST_ASSERT_EQUAL_INT(0, open_files);
    TEST_ASSERT_EQUAL_INT(0, open_dirs);
}

/* アサーションの失敗でテストが中断したとき(w_teardown が呼ばれなかったとき)の後始末。検査はしない(元の失敗を隠さない) */
void tearDown(void) {
    if (W_LIVE) {
        W_LIVE = false;
        mock_fs_power_on(&FS);
        iso_worker_logic_deinit(&W);
        mock_fs_free(&FS);
    }
}

static void drive_worker_until_idle(int max_iters) {
    for (int i = 0; i < max_iters; i++) {
        bool busy = false;
        bool waiting = false;
        (void)iso_worker_logic_run_once(&W, w_remount_cb, &E, &busy, &waiting);
        if (!busy) {
            return;
        }
        if (waiting) {
            NOW_MS += 100u;
        }
    }
    TEST_FAIL_MESSAGE("worker loop did not become idle");
}

/* ---- 補助 ---------------------------------------------------------------------------------- */

static iso_worker_logic_t Z; /* 0 で埋めた(初期化していない)インスタンス。os の関数ポインターは全部 NULL */

/* i 番目(0..15)のイメージの id。16 進の英字(a-f)を含む */
static void seed_id(unsigned i, char out[ISO_ID_LEN + 1u]) {
    snprintf(out, ISO_ID_LEN + 1u, "%08x", 0x0a0b0c00u + i * 0x11u);
}

static void seed_ready(const char *id, unsigned n) {
    char name[24];
    snprintf(name, sizeof(name), "img%02u.iso", n);
    iso_meta_t m = mk_meta(name, PART_SMALL, PB_SMALL, ISO_META_READY, 2u);
    put_meta(id, &m);
    put_parts_data(id, PB_SMALL, PART_SMALL);
}

static void seed_images(unsigned n) {
    for (unsigned i = 0u; i < n; i++) {
        char id[ISO_ID_LEN + 1u];
        seed_id(i, id);
        seed_ready(id, i);
    }
}

static void boot_and_index_all(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    drive_worker_until_idle(512);
    TEST_ASSERT_EQUAL_INT(0, E.job_err_calls);
}

/* ディスクに完全に残っている incomplete のイメージ(SHA-256 の状態は失われている)を FINISH する。
 * パーツの読み直しは、ステップの予算(budget)ごとに区切られる。FINISH(+ INDEX)が終わるまでの run_once の回数を返す。
 * 続きのあるステップでは、waiting は偽 */
static int finish_steps(uint32_t budget) {
    w_setup(budget);
    const char *id = "5a5a5a5a";
    iso_meta_t m = mk_meta("resume.iso", PART_SMALL, PB_SMALL, ISO_META_INCOMPLETE, 2u);
    put_meta(id, &m);
    put_parts_data(id, PB_SMALL, PART_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(iso_worker_logic_core(&W), id, NULL));
    iso_worker_logic_op_unlock(&W);
    int steps = 0;
    for (;;) {
        bool busy = false;
        bool waiting = true;
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, &E, &busy, &waiting));
        steps++;
        TEST_ASSERT_FALSE(waiting); /* FINISH / INDEX は、待たない */
        if (!busy) {
            break;
        }
        TEST_ASSERT_TRUE(steps < 200);
    }
    TEST_ASSERT_EQUAL_INT(0, E.job_err_calls);
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, iso_worker_logic_view_locked(&W, NULL)->images[0].state);
    iso_worker_logic_state_unlock(&W);
    w_teardown();
    return steps;
}

/* ---- iso_worker_logic_id_to_u32 ------------------------------------------------------------ */

void test_id_to_u32_decodes_every_digit_and_rejects_bad_ids(void) {
    static const struct {
        const char *s;
        uint32_t v;
    } ok[] = {
        {"00000000", 0x00000000u}, {"00000001", 0x00000001u}, {"00000009", 0x00000009u}, {"0000000a", 0x0000000au},
        {"0000000f", 0x0000000fu}, {"00000010", 0x00000010u}, {"10000000", 0x10000000u}, {"a0000000", 0xa0000000u},
        {"0123456f", 0x0123456fu}, {"89abcdef", 0x89abcdefu}, {"fedcba98", 0xfedcba98u}, {"deadbeef", 0xdeadbeefu},
        {"ffffffff", 0xffffffffu}, {"12345678", 0x12345678u},
    };
    for (size_t i = 0u; i < sizeof(ok) / sizeof(ok[0]); i++) {
        uint32_t out = 0x5a5a5a5au;
        TEST_ASSERT_TRUE(iso_worker_logic_id_to_u32(ok[i].s, &out));
        TEST_ASSERT_EQUAL_HEX32(ok[i].v, out);
    }
    TEST_ASSERT_TRUE(iso_worker_logic_id_to_u32("deadbeef", NULL)); /* out は省略できる */

    static const char *const bad[] = {"", "1234567", "123456789", "0000000g", "0000000G", "0000000/", "0000000:",
                                      "0000000`", "0000000A", " 0000000", "0000000 ", "-0000001"};
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint32_t out = 0x5a5a5a5au;
        TEST_ASSERT_FALSE(iso_worker_logic_id_to_u32(bad[i], &out));
        TEST_ASSERT_EQUAL_HEX32(0x5a5a5a5au, out); /* 失敗のときは書かない */
    }
    uint32_t out = 0x5a5a5a5au;
    TEST_ASSERT_FALSE(iso_worker_logic_id_to_u32(NULL, &out));
    TEST_ASSERT_EQUAL_HEX32(0x5a5a5a5au, out);
}

/* ---- init / deinit ------------------------------------------------------------------------- */

void test_init_rejects_null_arguments_and_every_missing_mandatory_field(void) {
    w_prepare(8192u);
    iso_worker_logic_cfg_t c;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_worker_logic_init(NULL, &CFG));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_worker_logic_init(&W, NULL));
#define REJECT_WITHOUT(expr)                                                         \
    do {                                                                             \
        c = CFG;                                                                     \
        c.expr = NULL;                                                               \
        TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_worker_logic_init(&W, &c));      \
        TEST_ASSERT_FALSE(W.initialized);                                            \
    } while (0)
    REJECT_WITHOUT(fs);
    REJECT_WITHOUT(os.state_lock);
    REJECT_WITHOUT(os.state_unlock);
    REJECT_WITHOUT(os.op_lock);
    REJECT_WITHOUT(os.op_trylock);
    REJECT_WITHOUT(os.op_unlock);
#undef REJECT_WITHOUT

    /* 省略できるコールバックが全部 NULL でも、初期化できる */
    c = CFG;
    c.os.wake_worker = NULL;
    c.os.set_hw_sd_state = NULL;
    c.os.on_view_published = NULL;
    c.os.on_job_error = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_init(&W, &c));
    TEST_ASSERT_TRUE(W.initialized);
    w_teardown();
}

void test_init_clears_the_whole_struct_and_defaults_the_step_budget(void) {
    w_prepare(0u);
    memset(&W, 0xA5, sizeof(W)); /* 前の使い方の残り */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_init(&W, &CFG));
    TEST_ASSERT_TRUE(W.initialized);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)ISO_WORKER_BUDGET_KIB * 1024u, W.cfg.step_budget_bytes); /* 0 = 既定 */
    TEST_ASSERT_FALSE(atomic_load(&W.reinit_pending));
    TEST_ASSERT_FALSE(atomic_load(&W.reprobe_pending));
    TEST_ASSERT_EQUAL_UINT(0u, W.pub_table_n);
    TEST_ASSERT_EQUAL_UINT64(0u, W.pub_consumed_since);
    TEST_ASSERT_FALSE(W.sd_override_non_ready);
    iso_worker_logic_deinit(&W);

    CFG.step_budget_bytes = 8192u; /* 指定した値は、そのまま */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_init(&W, &CFG));
    TEST_ASSERT_EQUAL_UINT32(8192u, W.cfg.step_budget_bytes);
    w_teardown();
}

void test_init_failure_leaves_a_zeroed_uninitialized_instance(void) {
    OPT = OPT_BAD_CLOCK;
    w_prepare(8192u);
    memset(&W, 0xA5, sizeof(W));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_worker_logic_init(&W, &CFG)); /* iso_core_init が拒否する */
    TEST_ASSERT_FALSE(W.initialized);
    TEST_ASSERT_NULL(W.cfg.fs);
    TEST_ASSERT_NULL(W.cfg.os.state_lock);
    TEST_ASSERT_NULL(W.core.cfg.cat);
    TEST_ASSERT_EQUAL_UINT(0u, W.pub_table_n);
    TEST_ASSERT_NULL(iso_worker_logic_core(&W));
    TEST_ASSERT_NULL(iso_worker_logic_media(&W));
    w_teardown();
}

void test_every_entry_point_is_inert_on_null_and_uninitialized_instances(void) {
    iso_worker_logic_t *const cases[2] = {NULL, &Z};
    for (size_t k = 0u; k < 2u; k++) {
        memset(&Z, 0, sizeof(Z));
        iso_worker_logic_t *w = cases[k];

        iso_worker_logic_deinit(w);
        TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_worker_logic_boot(w));
        iso_worker_logic_notify_sd_fault(w, ISO_SD_NO_CARD);
        iso_worker_logic_request_reprobe(w);
        iso_worker_logic_tick(w, 123456u);
        TEST_ASSERT_FALSE(iso_worker_logic_op_trylock(w, 0u));
        iso_worker_logic_op_unlock(w);
        iso_worker_logic_state_lock(w);
        iso_worker_logic_state_unlock(w);

        bool busy = true;
        bool waiting = true;
        TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_worker_logic_run_once(w, NULL, NULL, &busy, &waiting));
        TEST_ASSERT_FALSE(busy);
        TEST_ASSERT_FALSE(waiting);
        TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_worker_logic_run_once(w, NULL, NULL, NULL, NULL));

        uint64_t cs = 99u;
        TEST_ASSERT_NULL(iso_worker_logic_view_locked(w, &cs));
        TEST_ASSERT_EQUAL_UINT64(0u, cs);
        TEST_ASSERT_NULL(iso_worker_logic_view_locked(w, NULL));
        uint64_t bytes = 77u;
        TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_worker_logic_lookup_size_locked(w, 1u, &bytes));
        TEST_ASSERT_EQUAL_UINT64(77u, bytes);
        TEST_ASSERT_NULL(iso_worker_logic_acquire_table_locked(w, 1u));
        TEST_ASSERT_NULL(iso_worker_logic_core(w));
        TEST_ASSERT_NULL(iso_worker_logic_media(w));
    }
}

void test_deinit_releases_the_published_tables_itself_and_is_idempotent(void) {
    w_setup(8192u);
    seed_images(ISO_MAX_IMAGES);
    boot_and_index_all();
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, W.pub_table_n);

    /* 通常は、deinit の iso_core_sd_reinit の最後の公開が、表を全部放す。deinit 自身の後始末を確かめるため、公開のフックを外す */
    W.core.cfg.hooks.view_update = NULL;
    iso_worker_logic_deinit(&W);
    TEST_ASSERT_FALSE(W.initialized);
    TEST_ASSERT_EQUAL_UINT(0u, W.pub_table_n);
    for (size_t i = 0u; i < ISO_MAX_IMAGES; i++) {
        TEST_ASSERT_NULL(W.pub_tables[i].table);
    }
    TEST_ASSERT_NULL(iso_worker_logic_core(&W));
    TEST_ASSERT_NULL(iso_worker_logic_media(&W));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_worker_logic_boot(&W));
    iso_worker_logic_deinit(&W); /* 2 回目は何もしない(os の関数を呼ばない) */
    w_teardown();                /* 表の確保が全部返っている(ALLOC_LIVE == 0) */
}

/* ---- 公開ビュー、表の引き当て ----------------------------------------------------------------- */

void test_sixteen_ready_images_are_published_looked_up_and_released(void) {
    w_setup(8192u);
    seed_images(ISO_MAX_IMAGES);
    boot_and_index_all();

    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, W.pub_table_n);
    const iso_view_t *v = iso_worker_logic_view_locked(&W, NULL);
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, v->count);
    for (size_t i = 0u; i < ISO_MAX_IMAGES; i++) {
        uint32_t u = 0u;
        TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, v->images[i].state);
        TEST_ASSERT_NULL(v->images[i].table);
        TEST_ASSERT_TRUE(iso_worker_logic_id_to_u32(v->images[i].id, &u));
        TEST_ASSERT_EQUAL_STRING(v->images[i].id, W.pub_tables[i].id);
        TEST_ASSERT_EQUAL_UINT32(u, W.pub_tables[i].id_u32);
        TEST_ASSERT_EQUAL_UINT64(PART_SMALL, W.pub_tables[i].size_bytes);
        TEST_ASSERT_NOT_NULL(W.pub_tables[i].table);

        uint64_t bytes = 0u;
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_lookup_size_locked(&W, u, &bytes));
        TEST_ASSERT_EQUAL_UINT64(PART_SMALL, bytes);
        iso_extent_table_t *t = iso_worker_logic_acquire_table_locked(&W, u);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_TRUE(t == W.pub_tables[i].table);
        TEST_ASSERT_EQUAL_UINT32(3u, iso_extent_table_refs(t)); /* カタログ + 公開 + いま取った分 */
        (void)iso_extent_table_unref(t);
    }
    /* 満杯のときに、見つからない id: 配列の外を見ない。sd_override_val が id と同じ値でも(配列の外の読み出しを検出させる) */
    W.sd_override_val = (iso_sd_state_t)5;
    uint64_t bytes = 77u;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, iso_worker_logic_lookup_size_locked(&W, 5u, &bytes));
    TEST_ASSERT_EQUAL_UINT64(77u, bytes);
    TEST_ASSERT_NULL(iso_worker_logic_acquire_table_locked(&W, 5u));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, iso_worker_logic_lookup_size_locked(&W, 0x7fffffffu, &bytes));
    TEST_ASSERT_NULL(iso_worker_logic_acquire_table_locked(&W, 0x7fffffffu));
    W.sd_override_val = ISO_SD_NO_CARD;
    iso_worker_logic_state_unlock(&W);

    /* 16 個の表がある状態での再公開: 前の 16 個を放し、新しい 16 個を持つ */
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    iso_core_publish(&W.core);
    iso_worker_logic_op_unlock(&W);
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, W.pub_table_n);
    for (size_t i = 0u; i < ISO_MAX_IMAGES; i++) {
        TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(W.pub_tables[i].table)); /* カタログ + 公開(増えも減りもしない) */
    }
    iso_worker_logic_state_unlock(&W);
    w_teardown();
}

void test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images(void) {
    w_setup(8192u);
    seed_images(3u);
    boot_and_index_all();
    iso_view_t h = *iso_core_view(&W.core); /* 表つき(コールバックの間だけ有効なポインター) */
    TEST_ASSERT_EQUAL_UINT(3u, h.count);
    for (size_t k = 0u; k < 3u; k++) {
        TEST_ASSERT_NOT_NULL(h.images[k].table);
    }
    const iso_core_hooks_t hk = W.core.cfg.hooks;
    TEST_ASSERT_NOT_NULL(hk.view_update);

    /* ctx か view が NULL のときは、何もしない */
    int calls = E.view_cb_calls;
    hk.view_update(NULL, &h);
    hk.view_update(hk.ctx, NULL);
    TEST_ASSERT_EQUAL_INT(calls, E.view_cb_calls);
    TEST_ASSERT_EQUAL_UINT(3u, W.pub_table_n);

    /* count より後ろの要素は、ready で表があっても見ない。consumed_since を持ち越す */
    iso_view_t v = h;
    v.count = 2u;
    W.core.up.consumed_since = 12345u;
    hk.view_update(hk.ctx, &v);
    TEST_ASSERT_EQUAL_INT(calls + 1, E.view_cb_calls);
    TEST_ASSERT_EQUAL_UINT(2u, E.last_view_count);
    TEST_ASSERT_EQUAL_UINT64(12345u, E.last_consumed);
    TEST_ASSERT_EQUAL_UINT(2u, W.pub_table_n);
    TEST_ASSERT_NULL(W.pub_tables[2].table); /* 前の 3 個目の残りを残さない */
    uint32_t u2 = 0u;
    TEST_ASSERT_TRUE(iso_worker_logic_id_to_u32(h.images[2].id, &u2));
    iso_worker_logic_state_lock(&W);
    uint64_t cs = 0u;
    const iso_view_t *pv = iso_worker_logic_view_locked(&W, &cs);
    TEST_ASSERT_EQUAL_UINT(2u, pv->count);
    TEST_ASSERT_EQUAL_UINT64(12345u, cs);
    for (size_t i = 0u; i < ISO_MAX_IMAGES; i++) {
        TEST_ASSERT_NULL(pv->images[i].table);
    }
    uint64_t bytes = 0u;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, iso_worker_logic_lookup_size_locked(&W, u2, &bytes));
    TEST_ASSERT_NULL(iso_worker_logic_acquire_table_locked(&W, u2));
    iso_worker_logic_state_unlock(&W);

    /* ready でも、表がない(FINALIZING 相当)、ready でない、id が不正なものは、表の一覧に入れない */
    v = h;
    v.images[0].state = ISO_STATE_FINALIZING;
    v.images[1].table = NULL;
    memcpy(v.images[2].id, "zzzzzzzz", ISO_ID_LEN + 1u);
    hk.view_update(hk.ctx, &v);
    TEST_ASSERT_EQUAL_UINT(0u, W.pub_table_n);
    for (size_t i = 0u; i < 3u; i++) {
        TEST_ASSERT_NULL(W.pub_tables[i].table);
    }
    TEST_ASSERT_EQUAL_UINT(3u, W.pub_view.count);

    /* count が上限を超えていても、上限で頭打ち(配列の外を読まない) */
    v = h;
    v.count = ISO_MAX_IMAGES + 4u;
    hk.view_update(hk.ctx, &v);
    TEST_ASSERT_EQUAL_UINT(3u, W.pub_table_n);
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, W.pub_view.count);
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, E.last_view_count);

    /* sd_state: 上書きがあれば、それを公開する。なければ、ビューの値 */
    W.sd_override_non_ready = true;
    W.sd_override_val = ISO_SD_ERROR;
    hk.view_update(hk.ctx, &h);
    TEST_ASSERT_EQUAL_INT(ISO_SD_ERROR, W.pub_view.sd_state);
    TEST_ASSERT_EQUAL_INT(ISO_SD_ERROR, E.last_view_sd);
    W.sd_override_non_ready = false;
    hk.view_update(hk.ctx, &h);
    TEST_ASSERT_EQUAL_INT(ISO_SD_READY, W.pub_view.sd_state);
    TEST_ASSERT_EQUAL_INT(ISO_SD_READY, E.last_view_sd);
    TEST_ASSERT_EQUAL_UINT(3u, W.pub_table_n);
    w_teardown();
}

void test_lookup_and_acquire_reject_media_id_zero_and_a_missing_out_pointer(void) {
    w_setup(8192u);
    seed_ready("00000001", 1u);
    boot_and_index_all();

    iso_worker_logic_state_lock(&W);
    uint64_t bytes = 0u;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_lookup_size_locked(&W, 1u, &bytes)); /* id 1 は有効 */
    TEST_ASSERT_EQUAL_UINT64(PART_SMALL, bytes);
    iso_extent_table_t *t = iso_worker_logic_acquire_table_locked(&W, 1u);
    TEST_ASSERT_NOT_NULL(t);
    (void)iso_extent_table_unref(t);
    bytes = 77u;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_worker_logic_lookup_size_locked(&W, 0u, &bytes));
    TEST_ASSERT_EQUAL_UINT64(77u, bytes);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_worker_logic_lookup_size_locked(&W, 1u, NULL));
    TEST_ASSERT_NULL(iso_worker_logic_acquire_table_locked(&W, 0u));
    iso_worker_logic_state_unlock(&W);

    /* 予約の id 0 のイメージがビューにあっても、media_id 0 では引けない(0 は「なし」の値) */
    iso_view_t v = *iso_core_view(&W.core);
    v.count = 1u;
    memcpy(v.images[0].id, "00000000", ISO_ID_LEN + 1u);
    W.core.cfg.hooks.view_update(W.core.cfg.hooks.ctx, &v);
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_UINT(1u, W.pub_table_n);
    TEST_ASSERT_EQUAL_UINT32(0u, W.pub_tables[0].id_u32);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_worker_logic_lookup_size_locked(&W, 0u, &bytes));
    TEST_ASSERT_NULL(iso_worker_logic_acquire_table_locked(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, iso_worker_logic_lookup_size_locked(&W, 1u, &bytes));
    TEST_ASSERT_NULL(iso_worker_logic_acquire_table_locked(&W, 1u));
    iso_worker_logic_state_unlock(&W);
    w_teardown();
}

void test_is_mounted_hook_compares_the_loaded_image_id_only(void) {
    w_setup(8192u);
    seed_ready("0000000a", 0u);
    seed_ready("0000000b", 1u);
    boot_and_index_all();
    const iso_core_hooks_t hk = W.core.cfg.hooks;
    TEST_ASSERT_NOT_NULL(hk.is_mounted);

    TEST_ASSERT_FALSE(hk.is_mounted(NULL, "0000000a"));
    TEST_ASSERT_FALSE(hk.is_mounted(hk.ctx, NULL));
    TEST_ASSERT_FALSE(hk.is_mounted(hk.ctx, "zzzzzzzz"));
    TEST_ASSERT_FALSE(hk.is_mounted(hk.ctx, "0000000a")); /* まだ何もマウントしていない */
    TEST_ASSERT_FALSE(hk.is_mounted(hk.ctx, "00000000")); /* マウントなしの id 0 と、一致しない */

    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(iso_worker_logic_media(&W), 0x0000000au, false));
    iso_worker_logic_state_unlock(&W);
    TEST_ASSERT_TRUE(hk.is_mounted(hk.ctx, "0000000a"));
    TEST_ASSERT_FALSE(hk.is_mounted(hk.ctx, "0000000b"));
    TEST_ASSERT_FALSE(hk.is_mounted(hk.ctx, "00000000"));
    TEST_ASSERT_FALSE(hk.is_mounted(hk.ctx, "zzzzzzzz"));

    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(iso_worker_logic_media(&W), false));
    iso_worker_logic_state_unlock(&W);
    TEST_ASSERT_FALSE(hk.is_mounted(hk.ctx, "0000000a"));
    w_teardown();
}

void test_view_publish_hands_over_the_consumed_since_of_the_upload(void) {
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    char id[ISO_ID_LEN + 1u];
    iso_upload_write_t out;
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(UPP, "c.iso", 5u, PART_SMALL, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(id, 0u, 4096u, &out));
    const int calls = E.view_cb_calls;
    iso_core_publish(&W.core);
    iso_worker_logic_op_unlock(&W);
    TEST_ASSERT_EQUAL_INT(calls + 1, E.view_cb_calls);
    TEST_ASSERT_TRUE(W.core.up.consumed_since >= 4096u);
    TEST_ASSERT_EQUAL_UINT64(W.core.up.consumed_since, E.last_consumed);
    uint64_t cs = 0u;
    iso_worker_logic_state_lock(&W);
    (void)iso_worker_logic_view_locked(&W, &cs);
    iso_worker_logic_state_unlock(&W);
    TEST_ASSERT_EQUAL_UINT64(W.core.up.consumed_since, cs);
    w_teardown();
}

/* ---- boot ---------------------------------------------------------------------------------- */

void test_boot_with_a_not_ready_sd_only_publishes_the_state(void) {
    w_setup(8192u);
    seed_images(1u); /* READY のイメージが 1 件ある。非 READY の起動が走査すると、公開と INDEX に現れる */
    E.sd_state = ISO_SD_NO_CARD;
    const int calls = E.view_cb_calls;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    TEST_ASSERT_EQUAL_INT(calls + 1, E.view_cb_calls);
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, E.last_view_sd);
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, iso_worker_logic_view_locked(&W, NULL)->sd_state);
    TEST_ASSERT_EQUAL_UINT(0u, iso_worker_logic_view_locked(&W, NULL)->count); /* 走査していない */
    iso_worker_logic_state_unlock(&W);
    TEST_ASSERT_EQUAL_UINT(0u, iso_core_queue_len(&W.core));
    TEST_ASSERT_EQUAL_INT(0, E.wake_calls);
    TEST_ASSERT_EQUAL_INT(0, E.remount_calls);
    w_teardown();
}

void test_boot_wakes_the_worker_only_when_there_is_work(void) {
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W)); /* 空のカード: 仕事なし */
    TEST_ASSERT_EQUAL_INT(0, E.wake_calls);
    w_teardown();

    w_setup(8192u);
    seed_images(1u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W)); /* ready のイメージ: INDEX が待っている */
    TEST_ASSERT_EQUAL_INT(1, E.wake_calls);
    drive_worker_until_idle(32);
    w_teardown();
}

void test_boot_dirs_failure_publishes_the_sd_error_through_the_override_only(void) {
    OPT = OPT_NO_HWSD; /* sd->state() は READY のまま。ERROR は上書きだけが持つ */
    w_setup(8192u);
    E.fail_mkdir_rc = ISO_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_worker_logic_boot(&W));
    TEST_ASSERT_EQUAL_INT(ISO_SD_READY, E.sd_state);
    TEST_ASSERT_TRUE(W.sd_override_non_ready);
    TEST_ASSERT_EQUAL_INT(ISO_SD_ERROR, W.sd_override_val);
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_SD_ERROR, iso_worker_logic_view_locked(&W, NULL)->sd_state);
    iso_worker_logic_state_unlock(&W);
    w_teardown();
}

/* ---- SD の障害、再試行 ------------------------------------------------------------------------ */

void test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes(void) {
    OPT = OPT_NO_HWSD; /* sd->state() は READY のまま。公開される状態は、上書きだけで決まる */
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    int views = E.view_cb_calls;
    int wakes = E.wake_calls;
    const uint32_t gen = iso_media_abort_generation(&W.media);

    iso_worker_logic_notify_sd_fault(&W, ISO_SD_READY); /* READY は障害ではない: NO_CARD にする */
    TEST_ASSERT_EQUAL_INT(views + 1, E.view_cb_calls);
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, E.last_view_sd);
    TEST_ASSERT_EQUAL_INT(wakes + 1, E.wake_calls);
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, W.pub_view.sd_state);
    TEST_ASSERT_TRUE(W.sd_override_non_ready);
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, W.sd_override_val);
    TEST_ASSERT_TRUE(atomic_load(&W.reinit_pending));
    TEST_ASSERT_EQUAL_UINT32(gen + 1u, iso_media_abort_generation(&W.media)); /* force_eject */

    iso_worker_logic_notify_sd_fault(&W, ISO_SD_ERROR); /* 障害の種類は、そのまま */
    TEST_ASSERT_EQUAL_INT(views + 2, E.view_cb_calls);
    TEST_ASSERT_EQUAL_INT(ISO_SD_ERROR, E.last_view_sd);
    TEST_ASSERT_EQUAL_INT(wakes + 2, E.wake_calls);
    TEST_ASSERT_EQUAL_INT(ISO_SD_ERROR, W.sd_override_val);

    /* 再初期化の前に公開し直しても、障害の状態のまま(sd->state() はまだ READY を返す) */
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    iso_core_publish(&W.core);
    iso_worker_logic_op_unlock(&W);
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_SD_ERROR, iso_worker_logic_view_locked(&W, NULL)->sd_state);
    iso_worker_logic_state_unlock(&W);

    /* 再初期化が済めば、上書きを外して、実際の状態を公開する */
    bool busy = true;
    bool waiting = true;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, &E, &busy, &waiting));
    TEST_ASSERT_EQUAL_INT(1, E.remount_calls);
    TEST_ASSERT_FALSE(atomic_load(&W.reinit_pending));
    TEST_ASSERT_FALSE(W.sd_override_non_ready);
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_SD_READY, iso_worker_logic_view_locked(&W, NULL)->sd_state);
    iso_worker_logic_state_unlock(&W);
    w_teardown();
}

void test_request_reprobe_sets_the_flag_and_wakes_the_worker(void) {
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    const int wakes = E.wake_calls;
    iso_worker_logic_request_reprobe(&W);
    TEST_ASSERT_TRUE(atomic_load(&W.reprobe_pending));
    TEST_ASSERT_EQUAL_INT(wakes + 1, E.wake_calls);
    drive_worker_until_idle(8); /* SD は READY で、上書きもない: 再初期化はしない */
    TEST_ASSERT_EQUAL_INT(0, E.remount_calls);
    TEST_ASSERT_FALSE(atomic_load(&W.reprobe_pending));
    w_teardown();
}

/* ---- run_once ------------------------------------------------------------------------------ */

void test_run_once_without_a_pending_flag_does_not_reinit_a_card_that_is_not_ready(void) {
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    E.sd_state = ISO_SD_NO_CARD; /* 誰も通知していない */
    bool busy = true;
    bool waiting = true;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, &E, &busy, &waiting));
    TEST_ASSERT_EQUAL_INT(0, E.remount_calls);
    TEST_ASSERT_FALSE(busy);
    TEST_ASSERT_FALSE(waiting);
    w_teardown();
}

void test_run_once_accepts_null_out_pointers_on_every_path(void) {
    w_setup(8192u);
    seed_images(2u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    /* INDEX(CONTINUE と DONE)と、空きの経路 */
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, &E, NULL, NULL));
    }
    TEST_ASSERT_EQUAL_UINT(2u, W.pub_table_n);

    /* 空きの経路: 呼び出し側が持っていた値は、偽に書き直される */
    bool busy = true;
    bool waiting = true;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, &E, &busy, &waiting));
    TEST_ASSERT_FALSE(busy);
    TEST_ASSERT_FALSE(waiting);

    /* 再初期化の経路 */
    iso_worker_logic_notify_sd_fault(&W, ISO_SD_NO_CARD);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, &E, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, E.remount_calls);
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, &E, NULL, NULL));
    }
    TEST_ASSERT_EQUAL_UINT(2u, W.pub_table_n);
    w_teardown();
}

void test_run_once_returns_the_job_error_and_the_error_callback_gets_kind_id_and_code(void) {
    w_setup(8192u);
    const char *id = "1234abcd";
    iso_meta_t m = mk_meta("x.iso", PART_SMALL, PB_SMALL, ISO_META_DELETING, 3u);
    put_meta(id, &m);
    put_parts_data(id, PB_SMALL, PART_SMALL);
    const uint32_t cl = mock_fs_first_cluster(&FS, "/iso/1234abcd/p000");
    TEST_ASSERT_TRUE(cl != 0u);
    uint8_t dummy[16] = {0};
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/other.iso", dummy, sizeof(dummy), cl));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));

    int errors = 0;
    int last_rc = ISO_OK;
    for (int i = 0; i < 16; i++) {
        bool busy = true;
        bool waiting = true;
        const int rc = iso_worker_logic_run_once(&W, w_remount_cb, &E, &busy, &waiting);
        if (rc != ISO_OK) {
            errors++;
            last_rc = rc;
        }
        if (!busy) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, errors); /* ERROR のステップだけが、エラーを返す */
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_CROSSLINKED, last_rc);
    TEST_ASSERT_EQUAL_INT(1, E.job_err_calls);
    TEST_ASSERT_EQUAL_INT(ISO_JOB_DELETE, E.last_err_kind);
    TEST_ASSERT_EQUAL_STRING(id, E.last_err_id);
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_CROSSLINKED, E.last_err_code);
    w_teardown();
}

/* ---- 省略できるコールバック -------------------------------------------------------------------- */

void test_optional_callbacks_may_all_be_null(void) {
    OPT = OPT_NO_WAKE | OPT_NO_HWSD | OPT_NO_VIEWCB | OPT_NO_JOBERR;
    w_setup(8192u);
    const char *id = "1234abcd";
    iso_meta_t m = mk_meta("x.iso", PART_SMALL, PB_SMALL, ISO_META_DELETING, 3u);
    put_meta(id, &m);
    put_parts_data(id, PB_SMALL, PART_SMALL);
    const uint32_t cl = mock_fs_first_cluster(&FS, "/iso/1234abcd/p000");
    uint8_t dummy[16] = {0};
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/other.iso", dummy, sizeof(dummy), cl));
    seed_images(2u);

    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W)); /* ビューの公開と、wake が NULL でも落ちない */
    int errors = 0;
    for (int i = 0; i < 64; i++) {
        bool busy = false;
        bool waiting = false;
        if (iso_worker_logic_run_once(&W, w_remount_cb, &E, &busy, &waiting) != ISO_OK) {
            errors++;
        }
        if (!busy) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_UINT(2u, W.pub_table_n);

    iso_worker_logic_notify_sd_fault(&W, ISO_SD_ERROR);
    iso_worker_logic_request_reprobe(&W);
    iso_worker_logic_tick(&W, NOW_MS + 1000u);
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    iso_worker_logic_op_unlock(&W);
    drive_worker_until_idle(64);
    TEST_ASSERT_EQUAL_INT(1, E.remount_calls);
    TEST_ASSERT_EQUAL_UINT(2u, W.pub_table_n);
    w_teardown();
}

void test_force_eject_tolerates_a_missing_epoch_bump_and_a_missing_bdev(void) {
    OPT = OPT_NO_EPOCH_BUMP;
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    uint32_t gen = iso_media_abort_generation(&W.media);
    iso_worker_logic_notify_sd_fault(&W, ISO_SD_NO_CARD);
    TEST_ASSERT_EQUAL_UINT32(gen + 1u, iso_media_abort_generation(&W.media));
    w_teardown();

    OPT = OPT_NO_BDEV;
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    gen = iso_media_abort_generation(&W.media);
    iso_worker_logic_notify_sd_fault(&W, ISO_SD_NO_CARD);
    TEST_ASSERT_EQUAL_UINT32(gen + 1u, iso_media_abort_generation(&W.media));
    w_teardown();
}

void test_init_wires_every_cfg_field_into_the_core_the_catalog_and_the_media(void) {
    w_setup(8192u);
    TEST_ASSERT_TRUE(W.cat.fs == &WFS);
    TEST_ASSERT_TRUE(W.cat.rnd == &IRND);
    TEST_ASSERT_TRUE(W.media.on_force_eject_ctx == &W);
    TEST_ASSERT_TRUE(W.core.cfg.cat == &W.cat);
    TEST_ASSERT_TRUE(W.core.cfg.bdev == &BDEV);
    TEST_ASSERT_TRUE(W.core.cfg.clock == &CLK);
    TEST_ASSERT_TRUE(W.core.cfg.sha == &SHA);
    TEST_ASSERT_TRUE(W.core.cfg.sha_state == &SHA_STATE);
    TEST_ASSERT_TRUE(W.core.cfg.io_buf == IOBUF);
    TEST_ASSERT_EQUAL_UINT(sizeof(IOBUF), W.core.cfg.io_buf_size);
    TEST_ASSERT_TRUE(W.core.cfg.alloc == &ALLOC);
    TEST_ASSERT_TRUE(W.core.cfg.sd == &WSD);
    TEST_ASSERT_EQUAL_UINT32(4096u, W.core.cfg.chunk_bytes);
    TEST_ASSERT_EQUAL_UINT32(8192u, W.core.cfg.sync_bytes);
    TEST_ASSERT_EQUAL_UINT32(4096u, W.core.up.cfg.chunk_bytes);
    TEST_ASSERT_EQUAL_UINT32(8192u, W.core.up.cfg.sync_bytes);
    TEST_ASSERT_TRUE(W.core.cfg.hooks.ctx == &W);
    TEST_ASSERT_NOT_NULL(W.core.cfg.hooks.view_update);
    TEST_ASSERT_NOT_NULL(W.core.cfg.hooks.is_mounted);
    w_teardown();
}

void test_op_trylock_forwards_the_wait_time_and_the_result(void) {
    w_setup(8192u);
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 123u));
    TEST_ASSERT_EQUAL_UINT32(123u, E.last_wait_ms);
    TEST_ASSERT_FALSE(iso_worker_logic_op_trylock(&W, 7u)); /* もう取られている */
    TEST_ASSERT_EQUAL_UINT32(7u, E.last_wait_ms);
    iso_worker_logic_op_unlock(&W);
    w_teardown();
}

void test_a_fault_raised_during_the_reinit_keeps_the_override_and_the_pending_flag(void) {
    OPT = OPT_NO_WAKE | OPT_NO_HWSD; /* remount の中(op を持っている間)から通知するので、wake と hw state のコールバックは使わない */
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    iso_worker_logic_notify_sd_fault(&W, ISO_SD_ERROR);
    E.race_fault_once = true;

    bool busy = false;
    bool waiting = false;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, &E, &busy, &waiting));
    TEST_ASSERT_EQUAL_INT(1, E.remount_calls);
    /* 済んだ再初期化は、その最中に来た障害を、消さない */
    TEST_ASSERT_TRUE(atomic_load(&W.reinit_pending));
    TEST_ASSERT_TRUE(W.sd_override_non_ready);
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, W.sd_override_val);
    TEST_ASSERT_TRUE(busy); /* もう 1 回の再初期化が待っている */
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, iso_worker_logic_view_locked(&W, NULL)->sd_state);
    iso_worker_logic_state_unlock(&W);

    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, &E, &busy, &waiting));
    TEST_ASSERT_EQUAL_INT(2, E.remount_calls);
    TEST_ASSERT_FALSE(atomic_load(&W.reinit_pending));
    TEST_ASSERT_FALSE(W.sd_override_non_ready);
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_SD_READY, iso_worker_logic_view_locked(&W, NULL)->sd_state);
    iso_worker_logic_state_unlock(&W);
    w_teardown();
}

void test_reprobe_reinitializes_while_a_fault_override_is_published_even_if_the_card_reads_ready(void) {
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    W.sd_override_non_ready = true; /* 障害を公開済み(再初期化の要求は済んだ)。sd->state() は READY を返す */
    W.sd_override_val = ISO_SD_ERROR;
    iso_worker_logic_request_reprobe(&W);
    bool busy = false;
    bool waiting = false;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, &E, &busy, &waiting));
    TEST_ASSERT_EQUAL_INT(1, E.remount_calls);
    TEST_ASSERT_FALSE(W.sd_override_non_ready);
    w_teardown();
}

void test_a_missing_sd_status_is_not_ready_and_a_reprobe_reinitializes(void) {
    OPT = OPT_NO_SD;
    w_setup(8192u);
    seed_images(1u); /* READY のイメージが 1 件ある。走査すると、公開と INDEX に現れる */
    const int views = E.view_cb_calls;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    TEST_ASSERT_EQUAL_INT(views + 1, E.view_cb_calls); /* 走査せずに、状態だけ公開する */
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, E.last_view_sd);
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_UINT(0u, iso_worker_logic_view_locked(&W, NULL)->count); /* 走査していない */
    iso_worker_logic_state_unlock(&W);
    TEST_ASSERT_EQUAL_UINT(0u, iso_core_queue_len(&W.core));
    TEST_ASSERT_EQUAL_INT(0, E.wake_calls);
    iso_worker_logic_request_reprobe(&W);
    bool busy = false;
    bool waiting = false;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, &E, &busy, &waiting));
    TEST_ASSERT_EQUAL_INT(1, E.remount_calls);
    w_teardown();
}

/* 動作の検査: 公開スロットの id は、終端(NUL)までが同じ文字列で、id_u32 と大きさも同じ */
void test_view_slots_copy_the_id_with_its_terminator(void) {
    w_setup(8192u);
    seed_images(1u);
    boot_and_index_all();
    const iso_view_t v = *iso_core_view(&W.core);
    TEST_ASSERT_EQUAL_STRING(v.images[0].id, W.pub_tables[0].id);
    uint32_t want = 0u;
    TEST_ASSERT_TRUE(iso_worker_logic_id_to_u32(v.images[0].id, &want));
    TEST_ASSERT_EQUAL_UINT32(want, W.pub_tables[0].id_u32);
    TEST_ASSERT_EQUAL_UINT64(v.images[0].size_bytes, W.pub_tables[0].size_bytes);
    w_teardown();
}

/* 実装の詳細を固定するテスト(動作の検査ではない)。C は、構造体のメンバーの代入や構造体のコピーの後の詰め物の値を
 * 保証しない。ここで固定するのは、実装が公開スロットを 0 で埋めてから、id を ISO_ID_LEN + 1 バイトだけ写す、という意図
 * (id の後ろの詰め物が、元のビューの値に変わらない)。観測できる動作は変わらないので、これだけが殺すミュータントは、
 * docs/ISO_WORKER_TESTS.md の「詰め物だけを変えるもの」に分けて数える。 */
void test_impl_pin_view_slot_padding_after_the_id_is_zero(void) {
    w_setup(8192u);
    seed_images(1u);
    boot_and_index_all();
    iso_view_t v = *iso_core_view(&W.core);
    ((unsigned char *)&v.images[0])[offsetof(iso_view_image_t, id) + ISO_ID_LEN + 1u] = 0xEE; /* id の後ろの詰め物 */
    W.core.cfg.hooks.view_update(W.core.cfg.hooks.ctx, &v);
    unsigned char slot[sizeof(W.pub_tables[0])];
    memcpy(slot, &W.pub_tables[0], sizeof(slot));
    TEST_ASSERT_EQUAL_UINT8(0u, slot[offsetof(iso_worker_table_slot_t, id) + ISO_ID_LEN + 1u]);
    w_teardown();
}

void test_boot_dirs_failure_reports_the_error_to_the_hw_state_hook(void) {
    w_setup(8192u);
    E.fail_mkdir_rc = ISO_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_worker_logic_boot(&W));
    TEST_ASSERT_EQUAL_INT(ISO_SD_ERROR, E.sd_state);
    TEST_ASSERT_TRUE(E.hw_state_set_in_state_lock); /* state_lock の中で、os.ctx つきで呼ばれる */
    TEST_ASSERT_EQUAL_INT(ISO_SD_ERROR, E.last_view_sd);
    w_teardown();
}

void test_pending_work_wakes_the_worker_when_the_op_lock_is_released_or_on_tick(void) {
    /* (a) 動いているジョブだけ(キューも INDEX の集合も空): ディスクから読み直す FINISH の途中 */
    w_setup(4096u);
    const char *id = "5a5a5a5a";
    iso_meta_t m = mk_meta("resume.iso", PART_SMALL, PB_SMALL, ISO_META_INCOMPLETE, 2u);
    put_meta(id, &m);
    put_parts_data(id, PB_SMALL, PART_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(iso_worker_logic_core(&W), id, NULL));
    iso_worker_logic_op_unlock(&W);
    bool busy = false;
    bool waiting = false;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, &E, &busy, &waiting));
    TEST_ASSERT_TRUE(busy);
    TEST_ASSERT_EQUAL_UINT(0u, iso_core_queue_len(&W.core));
    TEST_ASSERT_EQUAL_UINT(0u, iso_core_index_pending(&W.core));
    TEST_ASSERT_TRUE(W.core.cur_active);
    int wakes = E.wake_calls;
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    iso_worker_logic_op_unlock(&W);
    TEST_ASSERT_EQUAL_INT(wakes + 1, E.wake_calls);
    iso_worker_logic_tick(&W, NOW_MS);
    TEST_ASSERT_EQUAL_INT(wakes + 2, E.wake_calls);
    drive_worker_until_idle(64);
    w_teardown();

    /* (b) 再確認の要求だけ */
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    iso_worker_logic_request_reprobe(&W);
    wakes = E.wake_calls;
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    iso_worker_logic_op_unlock(&W);
    TEST_ASSERT_EQUAL_INT(wakes + 1, E.wake_calls);
    iso_worker_logic_tick(&W, NOW_MS);
    TEST_ASSERT_EQUAL_INT(wakes + 2, E.wake_calls);
    drive_worker_until_idle(8);
    w_teardown();

    /* (c) 何もない: 起こさない */
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    iso_worker_logic_op_unlock(&W);
    iso_worker_logic_tick(&W, NOW_MS);
    TEST_ASSERT_EQUAL_INT(0, E.wake_calls);
    w_teardown();
}

void test_notify_hands_the_hw_hook_the_non_ready_state_and_the_published_consumed_since(void) {
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    char id[ISO_ID_LEN + 1u];
    iso_upload_write_t out;
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(UPP, "n.iso", 5u, PART_SMALL, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(id, 0u, 4096u, &out));
    iso_worker_logic_op_unlock(&W);
    TEST_ASSERT_TRUE(W.pub_consumed_since >= 4096u);

    /* コア側の消費量だけを進める(再公開しない)。通知のコールバックが受け取るのは、最後に公開した値 */
    const uint64_t published = W.pub_consumed_since;
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    W.core.up.consumed_since += 7777u;
    iso_worker_logic_op_unlock(&W);
    TEST_ASSERT_NOT_EQUAL(published, W.core.up.consumed_since);

    iso_worker_logic_notify_sd_fault(&W, ISO_SD_READY);
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, E.sd_state); /* READY は、障害の状態ではない */
    TEST_ASSERT_TRUE(E.hw_state_set_in_state_lock);
    TEST_ASSERT_EQUAL_UINT64(published, E.last_consumed);
    iso_worker_logic_notify_sd_fault(&W, ISO_SD_UNSUPPORTED_FS);
    TEST_ASSERT_EQUAL_INT(ISO_SD_UNSUPPORTED_FS, E.sd_state);
    TEST_ASSERT_EQUAL_UINT64(published, E.last_consumed);
    drive_worker_until_idle(32);
    w_teardown();
}

void test_finish_resume_steps_follow_the_configured_budget(void) {
    const int s4k = finish_steps(4096u);
    const int s16k = finish_steps(16384u);
    const int sdef = finish_steps(0u);
    const int s1m = finish_steps((uint32_t)ISO_WORKER_BUDGET_KIB * 1024u);
    TEST_ASSERT_EQUAL_INT(s1m, sdef);  /* 0 は、既定の予算 */
    TEST_ASSERT_TRUE(s16k > s1m);
    TEST_ASSERT_TRUE(s4k > s16k);
}

void test_deinit_cancels_the_jobs_and_clears_the_catalog_and_the_published_view(void) {
    w_setup(4096u);
    seed_images(2u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    TEST_ASSERT_EQUAL_UINT(2u, iso_core_index_pending(&W.core));
    iso_worker_logic_deinit(&W);
    TEST_ASSERT_EQUAL_UINT(0u, W.cat.count); /* remount が失敗する: カードを読み直さない */
    TEST_ASSERT_EQUAL_UINT(0u, iso_core_index_pending(&W.core));
    TEST_ASSERT_EQUAL_UINT(0u, iso_core_queue_len(&W.core));
    TEST_ASSERT_FALSE(W.core.cur_active);
    TEST_ASSERT_EQUAL_UINT(0u, W.pub_view.count);
    TEST_ASSERT_EQUAL_UINT(0u, W.pub_table_n);
    w_teardown();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_id_to_u32_decodes_every_digit_and_rejects_bad_ids);
    RUN_TEST(test_init_rejects_null_arguments_and_every_missing_mandatory_field);
    RUN_TEST(test_init_clears_the_whole_struct_and_defaults_the_step_budget);
    RUN_TEST(test_init_failure_leaves_a_zeroed_uninitialized_instance);
    RUN_TEST(test_every_entry_point_is_inert_on_null_and_uninitialized_instances);
    RUN_TEST(test_deinit_releases_the_published_tables_itself_and_is_idempotent);
    RUN_TEST(test_sixteen_ready_images_are_published_looked_up_and_released);
    RUN_TEST(test_view_hook_ignores_null_clamps_the_count_and_publishes_only_ready_indexed_images);
    RUN_TEST(test_lookup_and_acquire_reject_media_id_zero_and_a_missing_out_pointer);
    RUN_TEST(test_is_mounted_hook_compares_the_loaded_image_id_only);
    RUN_TEST(test_view_publish_hands_over_the_consumed_since_of_the_upload);
    RUN_TEST(test_boot_with_a_not_ready_sd_only_publishes_the_state);
    RUN_TEST(test_boot_wakes_the_worker_only_when_there_is_work);
    RUN_TEST(test_boot_dirs_failure_publishes_the_sd_error_through_the_override_only);
    RUN_TEST(test_notify_sd_fault_publishes_the_non_ready_state_until_the_reinit_completes);
    RUN_TEST(test_request_reprobe_sets_the_flag_and_wakes_the_worker);
    RUN_TEST(test_run_once_without_a_pending_flag_does_not_reinit_a_card_that_is_not_ready);
    RUN_TEST(test_run_once_accepts_null_out_pointers_on_every_path);
    RUN_TEST(test_run_once_returns_the_job_error_and_the_error_callback_gets_kind_id_and_code);
    RUN_TEST(test_optional_callbacks_may_all_be_null);
    RUN_TEST(test_force_eject_tolerates_a_missing_epoch_bump_and_a_missing_bdev);
    RUN_TEST(test_init_wires_every_cfg_field_into_the_core_the_catalog_and_the_media);
    RUN_TEST(test_op_trylock_forwards_the_wait_time_and_the_result);
    RUN_TEST(test_a_fault_raised_during_the_reinit_keeps_the_override_and_the_pending_flag);
    RUN_TEST(test_reprobe_reinitializes_while_a_fault_override_is_published_even_if_the_card_reads_ready);
    RUN_TEST(test_a_missing_sd_status_is_not_ready_and_a_reprobe_reinitializes);
    RUN_TEST(test_view_slots_copy_the_id_with_its_terminator);
    RUN_TEST(test_impl_pin_view_slot_padding_after_the_id_is_zero);
    RUN_TEST(test_boot_dirs_failure_reports_the_error_to_the_hw_state_hook);
    RUN_TEST(test_pending_work_wakes_the_worker_when_the_op_lock_is_released_or_on_tick);
    RUN_TEST(test_notify_hands_the_hw_hook_the_non_ready_state_and_the_published_consumed_since);
    RUN_TEST(test_finish_resume_steps_follow_the_configured_budget);
    RUN_TEST(test_deinit_cancels_the_jobs_and_clears_the_catalog_and_the_published_view);
    return UNITY_END();
}

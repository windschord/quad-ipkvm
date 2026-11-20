#include "unity.h"
#include "iso_worker_logic.h"
#include "up_common.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

typedef struct {
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
    (void)ctx;
    TEST_ASSERT_FALSE(E.state_held);
    E.state_held = true;
}

static void w_state_unlock(void *ctx) {
    (void)ctx;
    TEST_ASSERT_TRUE(E.state_held);
    E.state_held = false;
}

static void w_op_lock(void *ctx) {
    (void)ctx;
    /* §4.1 lock order: never acquire op_mutex while holding state_mutex. */
    TEST_ASSERT_FALSE(E.state_held);
    TEST_ASSERT_FALSE(E.op_held);
    E.op_held = true;
}

static bool w_op_trylock(void *ctx, uint32_t wait_ms) {
    (void)ctx;
    (void)wait_ms;
    TEST_ASSERT_FALSE(E.state_held);
    if (E.op_held) {
        return false;
    }
    E.op_held = true;
    return true;
}

static void w_op_unlock(void *ctx) {
    (void)ctx;
    TEST_ASSERT_TRUE(E.op_held);
    E.op_held = false;
}

static void w_wake(void *ctx) {
    (void)ctx;
    /* wake_worker must always be invoked with NO locks held. */
    TEST_ASSERT_FALSE(E.op_held);
    TEST_ASSERT_FALSE(E.state_held);
    E.wake_calls++;
}

static void w_set_hw_state(void *ctx, iso_sd_state_t st) {
    (void)ctx;
    TEST_ASSERT_TRUE(E.state_held);
    TEST_ASSERT_FALSE(E.op_held);
    E.hw_state_set_in_state_lock = true;
    E.sd_state = st;
}

static void w_on_view_pub(void *ctx, const iso_view_t *view, uint64_t consumed_since) {
    (void)ctx;
    (void)consumed_since;
    TEST_ASSERT_TRUE(E.state_held);
    E.view_cb_calls++;
    E.view_cb_saw_state_lock = true;
    TEST_ASSERT_NOT_NULL(view);
}

static void w_on_job_err(void *ctx, iso_job_kind_t kind, const char *id, int err) {
    (void)ctx;
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
    (void)ctx;
    TEST_ASSERT_TRUE(E.op_held);
    E.remount_calls++;
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

static void w_setup(uint32_t budget_bytes) {
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

    BDEV.epoch_bump = w_epoch_bump_checked;

    const iso_worker_logic_cfg_t cfg = {
        .fs = &WFS,
        .bdev = &BDEV,
        .clock = &CLK,
        .rnd = &IRND,
        .sha = &SHA,
        .sha_state = &SHA_STATE,
        .io_buf = IOBUF,
        .io_buf_size = sizeof(IOBUF),
        .alloc = &ALLOC,
        .sd = &WSD,
        .chunk_bytes = 4096u,
        .sync_bytes = 8192u,
        .step_budget_bytes = budget_bytes,
        .os = {
            .ctx = NULL,
            .state_lock = w_state_lock,
            .state_unlock = w_state_unlock,
            .op_lock = w_op_lock,
            .op_trylock = w_op_trylock,
            .op_unlock = w_op_unlock,
            .wake_worker = w_wake,
            .set_hw_sd_state = w_set_hw_state,
            .on_view_published = w_on_view_pub,
            .on_job_error = w_on_job_err,
        },
    };
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_init(&W, &cfg));
    UPP = iso_core_upload(iso_worker_logic_core(&W));
}

static void w_teardown(void) {
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

static void drive_worker_until_idle(int max_iters) {
    for (int i = 0; i < max_iters; i++) {
        bool busy = false;
        bool waiting = false;
        (void)iso_worker_logic_run_once(&W, w_remount_cb, NULL, &busy, &waiting);
        if (!busy) {
            return;
        }
        if (waiting) {
            NOW_MS += 100u;
        }
    }
    TEST_FAIL_MESSAGE("worker loop did not become idle");
}

void test_worker_boot_upload_finish_and_table_map(void) {
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));

    uint8_t sha[ISO_SHA256_LEN];
    gen_sha(PART_SMALL, sha);

    char id[ISO_ID_LEN + 1u];
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(UPP, "boot.iso", 8u, PART_SMALL, sha, id));
    iso_worker_logic_op_unlock(&W);

    for (uint64_t off = 0u; off < PART_SMALL;) {
        size_t n = (PART_SMALL - off < 4096u) ? (size_t)(PART_SMALL - off) : 4096u;
        iso_upload_write_t out;
        TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
        TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(id, off, n, &out));
        iso_worker_logic_op_unlock(&W);
        off += n;
    }

    const int wakes_before = E.wake_calls;
    uint64_t rec = 0u;
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(iso_worker_logic_core(&W), id, &rec));
    iso_worker_logic_op_unlock(&W);
    TEST_ASSERT_TRUE(E.wake_calls > wakes_before);
    TEST_ASSERT_EQUAL_UINT64(PART_SMALL, rec);

    drive_worker_until_idle(64);
    TEST_ASSERT_EQUAL_INT(0, E.job_err_calls);

    uint32_t id_u32 = 0u;
    TEST_ASSERT_TRUE(iso_worker_logic_id_to_u32(id, &id_u32));

    iso_worker_logic_state_lock(&W);
    uint64_t cs = 99u;
    const iso_view_t *v = iso_worker_logic_view_locked(&W, &cs);
    TEST_ASSERT_EQUAL_INT(ISO_SD_READY, v->sd_state);
    TEST_ASSERT_EQUAL_UINT(1u, v->count);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, v->images[0].state);
    TEST_ASSERT_NULL(v->images[0].table); /* pub_view nulls dangling pointers */
    uint64_t sz = 0u;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_lookup_size_locked(&W, id_u32, &sz));
    TEST_ASSERT_EQUAL_UINT64(PART_SMALL, sz);
    iso_extent_table_t *tbl = iso_worker_logic_acquire_table_locked(&W, id_u32);
    TEST_ASSERT_NOT_NULL(tbl);
    TEST_ASSERT_EQUAL_UINT32(3u, iso_extent_table_refs(tbl)); /* catalog + pub_tables + caller */
    iso_worker_logic_state_unlock(&W);

    (void)iso_extent_table_unref(tbl);
    w_teardown();
}

void test_worker_delete_waits_for_msc_ref_and_clears_after_target_eject(void) {
    w_setup(8192u);
    const char *id = "abcd1234";
    iso_meta_t m = mk_meta("cd.iso", PART_SMALL, PB_SMALL, ISO_META_READY, 2u);
    put_meta(id, &m);
    put_parts_data(id, PB_SMALL, PART_SMALL);

    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    drive_worker_until_idle(32);

    uint32_t id_u32 = 0u;
    TEST_ASSERT_TRUE(iso_worker_logic_id_to_u32(id, &id_u32));

    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(iso_worker_logic_media(&W), id_u32, false));
    iso_extent_table_t *in_flight_tbl = iso_worker_logic_acquire_table_locked(&W, id_u32);
    TEST_ASSERT_NOT_NULL(in_flight_tbl);
    TEST_ASSERT_EQUAL_UINT32(3u, iso_extent_table_refs(in_flight_tbl)); /* catalog + pub_tables + MSC READ */
    iso_worker_logic_state_unlock(&W);

    /* Reject DELETE while mounted. */
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_RES_MOUNTED, iso_core_submit_delete(iso_worker_logic_core(&W), id));
    iso_worker_logic_op_unlock(&W);

    /* Simulate target START STOP UNIT eject: media becomes EMPTY, queued DELETE succeeds. */
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_target_eject(iso_worker_logic_media(&W)));
    iso_worker_logic_state_unlock(&W);

    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(iso_worker_logic_core(&W), id));
    iso_worker_logic_op_unlock(&W);

    /* view_update immediately unref'd pub_tables slot; only catalog (1) + in_flight_tbl (1) remain = 2. */
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(in_flight_tbl));

    bool busy = false;
    bool waiting = false;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, NULL, &busy, &waiting));
    TEST_ASSERT_TRUE(busy);
    TEST_ASSERT_TRUE(waiting);

    /* Complete in-flight READ: ref drops to catalog-only (1), DELETE finishes cleanly. */
    TEST_ASSERT_EQUAL_UINT32(1u, iso_extent_table_unref(in_flight_tbl));
    drive_worker_until_idle(32);
    TEST_ASSERT_EQUAL_INT(0, E.job_err_calls);

    iso_worker_logic_state_lock(&W);
    const iso_view_t *v = iso_worker_logic_view_locked(&W, NULL);
    TEST_ASSERT_EQUAL_UINT(0u, v->count);
    iso_worker_logic_state_unlock(&W);

    w_teardown();
}

void test_worker_sd_fault_reinit_sequence_and_remount_recovery(void) {
    w_setup(8192u);
    const char *id = "deadbeef";
    iso_meta_t m = mk_meta("live.iso", PART_SMALL, PB_SMALL, ISO_META_READY, 2u);
    put_meta(id, &m);
    put_parts_data(id, PB_SMALL, PART_SMALL);

    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    drive_worker_until_idle(32);

    uint32_t id_u32 = 0u;
    TEST_ASSERT_TRUE(iso_worker_logic_id_to_u32(id, &id_u32));
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(iso_worker_logic_media(&W), id_u32, false));
    iso_worker_logic_state_unlock(&W);

    const uint32_t epoch_before = EPOCH;
    E.remount_rc = ISO_ERR_NO_CARD;
    E.remount_state = ISO_SD_NO_CARD;

    /* §3.4 Step (1): notify fault updates state, force-ejects media, and bumps epoch inside ONE lock section. */
    iso_worker_logic_notify_sd_fault(&W, ISO_SD_NO_CARD);
    TEST_ASSERT_TRUE(E.fault_seq_ok);
    TEST_ASSERT_EQUAL_UINT32(epoch_before + 1u, EPOCH);

    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_FALSE(iso_media_is_loaded(iso_worker_logic_media(&W)));
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, iso_worker_logic_view_locked(&W, NULL)->sd_state);
    iso_worker_logic_state_unlock(&W);

    /* Steps (2)-(7): remount fails -> stays non-READY with empty catalog. */
    bool busy = false;
    bool waiting = false;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_CARD, iso_worker_logic_run_once(&W, w_remount_cb, NULL, &busy, &waiting));
    TEST_ASSERT_FALSE(busy);
    iso_worker_logic_state_lock(&W);
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, iso_worker_logic_view_locked(&W, NULL)->sd_state);
    TEST_ASSERT_EQUAL_UINT(0u, iso_worker_logic_view_locked(&W, NULL)->count);
    iso_worker_logic_state_unlock(&W);

    /* Card re-inserted: reprobe remounts, scans, indexes, does NOT auto-mount. */
    E.remount_rc = ISO_OK;
    iso_worker_logic_request_reprobe(&W);
    drive_worker_until_idle(32);

    iso_worker_logic_state_lock(&W);
    const iso_view_t *v = iso_worker_logic_view_locked(&W, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_SD_READY, v->sd_state);
    TEST_ASSERT_EQUAL_UINT(1u, v->count);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, v->images[0].state);
    TEST_ASSERT_FALSE(iso_media_is_loaded(iso_worker_logic_media(&W))); /* never auto-mounts */
    iso_worker_logic_state_unlock(&W);

    /* Subsequent redundant reprobe while already READY does not re-run iso_core_sd_reinit. */
    const int remounts_before = E.remount_calls;
    iso_worker_logic_request_reprobe(&W);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_run_once(&W, w_remount_cb, NULL, &busy, &waiting));
    TEST_ASSERT_EQUAL_INT(remounts_before, E.remount_calls);

    w_teardown();
}

void test_worker_finish_aborts_on_epoch_bump_write_fence(void) {
    w_setup(4096u);
    E.enforce_write_fence = true;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));

    const uint64_t sz = PART_SMALL * 2u;
    uint8_t sha[ISO_SHA256_LEN];
    gen_sha(sz, sha);

    char id[ISO_ID_LEN + 1u];
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(UPP, "big.iso", 7u, sz, sha, id));
    iso_worker_logic_op_unlock(&W);

    for (uint64_t off = 0u; off < sz;) {
        size_t n = (sz - off < 4096u) ? (size_t)(sz - off) : 4096u;
        iso_upload_write_t out;
        TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
        TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(id, off, n, &out));
        iso_worker_logic_op_unlock(&W);
        off += n;
    }

    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(iso_worker_logic_core(&W), id, NULL));
    iso_worker_logic_op_unlock(&W);

    /* Simulate diskio write-fence firing right as FINISH attempts to write meta(ready). */
    E.bump_epoch_before_next_overwrite = true;
    drive_worker_until_idle(64);
    TEST_ASSERT_TRUE(E.job_err_calls > 0);
    TEST_ASSERT_EQUAL_INT(ISO_JOB_FINISH, E.last_err_kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_EPOCH, E.last_err_code);

    /* Image must remain incomplete on disk and never marked ready. */
    iso_slots_t slots;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, id, &slots));
    TEST_ASSERT_TRUE(slots.adopted >= 0);
    TEST_ASSERT_NOT_EQUAL(ISO_META_READY, slots.meta[slots.adopted].state);

    iso_worker_logic_state_lock(&W);
    const iso_view_t *v = iso_worker_logic_view_locked(&W, NULL);
    TEST_ASSERT_EQUAL_UINT(1u, v->count);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, v->images[0].state);
    iso_worker_logic_state_unlock(&W);

    w_teardown();
}

void test_worker_idle_timeout_and_put_cancel(void) {
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));

    char id[ISO_ID_LEN + 1u];
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(UPP, "idle.iso", 8u, PART_SMALL, NULL, id));
    iso_upload_write_t out;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(id, 0u, 4096u, &out));
    iso_worker_logic_op_unlock(&W);

    /* Before 300s idle: tick must not queue IDLE_CLOSE. */
    iso_worker_logic_tick(&W, NOW_MS + (uint64_t)(ISO_UPLOAD_IDLE_S - 1u) * 1000ULL);
    TEST_ASSERT_EQUAL_UINT(0u, iso_core_queue_len(iso_worker_logic_core(&W)));

    /* At 300s idle: tick queues IDLE_CLOSE, then an accepted PUT cancels it. */
    iso_worker_logic_tick(&W, NOW_MS + (uint64_t)ISO_UPLOAD_IDLE_S * 1000ULL);
    TEST_ASSERT_EQUAL_UINT(1u, iso_core_queue_len(iso_worker_logic_core(&W)));

    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(id, 4096u, 4096u, &out));
    iso_worker_logic_op_unlock(&W);
    TEST_ASSERT_EQUAL_UINT(0u, iso_core_queue_len(iso_worker_logic_core(&W)));

    /* Next 300s idle with no PUT: tick queues IDLE_CLOSE -> worker closes slot to incomplete. */
    NOW_MS += (uint64_t)ISO_UPLOAD_IDLE_S * 1000ULL;
    const int wakes_before = E.wake_calls;
    iso_worker_logic_tick(&W, NOW_MS);
    TEST_ASSERT_TRUE(E.wake_calls > wakes_before);

    drive_worker_until_idle(16);
    iso_worker_logic_state_lock(&W);
    const iso_view_t *v = iso_worker_logic_view_locked(&W, NULL);
    TEST_ASSERT_FALSE(v->upload_active);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, v->images[0].state);
    TEST_ASSERT_EQUAL_UINT64(8192u, v->images[0].received);
    iso_worker_logic_state_unlock(&W);

    w_teardown();
}

void test_worker_delete_crosslinked_logs_id_only(void) {
    w_setup(8192u);
    const char *id = "1234abcd";
    iso_meta_t m = mk_meta("secret_name.iso", PART_SMALL, PB_SMALL, ISO_META_DELETING, 3u);
    put_meta(id, &m);
    put_parts_data(id, PB_SMALL, PART_SMALL);

    /* Share p000 first_cluster with an import file -> triggers crosslinked abort during DELETE. */
    const uint32_t cl = mock_fs_first_cluster(&FS, "/iso/1234abcd/p000");
    TEST_ASSERT_TRUE(cl != 0u);
    uint8_t dummy[16] = {0};
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/other.iso", dummy, sizeof(dummy), cl));

    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    drive_worker_until_idle(16);

    TEST_ASSERT_EQUAL_INT(1, E.job_err_calls);
    TEST_ASSERT_EQUAL_INT(ISO_JOB_DELETE, E.last_err_kind);
    TEST_ASSERT_EQUAL_STRING(id, E.last_err_id);
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_CROSSLINKED, E.last_err_code);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/1234abcd/p000")); /* untouched! */

    w_teardown();
}

void test_worker_op_unlock_wakes_worker(void) {
    w_setup(8192u);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));

    const char *id = "cafebabe";
    iso_meta_t m = mk_meta("q.iso", PART_SMALL, PB_SMALL, ISO_META_READY, 1u);
    put_meta(id, &m);
    put_parts_data(id, PB_SMALL, PART_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_worker_logic_boot(&W));
    drive_worker_until_idle(16);

    const int wakes_before = E.wake_calls;
    TEST_ASSERT_TRUE(iso_worker_logic_op_trylock(&W, 0u));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(iso_worker_logic_core(&W), id));
    iso_worker_logic_op_unlock(&W);
    TEST_ASSERT_EQUAL_INT(wakes_before + 1, E.wake_calls);

    drive_worker_until_idle(16);
    w_teardown();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_worker_boot_upload_finish_and_table_map);
    RUN_TEST(test_worker_delete_waits_for_msc_ref_and_clears_after_target_eject);
    RUN_TEST(test_worker_sd_fault_reinit_sequence_and_remount_recovery);
    RUN_TEST(test_worker_finish_aborts_on_epoch_bump_write_fence);
    RUN_TEST(test_worker_idle_timeout_and_put_cancel);
    RUN_TEST(test_worker_delete_crosslinked_logs_id_only);
    RUN_TEST(test_worker_op_unlock_wakes_worker);
    return UNITY_END();
}

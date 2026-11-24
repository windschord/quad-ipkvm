#include "iso_worker.h"
#include "iso_worker_logic.h"

#include "sdkconfig.h"

#if defined(CONFIG_SD_WORKER_ENABLE) && CONFIG_SD_WORKER_ENABLE

#if !defined(CONFIG_SD_ENABLE) || !CONFIG_SD_ENABLE
#error "CONFIG_SD_WORKER_ENABLE requires CONFIG_SD_ENABLE=y"
#endif

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iso_sd.h"
#include "psa/crypto.h"

#ifndef CONFIG_SD_WORKER_TASK_STACK_SIZE
#define CONFIG_SD_WORKER_TASK_STACK_SIZE 8192
#endif
#ifndef CONFIG_SD_WORKER_TASK_PRIO
#define CONFIG_SD_WORKER_TASK_PRIO 3
#endif
#ifndef CONFIG_SD_WORKER_BUDGET_KIB
#define CONFIG_SD_WORKER_BUDGET_KIB 1024
#endif
#ifndef CONFIG_SD_WORKER_IO_BUF_KB
#define CONFIG_SD_WORKER_IO_BUF_KB 32
#endif
#ifndef CONFIG_SD_WORKER_DELETE_POLL_MS
#define CONFIG_SD_WORKER_DELETE_POLL_MS 50
#endif

static const char *TAG = "iso_worker";

typedef struct {
    bool active;
    bool failed;
    psa_hash_operation_t op;
} worker_psa_sha_ctx_t;

typedef struct {
    bool initialized;
    bool worker_started;
    _Atomic bool shutdown_req;
    _Atomic bool exited;
    _Atomic(TaskHandle_t) task;
    SemaphoreHandle_t op_mutex;
    SemaphoreHandle_t state_mutex;
    SemaphoreHandle_t done_sem;
    SemaphoreHandle_t delete_allow_sem;
    StaticSemaphore_t delete_allow_buf;
    uint8_t *io_buf;
    size_t io_buf_size;
    worker_psa_sha_ctx_t sha_ctx;
    iso_worker_logic_t logic;
    const iso_clock_t *clock;

    iso_worker_view_cb_t view_cb;
    void *view_cb_ctx;
} iso_worker_rt_t;

static iso_worker_rt_t s_worker;

/* ---- iso_random_t / iso_sha256_t / iso_allocator_t seams ----------------- */

static uint32_t worker_rand_next(void *ctx) {
    (void)ctx;
    return esp_random();
}

static const iso_random_t s_rand = {
    .ctx = NULL,
    .next = worker_rand_next,
};

static void worker_sha_init(void *state) {
    worker_psa_sha_ctx_t *sc = (worker_psa_sha_ctx_t *)state;
    if (!sc) {
        return;
    }
    if (sc->active) {
        (void)psa_hash_abort(&sc->op);
        sc->active = false;
    }
    sc->op = psa_hash_operation_init();
    sc->failed = false;
    if (psa_hash_setup(&sc->op, PSA_ALG_SHA_256) == PSA_SUCCESS) {
        sc->active = true;
    } else {
        sc->failed = true;
        ESP_LOGE(TAG, "psa_hash_setup(SHA-256) failed");
    }
}

static void worker_sha_update(void *state, const void *data, size_t len) {
    worker_psa_sha_ctx_t *sc = (worker_psa_sha_ctx_t *)state;
    if (!sc || !sc->active || sc->failed || len == 0u || !data) {
        return;
    }
    if (psa_hash_update(&sc->op, (const uint8_t *)data, len) != PSA_SUCCESS) {
        sc->failed = true;
        (void)psa_hash_abort(&sc->op);
        sc->active = false;
        ESP_LOGE(TAG, "psa_hash_update failed");
    }
}

static int worker_sha_final(void *state, uint8_t out[ISO_SHA256_LEN]) {
    if (out) {
        memset(out, 0, ISO_SHA256_LEN);
    }
    worker_psa_sha_ctx_t *sc = (worker_psa_sha_ctx_t *)state;
    if (!sc) {
        return ISO_ERR_IO;
    }
    int rc = ISO_ERR_IO;
    if (sc->active && !sc->failed && out != NULL) {
        size_t hash_len = 0u;
        psa_status_t st = psa_hash_finish(&sc->op, out, ISO_SHA256_LEN, &hash_len);
        if (st == PSA_SUCCESS && hash_len == ISO_SHA256_LEN) {
            rc = ISO_OK;
        } else {
            memset(out, 0, ISO_SHA256_LEN);
            (void)psa_hash_abort(&sc->op);
            ESP_LOGE(TAG, "psa_hash_finish failed (%d)", (int)st);
        }
    } else if (sc->active) {
        (void)psa_hash_abort(&sc->op);
    }
    sc->active = false;
    sc->failed = false;
    sc->op = psa_hash_operation_init();
    return rc;
}

static const iso_sha256_t s_sha256 = {
    .state_size = sizeof(worker_psa_sha_ctx_t),
    .init = worker_sha_init,
    .update = worker_sha_update,
    .final = worker_sha_final,
};

static void *worker_psram_alloc(void *ctx, size_t size) {
    (void)ctx;
    void *p = heap_caps_malloc_prefer(size, 2,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(size);
    }
    return p;
}

static void worker_psram_free(void *ctx, void *p) {
    (void)ctx;
    free(p);
}

static const iso_allocator_t s_alloc = {
    .ctx = NULL,
    .alloc = worker_psram_alloc,
    .free = worker_psram_free,
};

/* ---- OS / lock callbacks ------------------------------------------------- */

static void os_state_lock(void *ctx) {
    (void)ctx;
    if (s_worker.state_mutex) {
        xSemaphoreTake(s_worker.state_mutex, portMAX_DELAY);
    }
}

static void os_state_unlock(void *ctx) {
    (void)ctx;
    if (s_worker.state_mutex) {
        xSemaphoreGive(s_worker.state_mutex);
    }
}

static void os_op_lock(void *ctx) {
    (void)ctx;
    if (s_worker.op_mutex) {
        xSemaphoreTake(s_worker.op_mutex, portMAX_DELAY);
    }
}

static bool os_op_trylock(void *ctx, uint32_t wait_ms) {
    (void)ctx;
    if (!s_worker.op_mutex) {
        return false;
    }
    TickType_t ticks = pdMS_TO_TICKS(wait_ms);
    if (wait_ms > 0u && ticks == 0) {
        ticks = 1;
    }
    return xSemaphoreTake(s_worker.op_mutex, ticks) == pdTRUE;
}

static void os_op_unlock(void *ctx) {
    (void)ctx;
    if (s_worker.op_mutex) {
        xSemaphoreGive(s_worker.op_mutex);
    }
}

static void os_wake_worker(void *ctx) {
    (void)ctx;
    TaskHandle_t h = atomic_load_explicit(&s_worker.task, memory_order_acquire);
    if (h != NULL) {
        xTaskNotifyGive(h);
    }
}

static void os_set_hw_sd_state(void *ctx, iso_sd_state_t st) {
    (void)ctx;
    iso_sd_set_state(st);
}

static void os_on_view_published(void *ctx, const iso_view_t *view, uint64_t consumed_since) {
    (void)ctx;
    if (s_worker.view_cb) {
        s_worker.view_cb(s_worker.view_cb_ctx, view, consumed_since);
    }
}

static const char *job_kind_str(iso_job_kind_t kind) {
    switch (kind) {
    case ISO_JOB_DELETE:
        return "delete";
    case ISO_JOB_FINISH:
        return "finish";
    case ISO_JOB_IDLE_CLOSE:
        return "idle_close";
    case ISO_JOB_INDEX:
        return "index";
    default:
        return "unknown";
    }
}

static void os_on_job_error(void *ctx, iso_job_kind_t kind, const char *id, int err) {
    (void)ctx;
    const char *safe_id = (id != NULL && id[0] != '\0') ? id : "-";
    if (err == ISO_CAT_ERR_CROSSLINKED) {
        ESP_LOGW(TAG, "Crosslinked ISO cluster detected during %s (id=%s); aborting safely (run chkdsk)",
                 job_kind_str(kind), safe_id);
    } else {
        ESP_LOGW(TAG, "Worker job failed: kind=%s id=%s err=%d",
                 job_kind_str(kind), safe_id, err);
    }
}

/* ---- SD monitor & remount callbacks -------------------------------------- */

static void sd_on_fault_cb(void *ctx) {
    (void)ctx;
    if (atomic_load_explicit(&s_worker.shutdown_req, memory_order_acquire)) {
        return;
    }
    iso_worker_logic_notify_sd_fault(&s_worker.logic, ISO_SD_NO_CARD);
}

static void sd_on_reprobe_cb(void *ctx) {
    (void)ctx;
    if (atomic_load_explicit(&s_worker.shutdown_req, memory_order_acquire)) {
        return;
    }
    iso_worker_logic_request_reprobe(&s_worker.logic);
}

static int worker_remount_cb(void *ctx) {
    (void)ctx;
    int rc = iso_sd_remount();
    if (rc != ISO_OK) {
        return rc;
    }
    rc = iso_catalog_ensure_dirs(&s_worker.logic.cat);
    if (rc != ISO_OK) {
        ESP_LOGE(TAG, "iso_catalog_ensure_dirs after remount failed (%d)", rc);
        iso_sd_unmount();
        iso_sd_set_state(ISO_SD_ERROR);
    }
    return rc;
}

/* ---- Worker task --------------------------------------------------------- */

static void iso_worker_task(void *arg) {
    (void)arg;
    uint64_t last_tick_ms = s_worker.clock->now_ms(s_worker.clock->ctx);

    while (!atomic_load_explicit(&s_worker.shutdown_req, memory_order_acquire)) {
        const uint64_t now_ms = s_worker.clock->now_ms(s_worker.clock->ctx);
        if (now_ms >= last_tick_ms + 1000ULL || now_ms < last_tick_ms) {
            iso_worker_logic_tick(&s_worker.logic, now_ms);
            last_tick_ms = now_ms;
        }

        bool busy = false;
        bool waiting = false;
        (void)iso_worker_logic_run_once(&s_worker.logic, worker_remount_cb, NULL, &busy, &waiting);
        if (atomic_load_explicit(&s_worker.shutdown_req, memory_order_acquire)) {
            break;
        }

        if (busy && waiting) {
            TickType_t wait_ticks = pdMS_TO_TICKS(CONFIG_SD_WORKER_DELETE_POLL_MS);
            if (wait_ticks == 0) {
                wait_ticks = 1;
            }
            (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
        } else if (busy) {
            vTaskDelay(1);
        } else {
            const uint64_t t_after = s_worker.clock->now_ms(s_worker.clock->ctx);
            uint64_t rem_ms = 1000ULL;
            if (t_after >= last_tick_ms && (t_after - last_tick_ms) < 1000ULL) {
                rem_ms = 1000ULL - (t_after - last_tick_ms);
            }
            TickType_t wait_ticks = pdMS_TO_TICKS(rem_ms);
            if (wait_ticks == 0) {
                wait_ticks = 1;
            }
            (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
        }
    }

    if (s_worker.done_sem) {
        xSemaphoreGive(s_worker.done_sem);
    }
    if (s_worker.delete_allow_sem) {
        (void)xSemaphoreTake(s_worker.delete_allow_sem, portMAX_DELAY);
    }
    atomic_store_explicit(&s_worker.exited, true, memory_order_release);
    vTaskDelete(NULL);
}

/* ---- Public API ---------------------------------------------------------- */

esp_err_t iso_worker_init(void) {
    if (s_worker.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_worker, 0, sizeof(s_worker));
    atomic_init(&s_worker.shutdown_req, false);
    atomic_init(&s_worker.task, NULL);
    s_worker.sha_ctx.op = psa_hash_operation_init();

    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed");
        return ESP_FAIL;
    }

    s_worker.op_mutex = xSemaphoreCreateMutex();
    s_worker.state_mutex = xSemaphoreCreateMutex();
    s_worker.done_sem = xSemaphoreCreateBinary();
    atomic_store_explicit(&s_worker.exited, false, memory_order_release);
    s_worker.delete_allow_sem = xSemaphoreCreateBinaryStatic(&s_worker.delete_allow_buf);
    s_worker.io_buf_size = (size_t)CONFIG_SD_WORKER_IO_BUF_KB * 1024u;
    s_worker.io_buf = (uint8_t *)worker_psram_alloc(NULL, s_worker.io_buf_size);

    if (!s_worker.op_mutex || !s_worker.state_mutex || !s_worker.done_sem || !s_worker.delete_allow_sem ||
        !s_worker.io_buf) {
        iso_worker_deinit();
        return ESP_ERR_NO_MEM;
    }

    s_worker.clock = iso_sd_clock();
    const iso_worker_logic_cfg_t lcfg = {
        .fs = iso_sd_fs(),
        .bdev = iso_sd_blockdev(),
        .clock = s_worker.clock,
        .rnd = &s_rand,
        .sha = &s_sha256,
        .sha_state = &s_worker.sha_ctx,
        .io_buf = s_worker.io_buf,
        .io_buf_size = s_worker.io_buf_size,
        .alloc = &s_alloc,
        .sd = iso_sd_status(),
        .step_budget_bytes = (uint32_t)CONFIG_SD_WORKER_BUDGET_KIB * 1024u,
        .os = {
            .ctx = NULL,
            .state_lock = os_state_lock,
            .state_unlock = os_state_unlock,
            .op_lock = os_op_lock,
            .op_trylock = os_op_trylock,
            .op_unlock = os_op_unlock,
            .wake_worker = os_wake_worker,
            .set_hw_sd_state = os_set_hw_sd_state,
            .on_view_published = os_on_view_published,
            .on_job_error = os_on_job_error,
        },
    };
    int lrc = iso_worker_logic_init(&s_worker.logic, &lcfg);
    if (lrc != ISO_OK) {
        ESP_LOGE(TAG, "iso_worker_logic_init failed (%d)", lrc);
        iso_worker_deinit();
        return ESP_FAIL;
    }

    const iso_sd_config_t sd_cfg = {
        .managed_reinit = true,
        .on_fault = sd_on_fault_cb,
        .on_fault_ctx = NULL,
        .on_reprobe = sd_on_reprobe_cb,
        .on_reprobe_ctx = NULL,
    };
    esp_err_t sd_err = iso_sd_init(&sd_cfg);
    if (sd_err != ESP_OK) {
        ESP_LOGE(TAG, "iso_sd_init failed: %s", esp_err_to_name(sd_err));
        iso_worker_deinit();
        return sd_err;
    }

    int brc = iso_worker_logic_boot(&s_worker.logic);
    if (brc != ISO_OK) {
        ESP_LOGW(TAG, "iso_worker_logic_boot completed with rc=%d", brc);
        const iso_sd_status_t *sd_st = iso_sd_status();
        if (sd_st && sd_st->state && sd_st->state(sd_st->ctx) != ISO_SD_READY) {
            iso_sd_unmount();
        }
    }

    TaskHandle_t task_h = NULL;
    if (xTaskCreate(iso_worker_task, "iso_worker", CONFIG_SD_WORKER_TASK_STACK_SIZE, NULL,
                    CONFIG_SD_WORKER_TASK_PRIO, &task_h) != pdPASS) {
        iso_worker_deinit();
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_worker.task, task_h, memory_order_release);
    s_worker.worker_started = true;
    s_worker.initialized = true;
    xTaskNotifyGive(task_h);

    ESP_LOGI(TAG, "iso_worker initialized (stack=%d, budget=%d KiB, io_buf=%zu B)",
             CONFIG_SD_WORKER_TASK_STACK_SIZE, CONFIG_SD_WORKER_BUDGET_KIB, s_worker.io_buf_size);
    return ESP_OK;
}

void iso_worker_deinit(void) {
    atomic_store_explicit(&s_worker.shutdown_req, true, memory_order_release);

    /* Stop SD monitor callbacks first so fault/reprobe handlers cannot fire during shutdown,
     * keeping iso_sd locks alive while the worker finishes any in-flight remount. */
    iso_sd_stop_monitor();

    if (s_worker.worker_started) {
        TaskHandle_t task_h = atomic_load_explicit(&s_worker.task, memory_order_acquire);
        if (task_h != NULL) {
            xTaskNotifyGive(task_h);
        }
        if (s_worker.done_sem) {
            (void)xSemaphoreTake(s_worker.done_sem, portMAX_DELAY);
        }
        atomic_store_explicit(&s_worker.task, NULL, memory_order_release);
        s_worker.worker_started = false;
        if (s_worker.delete_allow_sem) {
            (void)xSemaphoreGive(s_worker.delete_allow_sem);
            while (!atomic_load_explicit(&s_worker.exited, memory_order_acquire)) {
                vTaskDelay(1);
            }
        }
    }

    iso_sd_deinit();

    iso_worker_logic_deinit(&s_worker.logic);

    if (s_worker.sha_ctx.active) {
        (void)psa_hash_abort(&s_worker.sha_ctx.op);
        s_worker.sha_ctx.active = false;
    }
    free(s_worker.io_buf);
    s_worker.io_buf = NULL;
    s_worker.io_buf_size = 0u;

    if (s_worker.op_mutex) {
        vSemaphoreDelete(s_worker.op_mutex);
        s_worker.op_mutex = NULL;
    }
    if (s_worker.state_mutex) {
        vSemaphoreDelete(s_worker.state_mutex);
        s_worker.state_mutex = NULL;
    }
    if (s_worker.done_sem) {
        vSemaphoreDelete(s_worker.done_sem);
        s_worker.done_sem = NULL;
    }
    if (s_worker.delete_allow_sem) {
        vSemaphoreDelete(s_worker.delete_allow_sem);
        s_worker.delete_allow_sem = NULL;
    }
    s_worker.initialized = false;
}

bool iso_worker_is_running(void) {
    return s_worker.initialized && s_worker.worker_started;
}

void iso_worker_set_view_cb(iso_worker_view_cb_t cb, void *ctx) {
    os_state_lock(NULL);
    s_worker.view_cb = cb;
    s_worker.view_cb_ctx = ctx;
    if (cb != NULL && s_worker.initialized) {
        cb(ctx, &s_worker.logic.pub_view, s_worker.logic.pub_consumed_since);
    }
    os_state_unlock(NULL);
}

void iso_worker_state_lock(void) {
    iso_worker_logic_state_lock(&s_worker.logic);
}

void iso_worker_state_unlock(void) {
    iso_worker_logic_state_unlock(&s_worker.logic);
}

bool iso_worker_op_trylock(uint32_t wait_ms) {
    return iso_worker_logic_op_trylock(&s_worker.logic, wait_ms);
}

void iso_worker_op_unlock(void) {
    iso_worker_logic_op_unlock(&s_worker.logic);
}

iso_core_t *iso_worker_core(void) {
    return iso_worker_logic_core(&s_worker.logic);
}

iso_media_t *iso_worker_media(void) {
    return iso_worker_logic_media(&s_worker.logic);
}

const iso_view_t *iso_worker_view_locked(uint64_t *out_consumed_since) {
    return iso_worker_logic_view_locked(&s_worker.logic, out_consumed_since);
}

int iso_worker_lookup_size_locked(uint32_t media_id, uint64_t *out_bytes) {
    return iso_worker_logic_lookup_size_locked(&s_worker.logic, media_id, out_bytes);
}

iso_extent_table_t *iso_worker_acquire_table_locked(uint32_t media_id) {
    return iso_worker_logic_acquire_table_locked(&s_worker.logic, media_id);
}

#else /* !CONFIG_SD_WORKER_ENABLE */

esp_err_t iso_worker_init(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

void iso_worker_deinit(void) {}

bool iso_worker_is_running(void) {
    return false;
}

void iso_worker_set_view_cb(iso_worker_view_cb_t cb, void *ctx) {
    (void)cb;
    (void)ctx;
}

void iso_worker_state_lock(void) {}
void iso_worker_state_unlock(void) {}

bool iso_worker_op_trylock(uint32_t wait_ms) {
    (void)wait_ms;
    return false;
}

void iso_worker_op_unlock(void) {}

iso_core_t *iso_worker_core(void) {
    return NULL;
}

iso_media_t *iso_worker_media(void) {
    return NULL;
}

const iso_view_t *iso_worker_view_locked(uint64_t *out_consumed_since) {
    if (out_consumed_since) {
        *out_consumed_since = 0u;
    }
    return NULL;
}

int iso_worker_lookup_size_locked(uint32_t media_id, uint64_t *out_bytes) {
    (void)media_id;
    (void)out_bytes;
    return ISO_ERR_NO_CARD;
}

iso_extent_table_t *iso_worker_acquire_table_locked(uint32_t media_id) {
    (void)media_id;
    return NULL;
}

#endif /* CONFIG_SD_WORKER_ENABLE */

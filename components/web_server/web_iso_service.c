#include "web_iso_service.h"
#include "sdkconfig.h"

#if defined(CONFIG_ISO_ENABLE) && CONFIG_ISO_ENABLE && \
    defined(CONFIG_SD_WORKER_ENABLE) && CONFIG_SD_WORKER_ENABLE

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iso_upload.h"
#include "iso_worker.h"
#include "lwip/sockets.h"
#include "web_iso.h"
#include "web_iso_put_logic.h"

static const char *TAG = "web_iso_svc";

#ifdef CONFIG_ISO_UPLOAD_CHUNK_KB
#define WEB_ISO_CHUNK_CAP_BYTES ((size_t)CONFIG_ISO_UPLOAD_CHUNK_KB * 1024u)
#else
#define WEB_ISO_CHUNK_CAP_BYTES ((size_t)ISO_UPLOAD_CHUNK_KB * 1024u)
#endif

static bool s_initialized = false;
static bool s_logged_stack_hwm = false;
static uint8_t *s_chunk_buf = NULL;
static size_t s_chunk_cap = 0u;
static iso_api_snapshot_t s_snapshot;

/* Updated under iso_worker_state_lock() by iso_worker whenever published view changes. */
static void on_worker_view_cb(void *ctx, const iso_view_t *view, uint64_t consumed_since) {
    (void)ctx;
    if (view == NULL) {
        return;
    }
    s_snapshot.view = *view;
    if (s_snapshot.view.count > ISO_MAX_IMAGES) {
        s_snapshot.view.count = ISO_MAX_IMAGES;
    }
    for (size_t i = 0u; i < ISO_MAX_IMAGES; i++) {
        s_snapshot.view.images[i].table = NULL;
    }
    s_snapshot.consumed_since = consumed_since;
}

static void env_state_lock(void *ctx) {
    (void)ctx;
    iso_worker_state_lock();
}

static void env_state_unlock(void *ctx) {
    (void)ctx;
    iso_worker_state_unlock();
}

static bool env_op_trylock(void *ctx, uint32_t wait_ms) {
    (void)ctx;
    return iso_worker_op_trylock(wait_ms);
}

static void env_op_unlock(void *ctx) {
    (void)ctx;
    iso_worker_op_unlock();
}

static iso_api_env_t s_env;

typedef struct {
    httpd_req_t *req;
    int fd;
    uint64_t last_ms;
    bool timeout_modified;
} put_io_ctx_t;

static uint64_t put_now_us_cb(void *ctx) {
    (void)ctx;
    return (uint64_t)esp_timer_get_time();
}

static void put_set_timeout_us_cb(void *ctx, uint64_t timeout_us) {
    put_io_ctx_t *p = (put_io_ctx_t *)ctx;
    if (p == NULL || p->fd < 0) {
        return;
    }
    uint64_t ms = (timeout_us + 999ULL) / 1000ULL;
    if (ms == 0ULL) {
        ms = 1ULL;
    }
    if (p->timeout_modified && ms == p->last_ms) {
        return;
    }
    struct timeval tv = {
        .tv_sec = (time_t)(ms / 1000ULL),
        .tv_usec = (suseconds_t)((ms % 1000ULL) * 1000ULL),
    };
    if (setsockopt(p->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0) {
        p->last_ms = ms;
        p->timeout_modified = true;
    }
}

static int put_recv_cb(void *ctx, uint8_t *buf, size_t max_len) {
    put_io_ctx_t *p = (put_io_ctx_t *)ctx;
    if (p == NULL || p->req == NULL) {
        return WEB_ISO_PUT_IO_ERROR;
    }
    int r = httpd_req_recv(p->req, (char *)buf, max_len);
    if (r > 0) {
        return r;
    }
    if (r == 0) {
        return 0;
    }
    if (r == HTTPD_SOCK_ERR_TIMEOUT) {
        return WEB_ISO_PUT_IO_TIMEOUT;
    }
    return WEB_ISO_PUT_IO_ERROR;
}

static int put_recv_chunk_cb(void *ctx, httpd_req_t *req, size_t len, const uint8_t **buf_out) {
    (void)ctx;
    if (buf_out != NULL) {
        *buf_out = NULL;
    }
    if (!s_initialized || s_chunk_buf == NULL || req == NULL || buf_out == NULL ||
        len == 0u || len > s_chunk_cap) {
        return -1;
    }

    if (!s_logged_stack_hwm) {
        s_logged_stack_hwm = true;
        ESP_LOGI(TAG, "iso httpd stack high water mark: %u bytes free",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    }

    const int fd = httpd_req_to_sockfd(req);
    struct timeval orig_tv = {0};
    bool have_orig_tv = false;
    if (fd >= 0) {
        socklen_t optlen = sizeof(orig_tv);
        if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &orig_tv, &optlen) == 0 &&
            optlen == sizeof(orig_tv)) {
            have_orig_tv = true;
        }
    }

    put_io_ctx_t io_ctx = {
        .req = req,
        .fd = fd,
        .last_ms = 0ULL,
        .timeout_modified = false,
    };
    const web_iso_put_io_t io = {
        .ctx = &io_ctx,
        .now_us = put_now_us_cb,
        .set_timeout_us = put_set_timeout_us_cb,
        .recv = put_recv_cb,
    };

    const uint64_t timeout_us = (uint64_t)ISO_PUT_RECV_TIMEOUT_S * 1000000ULL;
    const int rc = web_iso_put_recv_all(&io, s_chunk_buf, s_chunk_cap, len, timeout_us);

    if (have_orig_tv && io_ctx.timeout_modified && fd >= 0) {
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &orig_tv, sizeof(orig_tv));
    }

    if (rc != WEB_ISO_PUT_OK) {
        ESP_LOGW(TAG, "PUT chunk recv failed (rc=%d, len=%zu)", rc, len);
        return -1;
    }

    *buf_out = s_chunk_buf;
    return 0;
}

static void put_release_chunk_cb(void *ctx, const uint8_t *buf) {
    (void)ctx;
    (void)buf;
    /* Single-threaded esp_http_server reuses s_chunk_buf across requests. */
}

static const web_iso_put_hooks_t s_put_hooks = {
    .ctx = NULL,
    .recv_chunk = put_recv_chunk_cb,
    .release_chunk = put_release_chunk_cb,
};

esp_err_t web_iso_service_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }
    if (!iso_worker_is_running() || iso_worker_core() == NULL || iso_worker_media() == NULL) {
        ESP_LOGE(TAG, "iso_worker must be initialized before web_iso_service_init");
        return ESP_ERR_INVALID_STATE;
    }

    const size_t cap = WEB_ISO_CHUNK_CAP_BYTES;
    uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(64u, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = (uint8_t *)heap_caps_aligned_alloc(64u, cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PUT chunk buffer (%zu B)", cap);
        return ESP_ERR_NO_MEM;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_chunk_buf = buf;
    s_chunk_cap = cap;
    s_logged_stack_hwm = false;

    s_env = (iso_api_env_t){
        .ctx = NULL,
        .state_lock = env_state_lock,
        .state_unlock = env_state_unlock,
        .op_trylock = env_op_trylock,
        .op_unlock = env_op_unlock,
        .op_lock_wait_ms = (uint32_t)CONFIG_ISO_OP_LOCK_WAIT_MS,
        .snapshot = &s_snapshot,
        .media = iso_worker_media(),
        .core = iso_worker_core(),
    };

    iso_worker_set_view_cb(on_worker_view_cb, NULL);
    web_iso_set_env(&s_env);
    web_iso_set_put_hooks(&s_put_hooks);

    s_initialized = true;
    ESP_LOGI(TAG, "web_iso_service ready (chunk_buf=%zu B, timeout=%ds, op_wait=%dms)",
             s_chunk_cap, CONFIG_ISO_PUT_RECV_TIMEOUT_S, CONFIG_ISO_OP_LOCK_WAIT_MS);
    return ESP_OK;
}

void web_iso_service_deinit(void) {
    if (!s_initialized) {
        return;
    }
    web_iso_set_put_hooks(NULL);
    web_iso_set_env(NULL);
    iso_worker_set_view_cb(NULL, NULL);

    heap_caps_free(s_chunk_buf);
    s_chunk_buf = NULL;
    s_chunk_cap = 0u;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(&s_env, 0, sizeof(s_env));
    s_logged_stack_hwm = false;
    s_initialized = false;
}

#else /* !(CONFIG_ISO_ENABLE && CONFIG_SD_WORKER_ENABLE) */

esp_err_t web_iso_service_init(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

void web_iso_service_deinit(void) {
}

#endif

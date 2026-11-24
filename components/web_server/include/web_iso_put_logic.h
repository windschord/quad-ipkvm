#ifndef WEB_ISO_PUT_LOGIC_H
#define WEB_ISO_PUT_LOGIC_H

/*
 * Pure C11 helpers for ISO chunked PUT upload receive-loop deadline enforcement
 * and 5%-step upload progress milestone tracking (host-testable with ASan/UBSan).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_ISO_PUT_OK          0
#define WEB_ISO_PUT_ERR_INVALID (-1)
#define WEB_ISO_PUT_ERR_IO      (-2)
#define WEB_ISO_PUT_ERR_TIMEOUT (-3)

/* Return sentinels expected from web_iso_put_io_t.recv (alongside > 0 bytes or 0 for EOF). */
#define WEB_ISO_PUT_IO_ERROR   (-1)
#define WEB_ISO_PUT_IO_TIMEOUT (-2)

/* Minimum per-call socket receive timeout slice (10 ms) so SO_RCVTIMEO never rounds to 0 (infinite wait). */
#define WEB_ISO_PUT_MIN_SLICE_US 10000ULL

typedef struct {
    void *ctx;
    uint64_t (*now_us)(void *ctx);
    void (*set_timeout_us)(void *ctx, uint64_t timeout_us);
    int (*recv)(void *ctx, uint8_t *buf, size_t max_len);
} web_iso_put_io_t;

/**
 * @brief Receive exactly @p want_len bytes into @p buf within @p timeout_us total elapsed time.
 *
 * Updates socket receive timeout slice before each recv attempt so slow/stalled transfers
 * fail deterministically within ISO_PUT_RECV_TIMEOUT_S without blocking single-threaded httpd.
 */
int web_iso_put_recv_all(const web_iso_put_io_t *io,
                         uint8_t *buf,
                         size_t cap,
                         size_t want_len,
                         uint64_t timeout_us);

/**
 * @brief Emit 5% upload progress milestones (5, 10, ..., 100) without repeating buckets.
 *
 * @return Milestone percentage (5..100) when a new 5% bucket is reached, or -1 if suppressed/invalid.
 */
int web_iso_progress_step_pct(uint64_t received, uint64_t total, int *last_bucket);

#ifdef __cplusplus
}
#endif

#endif /* WEB_ISO_PUT_LOGIC_H */

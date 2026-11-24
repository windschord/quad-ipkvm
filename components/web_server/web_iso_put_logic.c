#include "web_iso_put_logic.h"

int web_iso_put_recv_all(const web_iso_put_io_t *io,
                         uint8_t *buf,
                         size_t cap,
                         size_t want_len,
                         uint64_t timeout_us) {
    if (!io || !io->now_us || !io->recv || !buf || want_len == 0u || want_len > cap || timeout_us == 0ULL) {
        return WEB_ISO_PUT_ERR_INVALID;
    }

    const uint64_t start_us = io->now_us(io->ctx);
    size_t got = 0u;

    while (got < want_len) {
        const uint64_t now = io->now_us(io->ctx);
        const uint64_t elapsed = (now >= start_us) ? (now - start_us) : timeout_us;
        if (elapsed >= timeout_us) {
            return WEB_ISO_PUT_ERR_TIMEOUT;
        }

        const uint64_t rem_us = timeout_us - elapsed;
        const uint64_t slice_us = (rem_us < WEB_ISO_PUT_MIN_SLICE_US) ? WEB_ISO_PUT_MIN_SLICE_US : rem_us;
        if (io->set_timeout_us) {
            io->set_timeout_us(io->ctx, slice_us);
        }

        const size_t rem_bytes = want_len - got;
        const int r = io->recv(io->ctx, buf + got, rem_bytes);
        if (r > 0) {
            if ((size_t)r > rem_bytes) {
                return WEB_ISO_PUT_ERR_IO;
            }
            got += (size_t)r;
        } else if (r == WEB_ISO_PUT_IO_TIMEOUT) {
            const uint64_t now2 = io->now_us(io->ctx);
            if (now2 < start_us || (now2 - start_us) >= timeout_us) {
                return WEB_ISO_PUT_ERR_TIMEOUT;
            }
        } else {
            return WEB_ISO_PUT_ERR_IO;
        }
    }

    return WEB_ISO_PUT_OK;
}

int web_iso_progress_step_pct(uint64_t received, uint64_t total, int *last_bucket) {
    if (total == 0ULL || !last_bucket) {
        return -1;
    }

    int pct;
    if (received >= total) {
        pct = 100;
    } else if (received <= UINT64_MAX / 100ULL) {
        pct = (int)((received * 100ULL) / total);
    } else {
        const uint64_t denom = total / 100ULL;
        const uint64_t raw = (denom > 0ULL) ? (received / denom) : 100ULL;
        pct = (raw > 100ULL) ? 100 : (int)raw;
    }

    const int bucket = pct / 5;
    if (bucket <= 0 || bucket <= *last_bucket) {
        return -1;
    }
    *last_bucket = bucket;
    return bucket * 5;
}

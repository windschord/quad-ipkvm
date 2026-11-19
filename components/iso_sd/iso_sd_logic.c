#include "iso_sd_logic.h"

#include <string.h>

int iso_sd_fresult_to_err(int fresult) {
    switch (fresult) {
        case ISO_SD_FR_OK:
            return ISO_OK;
        case ISO_SD_FR_NO_FILE:
        case ISO_SD_FR_NO_PATH:
            return ISO_ERR_NOT_FOUND;
        case ISO_SD_FR_EXIST:
            return ISO_ERR_EXISTS;
        case ISO_SD_FR_INVALID_NAME:
        case ISO_SD_FR_INVALID_PARAMETER:
            return ISO_ERR_INVALID;
        case ISO_SD_FR_INVALID_DRIVE:
        case ISO_SD_FR_NOT_ENABLED:
        case ISO_SD_FR_NOT_READY:
            return ISO_ERR_NO_CARD;
        case ISO_SD_FR_NO_FILESYSTEM:
            return ISO_ERR_UNSUPPORTED_FS;
        case ISO_SD_FR_TIMEOUT:
        case ISO_SD_FR_LOCKED:
            return ISO_ERR_BUSY;
        case ISO_SD_FR_NOT_ENOUGH_CORE:
            return ISO_ERR_NOMEM;
        case ISO_SD_FR_DISK_ERR:
        case ISO_SD_FR_INT_ERR:
        case ISO_SD_FR_DENIED:
        case ISO_SD_FR_INVALID_OBJECT:
        case ISO_SD_FR_WRITE_PROTECTED:
        case ISO_SD_FR_MKFS_ABORTED:
        case ISO_SD_FR_TOO_MANY_OPEN_FILES:
        default:
            return ISO_ERR_IO;
    }
}

static bool valid_volume_path_chars(const char *path, size_t len) {
    if (!path || len == 0u || len >= ISO_PATH_MAX || path[0] != '/') {
        return false;
    }
    for (size_t i = 0u; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 0x20u || c == 0x7Fu || c == '\\') {
            return false;
        }
        /* Reject "." / ".." segments on component boundaries to prevent traversal surprises. */
        if (c == '/' && path[i + 1u] == '.') {
            if (path[i + 2u] == '\0' || path[i + 2u] == '/') {
                return false;
            }
            if (path[i + 2u] == '.' && (path[i + 3u] == '\0' || path[i + 3u] == '/')) {
                return false;
            }
        }
    }
    return true;
}

int iso_sd_format_fatfs_path(char drive_digit, const char *path, char *out, size_t out_cap) {
    if (!out || !path || drive_digit < '0' || drive_digit > '9') {
        return ISO_ERR_INVALID;
    }
    size_t len = strlen(path);
    if (!valid_volume_path_chars(path, len)) {
        return ISO_ERR_INVALID;
    }
    /* "<digit>:" (2 bytes) + path (len) + NUL (1 byte) */
    if (out_cap < len + 3u) {
        return ISO_ERR_INVALID;
    }
    out[0] = drive_digit;
    out[1] = ':';
    memcpy(out + 2, path, len + 1u);
    return ISO_OK;
}

int iso_sd_join_path(const char *dir_path, const char *name, char *out, size_t out_cap) {
    if (!dir_path || !name || !out) {
        return ISO_ERR_INVALID;
    }
    size_t dlen = strlen(dir_path);
    size_t nlen = strlen(name);
    if (!valid_volume_path_chars(dir_path, dlen) || nlen == 0u) {
        return ISO_ERR_INVALID;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return ISO_ERR_INVALID;
    }
    for (size_t i = 0u; i < nlen; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20u || c == 0x7Fu || c == '/' || c == '\\') {
            return ISO_ERR_INVALID;
        }
    }
    bool need_slash = (dir_path[dlen - 1u] != '/');
    size_t total = dlen + (need_slash ? 1u : 0u) + nlen;
    if (total >= ISO_PATH_MAX || out_cap < total + 1u) {
        return ISO_ERR_INVALID;
    }
    memcpy(out, dir_path, dlen);
    size_t pos = dlen;
    if (need_slash) {
        out[pos++] = '/';
    }
    memcpy(out + pos, name, nlen + 1u);
    return ISO_OK;
}

int iso_sd_validate_mount(bool is_fat32, uint32_t sector_size, uint32_t sectors_per_cluster,
                          uint32_t *out_cluster_bytes) {
    if (!is_fat32 || sector_size != ISO_SD_SECTOR_BYTES) {
        return ISO_ERR_UNSUPPORTED_FS;
    }
    if (sectors_per_cluster == 0u || sectors_per_cluster > UINT32_MAX / sector_size) {
        return ISO_ERR_UNSUPPORTED_FS;
    }
    uint32_t cluster_bytes = sector_size * sectors_per_cluster;
    if (cluster_bytes < ISO_SD_MIN_CLUSTER_BYTES) {
        return ISO_ERR_UNSUPPORTED_FS;
    }
    if (out_cluster_bytes) {
        *out_cluster_bytes = cluster_bytes;
    }
    return ISO_OK;
}

int iso_sd_clmt_to_extents(const uint32_t *cltbl, size_t cltbl_cap,
                           uint64_t database_sector, uint32_t sectors_per_cluster,
                           iso_extent_t *out, size_t max_out, size_t *out_n) {
    if (!cltbl || !out_n || sectors_per_cluster == 0u || cltbl_cap < 2u) {
        return ISO_ERR_INVALID;
    }
    if (max_out > 0u && !out) {
        return ISO_ERR_INVALID;
    }

    size_t frags = 0u;
    size_t idx = 1u;
    while (idx < cltbl_cap) {
        uint32_t ncl = cltbl[idx];
        if (ncl == 0u) {
            *out_n = frags;
            return (frags > max_out) ? ISO_ERR_TOO_FRAGMENTED : ISO_OK;
        }
        if (idx + 1u >= cltbl_cap) {
            return ISO_ERR_INVALID;
        }
        uint32_t tcl = cltbl[idx + 1u];
        if (tcl < 2u) {
            return ISO_ERR_INVALID;
        }

        uint64_t rel_cluster = (uint64_t)(tcl - 2u);
        if (rel_cluster > (UINT64_MAX - database_sector) / sectors_per_cluster) {
            return ISO_ERR_INVALID;
        }
        uint64_t start_sector = database_sector + rel_cluster * (uint64_t)sectors_per_cluster;
        uint64_t sector_count = (uint64_t)ncl * (uint64_t)sectors_per_cluster;
        if (sector_count > UINT64_MAX - start_sector) {
            return ISO_ERR_INVALID;
        }

        if (frags < max_out) {
            out[frags].start_sector = start_sector;
            out[frags].sector_count = sector_count;
        }
        frags++;
        idx += 2u;
    }

    /* Missing terminating 0 entry in cltbl. */
    return ISO_ERR_INVALID;
}

void iso_sd_state_init(iso_sd_ctrl_state_t *st) {
    if (!st) {
        return;
    }
    atomic_init(&st->epoch, 1u);
    atomic_init(&st->sd_state, (int)ISO_SD_NO_CARD);
    atomic_init(&st->io_fault, false);
}

uint32_t iso_sd_state_epoch(const iso_sd_ctrl_state_t *st) {
    if (!st) {
        return 0u;
    }
    return atomic_load_explicit(&st->epoch, memory_order_acquire);
}

uint32_t iso_sd_state_epoch_bump(iso_sd_ctrl_state_t *st) {
    if (!st) {
        return 0u;
    }
    uint32_t cur = atomic_load_explicit(&st->epoch, memory_order_relaxed);
    for (;;) {
        uint32_t next = cur + 1u;
        if (next == 0u) {
            next = 1u;
        }
        if (atomic_compare_exchange_weak_explicit(&st->epoch, &cur, next,
                                                  memory_order_acq_rel,
                                                  memory_order_relaxed)) {
            return next;
        }
    }
}

iso_sd_state_t iso_sd_state_get(const iso_sd_ctrl_state_t *st) {
    if (!st) {
        return ISO_SD_NO_CARD;
    }
    return (iso_sd_state_t)atomic_load_explicit(&st->sd_state, memory_order_acquire);
}

void iso_sd_state_set(iso_sd_ctrl_state_t *st, iso_sd_state_t s) {
    if (!st) {
        return;
    }
    atomic_store_explicit(&st->sd_state, (int)s, memory_order_release);
}

void iso_sd_state_latch_io_fault(iso_sd_ctrl_state_t *st) {
    if (!st) {
        return;
    }
    atomic_store_explicit(&st->io_fault, true, memory_order_release);
}

bool iso_sd_state_take_io_fault(iso_sd_ctrl_state_t *st) {
    if (!st) {
        return false;
    }
    return atomic_exchange_explicit(&st->io_fault, false, memory_order_acq_rel);
}

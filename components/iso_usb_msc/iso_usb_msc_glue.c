#include "iso_usb_msc_glue.h"

#include <string.h>

static void be_lock(iso_usb_msc_extent_backend_t *be) {
    if (be->lock != NULL) {
        be->lock(be->lock_ctx);
    }
}

static void be_unlock(iso_usb_msc_extent_backend_t *be) {
    if (be->unlock != NULL) {
        be->unlock(be->lock_ctx);
    }
}

static mmc_dir_t map_bot_dir_to_mmc(msc_bot_dir_t dir) {
    switch (dir) {
    case MSC_BOT_DIR_IN:
        return MMC_DIR_IN;
    case MSC_BOT_DIR_OUT:
        return MMC_DIR_OUT;
    case MSC_BOT_DIR_NONE:
    default:
        return MMC_DIR_NONE;
    }
}

void iso_usb_msc_exec_cmd(mmc_scsi_t *scsi, const msc_bot_cmd_t *cmd, msc_bot_cmd_result_t *out_res) {
    if (scsi == NULL || cmd == NULL || out_res == NULL) {
        return;
    }
    memset(out_res, 0, sizeof(*out_res));
    out_res->seq = cmd->seq;

    switch (cmd->kind) {
    case MSC_BOT_CMD_EXECUTE: {
        mmc_result_t mr;
        memset(&mr, 0, sizeof(mr));
        mmc_scsi_execute(scsi, cmd->cdb, cmd->cdb_len, map_bot_dir_to_mmc(cmd->dir),
                         cmd->expected_len, cmd->out, cmd->out_cap, &mr);
        out_res->good = (mr.status == MMC_STATUS_GOOD);
        out_res->is_read = mr.read.is_read;
        out_res->read_lba = mr.read.lba;
        out_res->read_blocks = mr.read.blocks;
        out_res->data_len = mr.data_len;
        /* mmc_result_t omits data direction: valid returned data is always IN (WRITE OUT fails). */
        if (mr.read.is_read) {
            out_res->dev_dir = (mr.read.blocks > 0u) ? MSC_BOT_DIR_IN : MSC_BOT_DIR_NONE;
        } else {
            out_res->dev_dir = (mr.data_len > 0u) ? MSC_BOT_DIR_IN : MSC_BOT_DIR_NONE;
        }
        break;
    }
    case MSC_BOT_CMD_READ_BLOCKS: {
        if (cmd->lba > UINT32_MAX) {
            out_res->good = false;
            return;
        }
        const mmc_status_t st =
            mmc_scsi_read_blocks(scsi, (uint32_t)cmd->lba, cmd->blocks, cmd->buf, cmd->cap);
        out_res->good = (st == MMC_STATUS_GOOD);
        break;
    }
    default:
        out_res->good = false;
        break;
    }
}

static int be_size_bytes(void *ctx, uint32_t media_id, uint64_t *out) {
    iso_usb_msc_extent_backend_t *be = (iso_usb_msc_extent_backend_t *)ctx;
    if (be == NULL || out == NULL) {
        return ISO_ERR_INVALID;
    }
    be_lock(be);
    if (be->table == NULL || be->media_id != media_id) {
        be_unlock(be);
        return ISO_ERR_NOT_FOUND;
    }
    *out = iso_extent_table_size_bytes(be->table);
    be_unlock(be);
    return ISO_OK;
}

static int be_acquire(void *ctx, uint32_t media_id, void **handle) {
    iso_usb_msc_extent_backend_t *be = (iso_usb_msc_extent_backend_t *)ctx;
    if (be == NULL || handle == NULL) {
        return ISO_ERR_INVALID;
    }
    *handle = NULL;
    be_lock(be);
    if (be->table == NULL || be->media_id != media_id) {
        be_unlock(be);
        return ISO_ERR_NOT_FOUND;
    }
    iso_extent_table_ref(be->table);
    *handle = be->table;
    be_unlock(be);
    return ISO_OK;
}

static int be_read(void *ctx, void *handle, uint32_t lba, uint32_t blocks, uint8_t *buf) {
    iso_usb_msc_extent_backend_t *be = (iso_usb_msc_extent_backend_t *)ctx;
    if (be == NULL || handle == NULL) {
        return ISO_ERR_INVALID;
    }
    if (blocks == 0u) {
        return ISO_OK;
    }
    if (buf == NULL) {
        return ISO_ERR_INVALID;
    }

    be_lock(be);
    const iso_blockdev_t *bdev = be->bdev;
    be_unlock(be);

    if (bdev == NULL || bdev->read_sectors == NULL) {
        return ISO_ERR_NO_CARD;
    }

    const iso_extent_table_t *tbl = (const iso_extent_table_t *)handle;
    const uint32_t epoch = iso_extent_table_epoch(tbl);
    uint32_t cur_lba = lba;
    uint32_t rem = blocks;
    uint8_t *out_ptr = buf;

    while (rem > 0u) {
        iso_extent_t spans[64];
        size_t n_spans = 0u;
        uint32_t step = rem;

        for (;;) {
            const int rc = iso_extent_map(tbl, (uint64_t)cur_lba, step, spans,
                                          sizeof(spans) / sizeof(spans[0]), &n_spans);
            if (rc == ISO_EXT_ERR_SPANS_FULL && step > 1u) {
                step /= 2u;
                continue;
            }
            if (rc != ISO_EXT_OK) {
                return ISO_ERR_IO;
            }
            break;
        }

        const size_t step_bytes = (size_t)step * ISO_CD_BLOCK_BYTES;
        uint8_t *dst = out_ptr;
        size_t bytes_done = 0u;

        for (size_t i = 0u; i < n_spans; i++) {
            const size_t span_bytes = (size_t)spans[i].sector_count * ISO_SECTOR_BYTES;
            if (bytes_done + span_bytes > step_bytes) {
                return ISO_ERR_IO;
            }
            const int rc = bdev->read_sectors(bdev->ctx, epoch, spans[i].start_sector,
                                              spans[i].sector_count, dst);
            if (rc != ISO_OK) {
                return rc;
            }
            dst += span_bytes;
            bytes_done += span_bytes;
        }

        const uint64_t valid = iso_extent_valid_bytes(tbl, (uint64_t)cur_lba, step);
        if (valid < step_bytes) {
            memset(out_ptr + (size_t)valid, 0, step_bytes - (size_t)valid);
        }

        cur_lba += step;
        rem -= step;
        out_ptr += step_bytes;
    }

    return ISO_OK;
}

static void be_release(void *ctx, void *handle) {
    (void)ctx;
    if (handle != NULL) {
        iso_extent_table_unref((iso_extent_table_t *)handle);
    }
}

void iso_usb_msc_extent_backend_init(iso_usb_msc_extent_backend_t *be,
                                     const iso_blockdev_t *bdev,
                                     void (*lock)(void *ctx),
                                     void (*unlock)(void *ctx),
                                     void *lock_ctx) {
    if (be == NULL) {
        return;
    }
    memset(be, 0, sizeof(*be));
    be->bdev = bdev;
    be->lock = lock;
    be->unlock = unlock;
    be->lock_ctx = lock_ctx;
}

void iso_usb_msc_extent_backend_set_blockdev(iso_usb_msc_extent_backend_t *be,
                                             const iso_blockdev_t *bdev) {
    if (be == NULL) {
        return;
    }
    be_lock(be);
    be->bdev = bdev;
    be_unlock(be);
}

void iso_usb_msc_extent_backend_set_image(iso_usb_msc_extent_backend_t *be,
                                          uint32_t media_id,
                                          iso_extent_table_t *table) {
    if (be == NULL) {
        return;
    }
    if (table != NULL) {
        iso_extent_table_ref(table);
    }
    be_lock(be);
    iso_extent_table_t *old = be->table;
    be->table = table;
    be->media_id = (table != NULL) ? media_id : 0u;
    be_unlock(be);

    if (old != NULL) {
        iso_extent_table_unref(old);
    }
}

void iso_usb_msc_extent_backend_clear(iso_usb_msc_extent_backend_t *be) {
    iso_usb_msc_extent_backend_set_image(be, 0u, NULL);
}

mmc_scsi_backend_t iso_usb_msc_extent_backend_as_scsi(iso_usb_msc_extent_backend_t *be) {
    mmc_scsi_backend_t out = {
        .ctx = be,
        .size_bytes = be_size_bytes,
        .acquire = be_acquire,
        .read = be_read,
        .release = be_release,
    };
    return out;
}

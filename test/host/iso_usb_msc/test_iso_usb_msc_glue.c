#include "iso_usb_msc_glue.h"
#include "unity.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- Mock allocator & raw SD block device ---- */

static void *test_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void test_free(void *ctx, void *p) {
    (void)ctx;
    free(p);
}

static const iso_allocator_t s_alloc = {
    .ctx = NULL,
    .alloc = test_alloc,
    .free = test_free,
};

#define MOCK_SD_SECTORS 512u

typedef struct {
    uint8_t sectors[MOCK_SD_SECTORS][ISO_SECTOR_BYTES];
    uint32_t current_epoch;
    int fail_after_reads;
    int fail_rc;
    uint32_t read_calls;
} mock_bdev_t;

static int mock_bdev_read_sectors(void *ctx,
                                  uint32_t expected_epoch,
                                  uint64_t lba,
                                  uint32_t sector_count,
                                  uint8_t *dst) {
    mock_bdev_t *b = (mock_bdev_t *)ctx;
    b->read_calls++;
    if (expected_epoch != b->current_epoch) {
        return ISO_ERR_EPOCH;
    }
    if (b->fail_after_reads >= 0) {
        if (b->fail_after_reads == 0) {
            return b->fail_rc;
        }
        b->fail_after_reads--;
    }
    if (lba + sector_count > MOCK_SD_SECTORS) {
        return ISO_ERR_IO;
    }
    uint8_t *out = (uint8_t *)dst;
    for (uint32_t i = 0; i < sector_count; i++) {
        memcpy(out + (size_t)i * ISO_SECTOR_BYTES, b->sectors[lba + i], ISO_SECTOR_BYTES);
    }
    return ISO_OK;
}

static uint32_t mock_bdev_epoch(void *ctx) {
    return ((const mock_bdev_t *)ctx)->current_epoch;
}

/* ---- Deterministic host + BOT harness ---- */

typedef struct {
    msc_bot_t bot;
    mmc_scsi_t scsi;
    iso_media_t media;
    iso_usb_msc_extent_backend_t backend;
    pthread_mutex_t backend_mutex;

    uint8_t cbw[MSC_BOT_CBW_BUF_SIZE];
    uint8_t csw[MSC_BOT_CSW_BUF_SIZE];
    uint8_t *buf[2];
    uint32_t buf_size;

    bool in_pending;
    const uint8_t *in_ptr;
    uint32_t in_len;

    bool out_pending;
    uint8_t *out_ptr;
    uint32_t out_len;

    bool in_stalled;
    bool out_stalled;
    uint32_t reset_done_count;
    uint32_t reset_failed_count;
    uint32_t cancel_count;
    uint32_t command_done_count;

    bool hold_worker;
    bool cmd_pending;
    msc_bot_cmd_t pending_cmd;

    uint8_t rx_data[64u * 1024u];
    uint32_t rx_len;
    bool csw_valid;
    uint32_t csw_sig;
    uint32_t csw_tag;
    uint32_t csw_residue;
    uint8_t csw_status;
} harness_t;

static void harness_be_lock(void *ctx) {
    pthread_mutex_lock((pthread_mutex_t *)ctx);
}

static void harness_be_unlock(void *ctx) {
    pthread_mutex_unlock((pthread_mutex_t *)ctx);
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static int h_xfer_in(void *ctx, const uint8_t *buf, uint32_t len) {
    harness_t *h = (harness_t *)ctx;
    TEST_ASSERT_FALSE(h->in_pending);
    h->in_pending = true;
    h->in_ptr = buf;
    h->in_len = len;
    return 0;
}

static int h_xfer_out(void *ctx, uint8_t *buf, uint32_t len) {
    harness_t *h = (harness_t *)ctx;
    TEST_ASSERT_FALSE(h->out_pending);
    h->out_pending = true;
    h->out_ptr = buf;
    h->out_len = len;
    return 0;
}

static void h_stall_in(void *ctx) {
    harness_t *h = (harness_t *)ctx;
    h->in_stalled = true;
}

static void h_stall_out(void *ctx) {
    harness_t *h = (harness_t *)ctx;
    h->out_stalled = true;
}

static void h_unstall(void *ctx, msc_bot_ep_t ep) {
    harness_t *h = (harness_t *)ctx;
    if (ep == MSC_BOT_EP_IN) {
        h->in_stalled = false;
    } else {
        h->out_stalled = false;
    }
}

static void h_cancel_xfers(void *ctx) {
    harness_t *h = (harness_t *)ctx;
    h->cancel_count++;
    h->in_pending = false;
    h->out_pending = false;
}

static int h_submit_command(void *ctx, const msc_bot_cmd_t *cmd) {
    harness_t *h = (harness_t *)ctx;
    TEST_ASSERT_FALSE(h->cmd_pending);
    h->cmd_pending = true;
    h->pending_cmd = *cmd;
    return 0;
}

static void h_command_done(void *ctx) {
    harness_t *h = (harness_t *)ctx;
    h->command_done_count++;
    mmc_scsi_command_done(&h->scsi);
    iso_usb_msc_sync_media_backend(&h->media, &h->backend);
}

static void h_reset_done(void *ctx) {
    ((harness_t *)ctx)->reset_done_count++;
}

static void h_reset_failed(void *ctx) {
    ((harness_t *)ctx)->reset_failed_count++;
}

static void harness_init(harness_t *h, const iso_blockdev_t *bdev, uint32_t buf_size) {
    memset(h, 0, sizeof(*h));
    pthread_mutex_init(&h->backend_mutex, NULL);
    h->buf_size = buf_size;
    h->buf[0] = (uint8_t *)malloc(buf_size);
    h->buf[1] = (uint8_t *)malloc(buf_size);
    TEST_ASSERT_NOT_NULL(h->buf[0]);
    TEST_ASSERT_NOT_NULL(h->buf[1]);

    iso_media_init(&h->media, NULL, NULL);
    iso_usb_msc_extent_backend_init(&h->backend, bdev, harness_be_lock, harness_be_unlock,
                                    &h->backend_mutex);
    const mmc_scsi_backend_t scsi_be = iso_usb_msc_extent_backend_as_scsi(&h->backend);
    TEST_ASSERT_EQUAL_INT(0, mmc_scsi_init(&h->scsi, &h->media, &scsi_be));

    const msc_bot_cfg_t cfg = {
        .ops = {
            .ctx = h,
            .xfer_in = h_xfer_in,
            .xfer_out = h_xfer_out,
            .stall_in = h_stall_in,
            .stall_out = h_stall_out,
            .unstall = h_unstall,
            .cancel_xfers = h_cancel_xfers,
            .submit_command = h_submit_command,
            .command_done = h_command_done,
            .reset_done = h_reset_done,
            .reset_failed = h_reset_failed,
        },
        .cbw_buf = h->cbw,
        .csw_buf = h->csw,
        .buf = {h->buf[0], h->buf[1]},
        .buf_size = buf_size,
        .out_stall = true,
    };
    TEST_ASSERT_EQUAL_INT(MSC_BOT_OK, msc_bot_init(&h->bot, &cfg));
    msc_bot_attach(&h->bot);
}

static void harness_destroy(harness_t *h) {
    iso_usb_msc_extent_backend_clear(&h->backend);
    free(h->buf[0]);
    free(h->buf[1]);
    pthread_mutex_destroy(&h->backend_mutex);
}

static void pump_worker_once(harness_t *h) {
    TEST_ASSERT_TRUE(h->cmd_pending);
    h->cmd_pending = false;
    msc_bot_cmd_result_t res;
    iso_usb_msc_exec_cmd(&h->scsi, &h->pending_cmd, &res);
    if (h->pending_cmd.kind == MSC_BOT_CMD_EXECUTE) {
        iso_usb_msc_sync_media_backend(&h->media, &h->backend);
    }
    msc_bot_command_complete(&h->bot, &res);
}

static void host_send_cbw(harness_t *h, uint32_t tag, uint32_t xfer_len, uint8_t flags,
                          const uint8_t *cdb, uint8_t cdb_len) {
    TEST_ASSERT_TRUE(h->out_pending);
    TEST_ASSERT_EQUAL_UINT32(MSC_BOT_CBW_LEN, h->out_len);
    uint8_t *p = h->out_ptr;
    memset(p, 0, MSC_BOT_CBW_LEN);
    wr32le(p + 0, MSC_BOT_CBW_SIGNATURE);
    wr32le(p + 4, tag);
    wr32le(p + 8, xfer_len);
    p[12] = flags;
    p[13] = 0u; /* LUN 0 */
    p[14] = cdb_len;
    memcpy(p + 15, cdb, cdb_len);
    h->out_pending = false;
    h->rx_len = 0u;
    h->csw_valid = false;
    msc_bot_xfer_complete(&h->bot, MSC_BOT_EP_OUT, MSC_BOT_CBW_LEN, true);
}

static void host_run_bot_transaction(harness_t *h, uint32_t tag, uint32_t host_len, uint8_t flags,
                                     const uint8_t *cdb, uint8_t cdb_len,
                                     const uint8_t *out_data, uint32_t out_data_len) {
    host_send_cbw(h, tag, host_len, flags, cdb, cdb_len);

    uint32_t out_off = 0u;
    for (int guard = 0; guard < 128; guard++) {
        if (!h->hold_worker && h->cmd_pending) {
            pump_worker_once(h);
            continue;
        }
        if (h->in_stalled) {
            h->in_stalled = false;
            msc_bot_halt_cleared(&h->bot, MSC_BOT_EP_IN);
            continue;
        }
        if (h->out_stalled) {
            h->out_stalled = false;
            msc_bot_halt_cleared(&h->bot, MSC_BOT_EP_OUT);
            continue;
        }
        if (msc_bot_state(&h->bot) == MSC_BOT_STATUS_SEND && h->in_pending) {
            TEST_ASSERT_EQUAL_UINT32(MSC_BOT_CSW_LEN, h->in_len);
            h->csw_sig = rd32le(h->in_ptr + 0);
            h->csw_tag = rd32le(h->in_ptr + 4);
            h->csw_residue = rd32le(h->in_ptr + 8);
            h->csw_status = h->in_ptr[12];
            h->csw_valid = true;
            h->in_pending = false;
            msc_bot_xfer_complete(&h->bot, MSC_BOT_EP_IN, MSC_BOT_CSW_LEN, true);
            TEST_ASSERT_EQUAL_UINT32(MSC_BOT_CSW_SIGNATURE, h->csw_sig);
            TEST_ASSERT_EQUAL_UINT32(tag, h->csw_tag);
            TEST_ASSERT_EQUAL_INT(MSC_BOT_IDLE, msc_bot_state(&h->bot));
            return;
        }
        if (h->in_pending) {
            TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof(h->rx_data) - h->rx_len, h->in_len);
            memcpy(h->rx_data + h->rx_len, h->in_ptr, h->in_len);
            h->rx_len += h->in_len;
            const uint32_t got = h->in_len;
            h->in_pending = false;
            msc_bot_xfer_complete(&h->bot, MSC_BOT_EP_IN, got, true);
            continue;
        }
        if (msc_bot_state(&h->bot) == MSC_BOT_DATA_OUT && h->out_pending) {
            const uint32_t chunk =
                (out_data_len - out_off < h->out_len) ? (out_data_len - out_off) : h->out_len;
            if (chunk > 0u) {
                memcpy(h->out_ptr, out_data + out_off, chunk);
                out_off += chunk;
            }
            h->out_pending = false;
            msc_bot_xfer_complete(&h->bot, MSC_BOT_EP_OUT, chunk, true);
            continue;
        }
        if (h->hold_worker) {
            return;
        }
        TEST_FAIL_MESSAGE("BOT state machine stalled in harness");
    }
    TEST_FAIL_MESSAGE("BOT transaction exceeded step limit");
}

static void host_request_sense(harness_t *h, uint32_t tag, uint8_t *out_key, uint8_t *out_asc,
                               uint8_t *out_ascq) {
    uint8_t cdb[6] = {0x03u, 0u, 0u, 0u, 18u, 0u};
    host_run_bot_transaction(h, tag, 18u, 0x80u, cdb, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h->csw_status);
    TEST_ASSERT_EQUAL_UINT32(18u, h->rx_len);
    *out_key = (uint8_t)(h->rx_data[2] & 0x0Fu);
    *out_asc = h->rx_data[12];
    *out_ascq = h->rx_data[13];
}

void setUp(void) {}
void tearDown(void) {}

void test_inquiry_and_tur_empty(void) {
    mock_bdev_t raw = {.current_epoch = 1u, .fail_after_reads = -1};
    iso_blockdev_t bdev = {
        .ctx = &raw,
        .read_sectors = mock_bdev_read_sectors,
        .epoch = mock_bdev_epoch,
    };
    harness_t h;
    harness_init(&h, &bdev, 4096u);

    uint8_t inq[6] = {0x12u, 0u, 0u, 0u, 36u, 0u};
    host_run_bot_transaction(&h, 1u, 36u, 0x80u, inq, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_EQUAL_UINT32(36u, h.rx_len);
    TEST_ASSERT_EQUAL_UINT8(0x05u, h.rx_data[0] & 0x1Fu); /* CD/DVD-ROM */
    TEST_ASSERT_TRUE((h.rx_data[1] & 0x80u) != 0u);       /* RMB */

    uint8_t tur[6] = {0x00u, 0u, 0u, 0u, 0u, 0u};
    host_run_bot_transaction(&h, 2u, 0u, 0x00u, tur, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_FAILED, h.csw_status);

    uint8_t key = 0u, asc = 0u, ascq = 0u;
    host_request_sense(&h, 3u, &key, &asc, &ascq);
    TEST_ASSERT_EQUAL_HEX8(0x02u, key);  /* NOT READY */
    TEST_ASSERT_EQUAL_HEX8(0x3Au, asc);  /* MEDIUM NOT PRESENT */
    TEST_ASSERT_EQUAL_HEX8(0x00u, ascq);

    harness_destroy(&h);
}

void test_mount_read10_read12_and_non2048_tail(void) {
    mock_bdev_t raw = {.current_epoch = 7u, .fail_after_reads = -1};
    /* Part 0 = 3 blocks = 12 sectors (extents: [10..15] (6), [40..45] (6))
     * Part 1 = 2*2048 + 300 = 4396B = 9 sectors (extents: [80..83] (4), [100..104] (5)) */
    const uint64_t total_size = 5u * 2048u + 300u; /* 10540 bytes */
    uint8_t expected[6u * 2048u];
    memset(expected, 0, sizeof(expected));
    for (uint64_t i = 0; i < total_size; i++) {
        expected[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
    }

    const iso_extent_t p0_ext[2] = {{10u, 6u}, {40u, 6u}};
    const iso_extent_t p1_ext[2] = {{80u, 4u}, {100u, 5u}};
    /* Fill SD sectors for both parts, plus non-zero garbage in EOF sector tail [300..511]. */
    memcpy(raw.sectors[10], expected + 0u * 512u, 6u * 512u);
    memcpy(raw.sectors[40], expected + 6u * 512u, 6u * 512u);
    memcpy(raw.sectors[80], expected + 12u * 512u, 4u * 512u);
    memcpy(raw.sectors[100], expected + 16u * 512u, 5u * 512u);
    memset(&raw.sectors[104][300], 0xAA, ISO_SECTOR_BYTES - 300u);

    const iso_part_input_t parts[2] = {
        {.file_bytes = 6144u, .extents = p0_ext, .n_extents = 2u},
        {.file_bytes = 4396u, .extents = p1_ext, .n_extents = 2u},
    };
    iso_extent_table_t *tbl = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK,
                          iso_extent_table_build(&s_alloc, 7u, total_size, 3u, parts, 2u, &tbl));

    iso_blockdev_t bdev = {
        .ctx = &raw,
        .read_sectors = mock_bdev_read_sectors,
        .epoch = mock_bdev_epoch,
    };
    harness_t h;
    harness_init(&h, &bdev, 4096u); /* 2-block ping-pong buffer */

    iso_usb_msc_extent_backend_set_image(&h.backend, 0xCAFE0001u, tbl);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&h.media, 0xCAFE0001u, false));
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(tbl));

    /* First TUR sees Unit Attention 06h/28h/00h */
    uint8_t tur[6] = {0x00u, 0u, 0u, 0u, 0u, 0u};
    host_run_bot_transaction(&h, 10u, 0u, 0x00u, tur, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_FAILED, h.csw_status);
    uint8_t key = 0u, asc = 0u, ascq = 0u;
    host_request_sense(&h, 11u, &key, &asc, &ascq);
    TEST_ASSERT_EQUAL_HEX8(0x06u, key);
    TEST_ASSERT_EQUAL_HEX8(0x28u, asc);
    TEST_ASSERT_EQUAL_HEX8(0x00u, ascq);

    /* Second TUR is GOOD */
    host_run_bot_transaction(&h, 12u, 0u, 0x00u, tur, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);

    /* READ CAPACITY(10) */
    uint8_t rcap[10] = {0x25u};
    host_run_bot_transaction(&h, 13u, 8u, 0x80u, rcap, 10u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_EQUAL_UINT32(8u, h.rx_len);
    TEST_ASSERT_EQUAL_UINT32(5u, rd32be(&h.rx_data[0]));    /* last LBA = 6 - 1 */
    TEST_ASSERT_EQUAL_UINT32(2048u, rd32be(&h.rx_data[4])); /* block size */

    /* READ(10) LBA=0, blocks=6 across 3 ping-pong chunks + zero-padded EOF block */
    uint8_t rd10[10] = {0x28u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 6u, 0u};
    host_run_bot_transaction(&h, 14u, 6u * 2048u, 0x80u, rd10, 10u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_EQUAL_UINT32(0u, h.csw_residue);
    TEST_ASSERT_EQUAL_UINT32(6u * 2048u, h.rx_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, h.rx_data, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(tbl));

    /* READ(12) LBA=2, blocks=3 spanning Part 0 -> Part 1 and multiple extents */
    uint8_t rd12[12] = {0xA8u, 0u, 0u, 0u, 0u, 2u, 0u, 0u, 0u, 3u, 0u, 0u};
    host_run_bot_transaction(&h, 15u, 3u * 2048u, 0x80u, rd12, 12u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_EQUAL_UINT32(3u * 2048u, h.rx_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected + 2u * 2048u, h.rx_data, 3u * 2048u);
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(tbl));

    harness_destroy(&h);
    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(tbl));
}

void test_mode_sense_toc_get_cfg_and_write_protect(void) {
    mock_bdev_t raw = {.current_epoch = 1u, .fail_after_reads = -1};
    uint8_t img[4096];
    memset(img, 0x5Au, sizeof(img));
    memcpy(raw.sectors[0], img, sizeof(img));
    const iso_extent_t ext = {0u, 8u};
    const iso_part_input_t part = {.file_bytes = sizeof(img), .extents = &ext, .n_extents = 1u};
    iso_extent_table_t *tbl = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK,
                          iso_extent_table_build(&s_alloc, 1u, sizeof(img), 4u, &part, 1u, &tbl));

    iso_blockdev_t bdev = {
        .ctx = &raw,
        .read_sectors = mock_bdev_read_sectors,
        .epoch = mock_bdev_epoch,
    };
    harness_t h;
    harness_init(&h, &bdev, 4096u);
    iso_usb_msc_extent_backend_set_image(&h.backend, 1u, tbl);
    iso_media_mount(&h.media, 1u, false);

    /* Clear UA */
    uint8_t tur[6] = {0x00u};
    host_run_bot_transaction(&h, 20u, 0u, 0x00u, tur, 6u, NULL, 0u);
    uint8_t k = 0u, a = 0u, q = 0u;
    host_request_sense(&h, 21u, &k, &a, &q);

    /* MODE SENSE(6) all pages */
    uint8_t ms6[6] = {0x1Au, 0u, 0x3Fu, 0u, 64u, 0u};
    host_run_bot_transaction(&h, 22u, 64u, 0x80u, ms6, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_TRUE(h.rx_len >= 4u);

    /* READ TOC */
    uint8_t toc[10] = {0x43u, 0x02u, 0u, 0u, 0u, 0u, 0u, 0u, 20u, 0u};
    host_run_bot_transaction(&h, 23u, 20u, 0x80u, toc, 10u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_TRUE(h.rx_len >= 12u);

    /* GET CONFIGURATION */
    uint8_t gcfg[10] = {0x46u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 16u, 0u};
    host_run_bot_transaction(&h, 24u, 16u, 0x80u, gcfg, 10u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_TRUE(h.rx_len >= 8u);

    /* WRITE(10) -> STALL Bulk-OUT + CSW FAILED + DATA PROTECT 07h/27h/00h */
    uint8_t wr10[10] = {0x2Au, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 0u};
    uint8_t payload[2048];
    memset(payload, 0xCC, sizeof(payload));
    host_run_bot_transaction(&h, 25u, 2048u, 0x00u, wr10, 10u, payload, sizeof(payload));
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_FAILED, h.csw_status);
    host_request_sense(&h, 26u, &k, &a, &q);
    TEST_ASSERT_EQUAL_HEX8(0x07u, k);
    TEST_ASSERT_EQUAL_HEX8(0x27u, a);
    TEST_ASSERT_EQUAL_HEX8(0x00u, q);

    harness_destroy(&h);
    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(tbl));
}

void test_mid_read_epoch_error_reports_medium_error_and_releases_ref(void) {
    mock_bdev_t raw = {.current_epoch = 3u, .fail_after_reads = 1, .fail_rc = ISO_ERR_EPOCH};
    const iso_extent_t ext = {0u, 24u};
    const iso_part_input_t part = {.file_bytes = 6u * 2048u, .extents = &ext, .n_extents = 1u};
    iso_extent_table_t *tbl = NULL;
    TEST_ASSERT_EQUAL_INT(
        ISO_EXT_OK, iso_extent_table_build(&s_alloc, 3u, 6u * 2048u, 6u, &part, 1u, &tbl));

    iso_blockdev_t bdev = {
        .ctx = &raw,
        .read_sectors = mock_bdev_read_sectors,
        .epoch = mock_bdev_epoch,
    };
    harness_t h;
    harness_init(&h, &bdev, 4096u);
    iso_usb_msc_extent_backend_set_image(&h.backend, 0x42u, tbl);
    iso_media_mount(&h.media, 0x42u, false);

    uint8_t tur[6] = {0x00u};
    host_run_bot_transaction(&h, 30u, 0u, 0x00u, tur, 6u, NULL, 0u);
    uint8_t k = 0u, a = 0u, q = 0u;
    host_request_sense(&h, 31u, &k, &a, &q);

    uint8_t rd10[10] = {0x28u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 6u, 0u};
    host_run_bot_transaction(&h, 32u, 6u * 2048u, 0x80u, rd10, 10u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_FAILED, h.csw_status);
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(tbl)); /* no leak! */

    host_request_sense(&h, 33u, &k, &a, &q);
    TEST_ASSERT_EQUAL_HEX8(0x03u, k); /* MEDIUM ERROR */
    TEST_ASSERT_EQUAL_HEX8(0x11u, a);
    TEST_ASSERT_EQUAL_HEX8(0x00u, q);

    harness_destroy(&h);
    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(tbl));
}

void test_mid_command_bus_reset_defers_reset_done_and_reports_ua_29(void) {
    mock_bdev_t raw = {.current_epoch = 1u, .fail_after_reads = -1};
    for (uint32_t s = 0; s < 24u; s++) {
        memset(raw.sectors[s], (int)(0x30u + s), ISO_SECTOR_BYTES);
    }
    const iso_extent_t ext = {0u, 24u};
    const iso_part_input_t part = {.file_bytes = 6u * 2048u, .extents = &ext, .n_extents = 1u};
    iso_extent_table_t *tbl = NULL;
    TEST_ASSERT_EQUAL_INT(
        ISO_EXT_OK, iso_extent_table_build(&s_alloc, 1u, 6u * 2048u, 6u, &part, 1u, &tbl));

    iso_blockdev_t bdev = {
        .ctx = &raw,
        .read_sectors = mock_bdev_read_sectors,
        .epoch = mock_bdev_epoch,
    };
    harness_t h;
    harness_init(&h, &bdev, 4096u);
    iso_usb_msc_extent_backend_set_image(&h.backend, 0x99u, tbl);
    iso_media_mount(&h.media, 0x99u, false);

    uint8_t tur[6] = {0x00u};
    host_run_bot_transaction(&h, 40u, 0u, 0x00u, tur, 6u, NULL, 0u);
    uint8_t k = 0u, a = 0u, q = 0u;
    host_request_sense(&h, 41u, &k, &a, &q);

    /* Send READ(10) and stop while READ_BLOCKS is outstanding */
    h.hold_worker = true;
    uint8_t rd10[10] = {0x28u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 6u, 0u};
    host_send_cbw(&h, 42u, 6u * 2048u, 0x80u, rd10, 10u);
    pump_worker_once(&h); /* finishes EXECUTE -> dispatches READ_BLOCKS which stays cmd_pending */
    TEST_ASSERT_TRUE(h.cmd_pending);
    TEST_ASSERT_EQUAL_UINT32(MSC_BOT_CMD_READ_BLOCKS, h.pending_cmd.kind);
    TEST_ASSERT_EQUAL_UINT32(3u, iso_extent_table_refs(tbl)); /* SCSI holds 1 ref */

    /* Bus reset + BOT Reset while READ_BLOCKS is running -> reset_done is deferred! */
    iso_media_bus_reset(&h.media);
    msc_bot_reset(&h.bot);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_RESET_WAIT, msc_bot_state(&h.bot));
    TEST_ASSERT_EQUAL_UINT32(0u, h.reset_done_count);

    /* Finish in-flight READ_BLOCKS -> command_done runs, reset_done fires, CBW re-armed */
    h.hold_worker = false;
    pump_worker_once(&h);
    TEST_ASSERT_EQUAL_UINT32(1u, h.reset_done_count);
    TEST_ASSERT_EQUAL_UINT32(0u, h.reset_failed_count);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_IDLE, msc_bot_state(&h.bot));
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(tbl));

    /* Next TUR reports Unit Attention 06h/29h/00h */
    host_run_bot_transaction(&h, 43u, 0u, 0x00u, tur, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_FAILED, h.csw_status);
    host_request_sense(&h, 44u, &k, &a, &q);
    TEST_ASSERT_EQUAL_HEX8(0x06u, k);
    TEST_ASSERT_EQUAL_HEX8(0x29u, a);
    TEST_ASSERT_EQUAL_HEX8(0x00u, q);

    /* Subsequent TUR + READ(10) succeed cleanly */
    host_run_bot_transaction(&h, 45u, 0u, 0x00u, tur, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    host_run_bot_transaction(&h, 46u, 6u * 2048u, 0x80u, rd10, 10u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_EQUAL_UINT32(6u * 2048u, h.rx_len);

    harness_destroy(&h);
    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(tbl));
}

void test_highly_fragmented_spans_full_fallback(void) {
    mock_bdev_t raw = {.current_epoch = 1u, .fail_after_reads = -1};
    /* 18 blocks * 4 single-sector fragmented extents = 72 spans (>64 span scratch buffer) */
    const uint32_t n_blocks = 18u;
    const uint32_t n_sectors = n_blocks * ISO_SECTORS_PER_BLOCK; /* 72 */
    iso_extent_t exts[72];
    uint8_t expected[18u * ISO_CD_BLOCK_BYTES];

    for (uint32_t i = 0; i < n_sectors; i++) {
        const uint64_t sd_sec = (uint64_t)i * 2u; /* spaced by 2 -> forces 72 separate spans */
        exts[i].start_sector = sd_sec;
        exts[i].sector_count = 1u;
        for (uint32_t b = 0; b < ISO_SECTOR_BYTES; b++) {
            const uint8_t v = (uint8_t)((i * 19u + b * 7u) & 0xFFu);
            raw.sectors[sd_sec][b] = v;
            expected[(size_t)i * ISO_SECTOR_BYTES + b] = v;
        }
    }

    const iso_part_input_t part = {
        .file_bytes = sizeof(expected),
        .extents = exts,
        .n_extents = n_sectors,
    };
    iso_extent_table_t *tbl = NULL;
    TEST_ASSERT_EQUAL_INT(
        ISO_EXT_OK, iso_extent_table_build(&s_alloc, 1u, sizeof(expected), n_blocks, &part, 1u, &tbl));

    iso_blockdev_t bdev = {
        .ctx = &raw,
        .read_sectors = mock_bdev_read_sectors,
        .epoch = mock_bdev_epoch,
    };
    harness_t h;
    /* 20-block buffer forces one 18-block READ_BLOCKS call -> >64 spans -> exercises step /= 2 */
    harness_init(&h, &bdev, 20u * ISO_CD_BLOCK_BYTES);
    iso_usb_msc_extent_backend_set_image(&h.backend, 0x77u, tbl);
    iso_media_mount(&h.media, 0x77u, false);

    uint8_t tur[6] = {0x00u};
    host_run_bot_transaction(&h, 50u, 0u, 0x00u, tur, 6u, NULL, 0u);
    uint8_t k = 0u, a = 0u, q = 0u;
    host_request_sense(&h, 51u, &k, &a, &q);

    uint8_t rd10[10] = {0x28u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, (uint8_t)n_blocks, 0u};
    host_run_bot_transaction(&h, 52u, sizeof(expected), 0x80u, rd10, 10u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), h.rx_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, h.rx_data, sizeof(expected));

    harness_destroy(&h);
    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(tbl));
}

void test_target_start_stop_unit_eject_releases_extent_ref(void) {
    mock_bdev_t raw = {.current_epoch = 3u, .fail_after_reads = -1};
    uint8_t dummy[2048];
    memset(dummy, 0x5Au, sizeof(dummy));
    for (uint32_t s = 0; s < ISO_SECTORS_PER_BLOCK; s++) {
        memcpy(raw.sectors[s], dummy + s * ISO_SECTOR_BYTES, ISO_SECTOR_BYTES);
    }
    const iso_extent_t ext = {.start_sector = 0u, .sector_count = ISO_SECTORS_PER_BLOCK};
    const iso_part_input_t part = {.file_bytes = sizeof(dummy), .extents = &ext, .n_extents = 1u};
    iso_extent_table_t *tbl = NULL;
    TEST_ASSERT_EQUAL_INT(
        ISO_EXT_OK, iso_extent_table_build(&s_alloc, 3u, sizeof(dummy), 1u, &part, 1u, &tbl));

    iso_blockdev_t bdev = {
        .ctx = &raw,
        .read_sectors = mock_bdev_read_sectors,
        .epoch = mock_bdev_epoch,
    };
    harness_t h;
    harness_init(&h, &bdev, 4u * ISO_CD_BLOCK_BYTES);

    iso_usb_msc_extent_backend_set_image(&h.backend, 0x42u, tbl);
    iso_media_mount(&h.media, 0x42u, false);
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(tbl)); /* test + backend */

    /* Prevent=1 blocks target START STOP UNIT eject and keeps backend ref */
    const uint8_t prevent_on[6] = {0x1Eu, 0u, 0u, 0u, 0x01u, 0u};
    host_run_bot_transaction(&h, 60u, 0u, 0x00u, prevent_on, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_FAILED, h.csw_status);
    uint8_t k = 0u, a = 0u, q = 0u;
    host_request_sense(&h, 61u, &k, &a, &q); /* UNIT ATTENTION 06h/28h */
    host_run_bot_transaction(&h, 62u, 0u, 0x00u, prevent_on, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);

    const uint8_t ssu_eject[6] = {0x1Bu, 0u, 0u, 0u, 0x02u, 0u}; /* LoEj=1, Start=0 */
    host_run_bot_transaction(&h, 63u, 0u, 0x00u, ssu_eject, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_FAILED, h.csw_status);
    host_request_sense(&h, 64u, &k, &a, &q);
    TEST_ASSERT_EQUAL_HEX8(0x05u, k);
    TEST_ASSERT_EQUAL_HEX8(0x53u, a);
    TEST_ASSERT_EQUAL_HEX8(0x02u, q);
    TEST_ASSERT_TRUE(iso_media_is_loaded(&h.media));
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(tbl));

    /* Clear prevent=0 -> START STOP UNIT eject succeeds and backend ref is released immediately */
    const uint8_t prevent_off[6] = {0x1Eu, 0u, 0u, 0u, 0x00u, 0u};
    host_run_bot_transaction(&h, 65u, 0u, 0x00u, prevent_off, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);

    host_run_bot_transaction(&h, 66u, 0u, 0x00u, ssu_eject, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_FALSE(iso_media_is_loaded(&h.media));

    iso_media_status_t st;
    iso_media_get_status(&h.media, &st);
    TEST_ASSERT_FALSE(st.mounted);
    TEST_ASSERT_TRUE(st.ejected_by_target);
    TEST_ASSERT_EQUAL_UINT32(1u, iso_extent_table_refs(tbl)); /* backend released immediately */

    harness_destroy(&h);
    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(tbl));
}

typedef struct {
    uint32_t target_id;
    iso_extent_table_t *tbl;
    uint32_t lookup_calls;
    uint32_t acquire_calls;
} test_resolver_ctx_t;

static int test_res_lookup_size_locked(void *ctx, uint32_t media_id, uint64_t *out_bytes) {
    test_resolver_ctx_t *rc = (test_resolver_ctx_t *)ctx;
    rc->lookup_calls++;
    if (rc->tbl == NULL || media_id != rc->target_id) {
        return ISO_ERR_NOT_FOUND;
    }
    *out_bytes = iso_extent_table_size_bytes(rc->tbl);
    return ISO_OK;
}

static iso_extent_table_t *test_res_acquire_table_locked(void *ctx, uint32_t media_id) {
    test_resolver_ctx_t *rc = (test_resolver_ctx_t *)ctx;
    rc->acquire_calls++;
    if (rc->tbl == NULL || media_id != rc->target_id) {
        return NULL;
    }
    iso_extent_table_ref(rc->tbl);
    return rc->tbl;
}

void test_resolver_delegation_refcounts_and_clear(void) {
    mock_bdev_t raw = {.current_epoch = 4u, .fail_after_reads = -1};
    for (uint32_t s = 0; s < 16u; s++) {
        memset(raw.sectors[s], (int)(0x60u + s), ISO_SECTOR_BYTES);
    }
    const iso_extent_t ext = {.start_sector = 0u, .sector_count = 16u};
    const iso_part_input_t part = {.file_bytes = 4u * ISO_CD_BLOCK_BYTES, .extents = &ext, .n_extents = 1u};
    iso_extent_table_t *tbl = NULL;
    TEST_ASSERT_EQUAL_INT(
        ISO_EXT_OK, iso_extent_table_build(&s_alloc, 4u, 4u * ISO_CD_BLOCK_BYTES, 4u, &part, 1u, &tbl));

    iso_blockdev_t bdev = {
        .ctx = &raw,
        .read_sectors = mock_bdev_read_sectors,
        .epoch = mock_bdev_epoch,
    };
    harness_t h;
    harness_init(&h, &bdev, 4u * ISO_CD_BLOCK_BYTES);

    test_resolver_ctx_t rctx = {.target_id = 0x55u, .tbl = tbl};
    const iso_usb_msc_table_resolver_t res = {
        .ctx = &rctx,
        .lookup_size_locked = test_res_lookup_size_locked,
        .acquire_table_locked = test_res_acquire_table_locked,
    };
    iso_usb_msc_extent_backend_set_resolver(&h.backend, &res);
    iso_media_mount(&h.media, 0x55u, false);

    uint8_t k = 0u, a = 0u, q = 0u;
    const uint8_t tur[6] = {0x00u};
    host_run_bot_transaction(&h, 70u, 0u, 0x00u, tur, 6u, NULL, 0u);
    host_request_sense(&h, 71u, &k, &a, &q);

    const uint8_t rdcap[10] = {0x25u};
    host_run_bot_transaction(&h, 72u, 8u, 0x80u, rdcap, 10u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, rctx.lookup_calls);
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)h.rx_data[3]); /* last LBA = 4 - 1 = 3 */

    const uint8_t rd10[10] = {0x28u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 2u, 0u};
    host_run_bot_transaction(&h, 73u, 2u * ISO_CD_BLOCK_BYTES, 0x80u, rd10, 10u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_EQUAL_UINT32(1u, rctx.acquire_calls);
    TEST_ASSERT_EQUAL_MEMORY(raw.sectors[0], h.rx_data, 8u * ISO_SECTOR_BYTES);
    TEST_ASSERT_EQUAL_UINT32(1u, iso_extent_table_refs(tbl)); /* command_done unref'd */

    /* Unknown ID -> NOT READY */
    iso_media_mount(&h.media, 0x99u, true);
    host_run_bot_transaction(&h, 74u, 0u, 0x00u, tur, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_FAILED, h.csw_status);
    host_request_sense(&h, 75u, &k, &a, &q); /* UA 06h/28h */
    host_run_bot_transaction(&h, 76u, 0u, 0x00u, tur, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_FAILED, h.csw_status);
    host_request_sense(&h, 77u, &k, &a, &q);
    TEST_ASSERT_EQUAL_HEX8(0x02u, k);
    TEST_ASSERT_EQUAL_HEX8(0x3Au, a);

    /* Clear resolver and verify fallback to standalone table image */
    iso_usb_msc_extent_backend_set_resolver(&h.backend, NULL);
    iso_usb_msc_extent_backend_set_image(&h.backend, 0x55u, tbl);
    iso_media_mount(&h.media, 0x55u, true);
    host_run_bot_transaction(&h, 78u, 0u, 0x00u, tur, 6u, NULL, 0u);
    host_request_sense(&h, 79u, &k, &a, &q);
    host_run_bot_transaction(&h, 80u, 2u * ISO_CD_BLOCK_BYTES, 0x80u, rd10, 10u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(tbl));

    harness_destroy(&h);
    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(tbl));
}

void test_table_epoch_matches_valid_stale_and_null(void) {
    mock_bdev_t raw = {.current_epoch = 7u, .fail_after_reads = -1};
    const iso_extent_t ext = {.start_sector = 0u, .sector_count = ISO_SECTORS_PER_BLOCK};
    const iso_part_input_t part = {.file_bytes = ISO_CD_BLOCK_BYTES, .extents = &ext, .n_extents = 1u};
    iso_extent_table_t *tbl = NULL;
    TEST_ASSERT_EQUAL_INT(
        ISO_EXT_OK, iso_extent_table_build(&s_alloc, 7u, ISO_CD_BLOCK_BYTES, 1u, &part, 1u, &tbl));

    iso_blockdev_t bdev = {
        .ctx = &raw,
        .read_sectors = mock_bdev_read_sectors,
        .epoch = mock_bdev_epoch,
    };
    TEST_ASSERT_TRUE(iso_usb_msc_table_epoch_matches(&bdev, tbl));

    raw.current_epoch = 8u;
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(&bdev, tbl));

    iso_blockdev_t bdev_no_epoch = {.ctx = &raw, .read_sectors = mock_bdev_read_sectors, .epoch = NULL};
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(NULL, tbl));
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(&bdev_no_epoch, tbl));
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(&bdev, NULL));

    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(tbl));
}

void test_on_disconnect_media_clears_prevent_removal_and_keeps_media_loaded(void) {
    mock_bdev_t raw = {.current_epoch = 1u, .fail_after_reads = -1};
    const iso_extent_t ext = {.start_sector = 0u, .sector_count = ISO_SECTORS_PER_BLOCK};
    const iso_part_input_t part = {.file_bytes = ISO_CD_BLOCK_BYTES, .extents = &ext, .n_extents = 1u};
    iso_extent_table_t *tbl = NULL;
    TEST_ASSERT_EQUAL_INT(
        ISO_EXT_OK, iso_extent_table_build(&s_alloc, 1u, ISO_CD_BLOCK_BYTES, 1u, &part, 1u, &tbl));

    iso_blockdev_t bdev = {
        .ctx = &raw,
        .read_sectors = mock_bdev_read_sectors,
        .epoch = mock_bdev_epoch,
    };
    harness_t h;
    harness_init(&h, &bdev, 4u * ISO_CD_BLOCK_BYTES);

    iso_usb_msc_extent_backend_set_image(&h.backend, 0x77u, tbl);
    iso_media_mount(&h.media, 0x77u, false);

    uint8_t k = 0u, a = 0u, q = 0u;
    const uint8_t prevent_on[6] = {0x1Eu, 0u, 0u, 0u, 0x01u, 0u};
    host_run_bot_transaction(&h, 90u, 0u, 0x00u, prevent_on, 6u, NULL, 0u);
    host_request_sense(&h, 91u, &k, &a, &q);
    host_run_bot_transaction(&h, 92u, 0u, 0x00u, prevent_on, 6u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT8(MSC_BOT_CSW_PASSED, h.csw_status);

    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_LOCKED, iso_media_eject(&h.media, false));
    TEST_ASSERT_TRUE(iso_media_is_loaded(&h.media));

    iso_usb_msc_on_disconnect_media(NULL);
    iso_usb_msc_on_disconnect_media(&h.media);
    TEST_ASSERT_TRUE(iso_media_is_loaded(&h.media));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&h.media, false));
    TEST_ASSERT_FALSE(iso_media_is_loaded(&h.media));

    harness_destroy(&h);
    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(tbl));
}

static void mock_bdev_epoch_bump(void *ctx) {
    ((mock_bdev_t *)ctx)->current_epoch++;
}

static void test_force_eject_epoch_bump_cb(void *ctx) {
    const iso_blockdev_t *bd = (const iso_blockdev_t *)ctx;
    if (bd != NULL && bd->epoch_bump != NULL) {
        bd->epoch_bump(bd->ctx);
    }
}

void test_force_eject_callback_bumps_epoch_and_rejects_in_flight_read(void) {
    mock_bdev_t raw = {.current_epoch = 2u, .fail_after_reads = -1};
    const iso_extent_t ext = {.start_sector = 0u, .sector_count = 8u};
    const iso_part_input_t part = {.file_bytes = 2u * ISO_CD_BLOCK_BYTES, .extents = &ext, .n_extents = 1u};
    iso_extent_table_t *tbl = NULL;
    TEST_ASSERT_EQUAL_INT(
        ISO_EXT_OK, iso_extent_table_build(&s_alloc, 2u, 2u * ISO_CD_BLOCK_BYTES, 2u, &part, 1u, &tbl));

    iso_blockdev_t bdev = {
        .ctx = &raw,
        .read_sectors = mock_bdev_read_sectors,
        .epoch = mock_bdev_epoch,
        .epoch_bump = mock_bdev_epoch_bump,
    };

    iso_media_t media;
    iso_media_init(&media, test_force_eject_epoch_bump_cb, &bdev);
    iso_usb_msc_extent_backend_t be;
    iso_usb_msc_extent_backend_init(&be, &bdev, NULL, NULL, NULL);
    iso_usb_msc_extent_backend_set_image(&be, 0x88u, tbl);
    mmc_scsi_t scsi;
    const mmc_scsi_backend_t scsi_be = iso_usb_msc_extent_backend_as_scsi(&be);
    TEST_ASSERT_EQUAL_INT(0, mmc_scsi_init(&scsi, &media, &scsi_be));

    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&media, 0x88u, false));

    /* Clear UA and begin READ(10) to acquire extent ref */
    uint8_t out[64] = {0};
    mmc_result_t mr;
    const uint8_t tur[6] = {0x00u};
    mmc_scsi_execute(&scsi, tur, 6u, MMC_DIR_NONE, 0u, out, sizeof(out), &mr);
    const uint8_t req_sense[6] = {0x03u, 0u, 0u, 0u, 18u, 0u};
    mmc_scsi_execute(&scsi, req_sense, 6u, MMC_DIR_IN, 18u, out, sizeof(out), &mr);

    const uint8_t rd10[10] = {0x28u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 0u};
    mmc_scsi_execute(&scsi, rd10, 10u, MMC_DIR_IN, ISO_CD_BLOCK_BYTES, out, sizeof(out), &mr);
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mr.status);
    TEST_ASSERT_TRUE(mr.read.is_read);
    TEST_ASSERT_EQUAL_UINT32(3u, iso_extent_table_refs(tbl));

    /* Force eject fires on_force_eject -> bdev->epoch_bump while read is in flight */
    iso_media_force_eject(&media);
    TEST_ASSERT_EQUAL_UINT32(3u, raw.current_epoch);
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(&bdev, tbl));

    uint8_t read_buf[ISO_CD_BLOCK_BYTES];
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION,
                          mmc_scsi_read_blocks(&scsi, 0u, 1u, read_buf, sizeof(read_buf)));
    mmc_scsi_command_done(&scsi);
    iso_usb_msc_sync_media_backend(&media, &be);
    TEST_ASSERT_EQUAL_UINT32(1u, iso_extent_table_refs(tbl));

    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(tbl));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_inquiry_and_tur_empty);
    RUN_TEST(test_mount_read10_read12_and_non2048_tail);
    RUN_TEST(test_mode_sense_toc_get_cfg_and_write_protect);
    RUN_TEST(test_mid_read_epoch_error_reports_medium_error_and_releases_ref);
    RUN_TEST(test_mid_command_bus_reset_defers_reset_done_and_reports_ua_29);
    RUN_TEST(test_highly_fragmented_spans_full_fallback);
    RUN_TEST(test_target_start_stop_unit_eject_releases_extent_ref);
    RUN_TEST(test_resolver_delegation_refcounts_and_clear);
    RUN_TEST(test_table_epoch_matches_valid_stale_and_null);
    RUN_TEST(test_on_disconnect_media_clears_prevent_removal_and_keeps_media_loaded);
    RUN_TEST(test_force_eject_callback_bumps_epoch_and_rejects_in_flight_read);
    return UNITY_END();
}

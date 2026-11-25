#include "iso_usb_msc_glue.h"
#include "unity.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * iso_usb_msc_glue の追加のテスト(C-16。ミューテーションテストの生き残りを殺すためのもの。docs/ISO_PUT_MSC_TESTS.md)。
 * test_iso_usb_msc_glue.c(A-15 より前の PR)は書き換えない。部品は、このファイルの中だけ(実行ファイルごとに別)。
 *
 * 部品: ロックの記録(深さ、回数、ctx)。アロケーターの記録(生きている数、ロックの中で解放されたか)。SD のモック(呼び出しごとの記録と、
 * N 回目の失敗)。表の組み立て(エクステントの一覧と大きさから)。
 * mmc_scsi_read_blocks() は、バックエンドの読み出しの後で、size_bytes 以降を 0 で埋め直す。そのため、グルーの EOF 以降のゼロ埋めは、
 * mmc_scsi を通すと見えない。バックエンドの read(iso_usb_msc_extent_backend_as_scsi の関数ポインター)を直接呼んで検査する。
 */

/* ---- 記録 ---- */

static int LOCK_DEPTH;
static int LOCKS;
static int UNLOCKS;
static int BAD_LOCK_CTX;
static int LOCK_TAG; /* lock_ctx に渡す印(取り違えを検出する) */

static int A_LIVE;
static int A_FREE_UNDER_LOCK;

static void t_lock(void *ctx) {
    if (ctx != &LOCK_TAG) {
        BAD_LOCK_CTX++;
    }
    LOCK_DEPTH++;
    LOCKS++;
}

static void t_unlock(void *ctx) {
    if (ctx != &LOCK_TAG) {
        BAD_LOCK_CTX++;
    }
    LOCK_DEPTH--;
    UNLOCKS++;
}

static void *t_alloc(void *ctx, size_t n) {
    (void)ctx;
    void *p = malloc(n);
    if (p != NULL) {
        A_LIVE++;
    }
    return p;
}

static void t_free(void *ctx, void *p) {
    (void)ctx;
    if (p != NULL) {
        A_LIVE--;
        if (LOCK_DEPTH != 0) {
            A_FREE_UNDER_LOCK++;
        }
        free(p);
    }
}

static const iso_allocator_t ALLOC = {.ctx = NULL, .alloc = t_alloc, .free = t_free};

/* ---- SD のモック ---- */

#define SD_SECTORS 1024u
#define REC_MAX 160u

typedef struct {
    uint32_t epoch;
    uint32_t calls;
    int fail_at; /* 1 始まりの呼び出しの番号。この回で fail_rc を返す(0 = 失敗しない) */
    int fail_rc;
    unsigned bad_ctx;
    int lock_depth_in_read; /* read_sectors の中でロックが取られていた回数 */
    struct {
        uint32_t epoch;
        uint64_t lba;
        uint32_t n;
        uint8_t *dst;
    } rec[REC_MAX];
    uint8_t sectors[SD_SECTORS][ISO_SECTOR_BYTES];
} sd_t;

static sd_t SD;

static uint8_t pat(uint32_t sector, uint32_t i) {
    return (uint8_t)(sector * 31u + i * 7u + 3u);
}

static int sd_read(void *ctx, uint32_t expected_epoch, uint64_t lba, uint32_t n, uint8_t *dst) {
    if (ctx != &SD) {
        SD.bad_ctx++;
    }
    if (LOCK_DEPTH != 0) {
        SD.lock_depth_in_read++;
    }
    const uint32_t i = SD.calls++;
    if (i < REC_MAX) {
        SD.rec[i].epoch = expected_epoch;
        SD.rec[i].lba = lba;
        SD.rec[i].n = n;
        SD.rec[i].dst = dst;
    }
    if (SD.fail_at != 0 && (int)SD.calls == SD.fail_at) {
        return SD.fail_rc;
    }
    if (expected_epoch != SD.epoch) {
        return ISO_ERR_EPOCH;
    }
    if (lba + n > SD_SECTORS) {
        return ISO_ERR_IO;
    }
    for (uint32_t k = 0; k < n; k++) {
        memcpy(dst + (size_t)k * ISO_SECTOR_BYTES, SD.sectors[lba + k], ISO_SECTOR_BYTES);
    }
    return ISO_OK;
}

static uint32_t sd_epoch(void *ctx) {
    if (ctx != &SD) {
        SD.bad_ctx++;
    }
    return SD.epoch;
}

static iso_blockdev_t BDEV;

static void sd_reset(uint32_t epoch) {
    memset(&SD, 0, sizeof(SD));
    SD.epoch = epoch;
    for (uint32_t s = 0; s < SD_SECTORS; s++) {
        for (uint32_t i = 0; i < ISO_SECTOR_BYTES; i++) {
            SD.sectors[s][i] = pat(s, i);
        }
    }
    memset(&BDEV, 0, sizeof(BDEV));
    BDEV.ctx = &SD;
    BDEV.read_sectors = sd_read;
    BDEV.epoch = sd_epoch;
}

/* ---- 表と、期待するデータ ---- */

static iso_extent_table_t *build_table(uint32_t epoch, uint64_t size_bytes, const iso_extent_t *ext, size_t n_ext) {
    const uint32_t blocks = (uint32_t)((size_bytes + ISO_CD_BLOCK_BYTES - 1u) / ISO_CD_BLOCK_BYTES);
    const iso_part_input_t part = {.file_bytes = size_bytes, .extents = ext, .n_extents = n_ext};
    iso_extent_table_t *t = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_table_build(&ALLOC, epoch, size_bytes, blocks, &part, 1u, &t));
    TEST_ASSERT_NOT_NULL(t);
    return t;
}

/* ファイルの内容(エクステントの順にセクタを並べたもの)を、size_bytes までは SD の内容、その後は 0 にして、exp_len バイト作る */
static void expected_image(const iso_extent_t *ext, size_t n_ext, uint64_t size_bytes, uint8_t *exp, size_t exp_len) {
    memset(exp, 0, exp_len);
    uint64_t off = 0u;
    for (size_t e = 0; e < n_ext; e++) {
        for (uint32_t k = 0; k < ext[e].sector_count; k++) {
            for (uint32_t i = 0; i < ISO_SECTOR_BYTES; i++) {
                if (off < size_bytes && off < exp_len) {
                    exp[off] = SD.sectors[ext[e].start_sector + k][i];
                }
                off++;
            }
        }
    }
}

/* ---- フィクスチャ(アサーションの失敗で fx_teardown が呼ばれなかったときは、tearDown が後始末する) ---- */

#define MEDIA_ID 0x42u

static iso_extent_table_t *TBL;
static iso_media_t MEDIA;
static iso_usb_msc_extent_backend_t BE;
static mmc_scsi_t SCSI;
static bool FX_LIVE = false;
static bool FX_SCSI = false;

static void fx_init(uint32_t epoch, uint64_t size_bytes, const iso_extent_t *ext, size_t n_ext, bool with_lock, bool with_scsi) {
    sd_reset(epoch);
    TBL = build_table(epoch, size_bytes, ext, n_ext);
    FX_LIVE = true;
    iso_media_init(&MEDIA, NULL, NULL);
    iso_usb_msc_extent_backend_init(&BE, &BDEV, with_lock ? t_lock : NULL, with_lock ? t_unlock : NULL,
                                    with_lock ? &LOCK_TAG : NULL);
    iso_usb_msc_extent_backend_set_image(&BE, MEDIA_ID, TBL);
    if (with_scsi) {
        const mmc_scsi_backend_t sb = iso_usb_msc_extent_backend_as_scsi(&BE);
        TEST_ASSERT_EQUAL_INT(0, mmc_scsi_init(&SCSI, &MEDIA, &sb));
        FX_SCSI = true;
        TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, MEDIA_ID, false));
    }
    LOCKS = 0; /* 準備のロックは数えない */
    UNLOCKS = 0;
}

static void fx_teardown(void) {
    if (FX_SCSI) {
        mmc_scsi_command_done(&SCSI);
        FX_SCSI = false;
    }
    iso_usb_msc_extent_backend_clear(&BE);
    const uint32_t refs = iso_extent_table_refs(TBL);
    TEST_ASSERT_EQUAL_UINT32(1u, refs); /* テストの分だけ */
    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(TBL));
    TBL = NULL;
    FX_LIVE = false;
    TEST_ASSERT_EQUAL_INT(0, A_LIVE);
    TEST_ASSERT_EQUAL_INT(0, A_FREE_UNDER_LOCK);
    TEST_ASSERT_EQUAL_INT(0, LOCK_DEPTH);
    TEST_ASSERT_EQUAL_INT(LOCKS, UNLOCKS);
    TEST_ASSERT_EQUAL_INT(0, BAD_LOCK_CTX);
    TEST_ASSERT_EQUAL_UINT(0u, SD.bad_ctx);
    TEST_ASSERT_EQUAL_INT(0, SD.lock_depth_in_read);
}

void setUp(void) {
    LOCK_DEPTH = 0;
    LOCKS = 0;
    UNLOCKS = 0;
    BAD_LOCK_CTX = 0;
    A_LIVE = 0;
    A_FREE_UNDER_LOCK = 0;
}

/* アサーションの失敗でテストが中断したときの後始末。検査はしない(元の失敗を隠さない) */
void tearDown(void) {
    if (FX_LIVE) {
        if (FX_SCSI) {
            mmc_scsi_command_done(&SCSI);
            FX_SCSI = false;
        }
        iso_usb_msc_extent_backend_clear(&BE);
        iso_extent_table_unref(TBL);
        TBL = NULL;
        FX_LIVE = false;
    }
}

/* ---- 小さい補助 ---- */

static mmc_scsi_backend_t scsi_be(void) {
    return iso_usb_msc_extent_backend_as_scsi(&BE);
}

/* msc_bot_cmd_t を作って、グルーに渡す(EXECUTE) */
static void exec_cdb(const uint8_t *cdb, uint8_t cdb_len, msc_bot_dir_t dir, uint32_t expected_len, uint8_t *out,
                     uint32_t out_cap, uint32_t seq, msc_bot_cmd_result_t *res) {
    msc_bot_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = MSC_BOT_CMD_EXECUTE;
    cmd.seq = seq;
    memcpy(cmd.cdb, cdb, cdb_len);
    cmd.cdb_len = cdb_len;
    cmd.dir = dir;
    cmd.expected_len = expected_len;
    cmd.out = out;
    cmd.out_cap = out_cap;
    iso_usb_msc_exec_cmd(&SCSI, &cmd, res);
}

/* マウント直後の Unit Attention を消す(TUR は失敗し、REQUEST SENSE は成功する) */
static void clear_unit_attention(void) {
    const uint8_t tur[6] = {0x00u};
    const uint8_t rs[6] = {0x03u, 0u, 0u, 0u, 18u, 0u};
    uint8_t out[32];
    msc_bot_cmd_result_t r;
    exec_cdb(tur, 6u, MSC_BOT_DIR_NONE, 0u, out, sizeof(out), 1u, &r);
    TEST_ASSERT_FALSE(r.good);
    exec_cdb(rs, 6u, MSC_BOT_DIR_IN, 18u, out, sizeof(out), 2u, &r);
    TEST_ASSERT_TRUE(r.good);
    exec_cdb(tur, 6u, MSC_BOT_DIR_NONE, 0u, out, sizeof(out), 3u, &r);
    TEST_ASSERT_TRUE(r.good);
}

static const iso_extent_t EXT6[1] = {{.start_sector = 8u, .sector_count = 24u}}; /* 6 ブロック(24 セクタ) */

/* ======================================================================
 * iso_usb_msc_exec_cmd
 * ====================================================================== */

void test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, true);
    msc_bot_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = MSC_BOT_CMD_EXECUTE;
    cmd.seq = 77u;
    msc_bot_cmd_result_t res;
    memset(&res, 0xA5, sizeof(res));

    iso_usb_msc_exec_cmd(NULL, &cmd, &res);
    TEST_ASSERT_EQUAL_UINT32(0xA5A5A5A5u, res.seq);
    iso_usb_msc_exec_cmd(&SCSI, NULL, &res);
    TEST_ASSERT_EQUAL_UINT32(0xA5A5A5A5u, res.seq);
    iso_usb_msc_exec_cmd(&SCSI, &cmd, NULL); /* クラッシュしない */
    fx_teardown();
}

void test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, true);
    const msc_bot_cmd_kind_t kinds[] = {(msc_bot_cmd_kind_t)0, (msc_bot_cmd_kind_t)3, (msc_bot_cmd_kind_t)99};
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        msc_bot_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = kinds[k];
        cmd.seq = 1234u + (uint32_t)k;
        msc_bot_cmd_result_t res;
        memset(&res, 0xA5, sizeof(res));
        iso_usb_msc_exec_cmd(&SCSI, &cmd, &res);
        TEST_ASSERT_EQUAL_UINT32(1234u + (uint32_t)k, res.seq);
        TEST_ASSERT_FALSE(res.good);
        TEST_ASSERT_FALSE(res.is_read);
        TEST_ASSERT_EQUAL_UINT64(0u, res.read_lba);
        TEST_ASSERT_EQUAL_UINT32(0u, res.read_blocks);
        TEST_ASSERT_EQUAL_UINT32(0u, res.data_len);
        TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_NONE, res.dev_dir);
    }
    fx_teardown();
}

void test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, true);
    uint8_t out[64];
    msc_bot_cmd_result_t r;

    /* マウント直後の TUR: Unit Attention で失敗。データなし */
    const uint8_t tur[6] = {0x00u};
    memset(&r, 0xA5, sizeof(r));
    exec_cdb(tur, 6u, MSC_BOT_DIR_NONE, 0u, out, sizeof(out), 500u, &r);
    TEST_ASSERT_EQUAL_UINT32(500u, r.seq);
    TEST_ASSERT_FALSE(r.good);
    TEST_ASSERT_FALSE(r.is_read);
    TEST_ASSERT_EQUAL_UINT32(0u, r.data_len);
    TEST_ASSERT_EQUAL_UINT64(0u, r.read_lba);
    TEST_ASSERT_EQUAL_UINT32(0u, r.read_blocks);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_NONE, r.dev_dir);

    /* REQUEST SENSE: 成功。18 バイトの IN */
    const uint8_t rs[6] = {0x03u, 0u, 0u, 0u, 18u, 0u};
    memset(&r, 0xA5, sizeof(r));
    exec_cdb(rs, 6u, MSC_BOT_DIR_IN, 18u, out, sizeof(out), 501u, &r);
    TEST_ASSERT_TRUE(r.good);
    TEST_ASSERT_FALSE(r.is_read);
    TEST_ASSERT_EQUAL_UINT32(18u, r.data_len);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_IN, r.dev_dir);
    TEST_ASSERT_EQUAL_UINT64(0u, r.read_lba);
    TEST_ASSERT_EQUAL_UINT32(0u, r.read_blocks);

    /* TUR(2 回目)は成功。データなし → NONE */
    memset(&r, 0xA5, sizeof(r));
    exec_cdb(tur, 6u, MSC_BOT_DIR_NONE, 0u, out, sizeof(out), 502u, &r);
    TEST_ASSERT_TRUE(r.good);
    TEST_ASSERT_EQUAL_UINT32(0u, r.data_len);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_NONE, r.dev_dir);
    TEST_ASSERT_FALSE(r.is_read);

    /* INQUIRY: 36 バイトの IN。out_cap で切り詰める */
    const uint8_t inq[6] = {0x12u, 0u, 0u, 0u, 36u, 0u};
    memset(out, 0xA5, sizeof(out));
    memset(&r, 0xA5, sizeof(r));
    exec_cdb(inq, 6u, MSC_BOT_DIR_IN, 36u, out, 36u, 503u, &r);
    TEST_ASSERT_TRUE(r.good);
    TEST_ASSERT_EQUAL_UINT32(36u, r.data_len);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_IN, r.dev_dir);
    TEST_ASSERT_EQUAL_HEX8(0x05u, out[0] & 0x1Fu);
    TEST_ASSERT_EQUAL_HEX8(0xA5u, out[36]); /* out_cap を超えて書かない */
    memset(out, 0xA5, sizeof(out));
    exec_cdb(inq, 6u, MSC_BOT_DIR_IN, 36u, out, 10u, 504u, &r);
    TEST_ASSERT_TRUE(r.good);
    TEST_ASSERT_EQUAL_UINT32(10u, r.data_len); /* out_cap(10)で切り詰める */
    TEST_ASSERT_EQUAL_HEX8(0xA5u, out[10]);

    /* 1 バイトだけの応答(割り当て長 1): data_len 1 は IN */
    const uint8_t inq1[6] = {0x12u, 0u, 0u, 0u, 1u, 0u};
    memset(&r, 0xA5, sizeof(r));
    exec_cdb(inq1, 6u, MSC_BOT_DIR_IN, 1u, out, sizeof(out), 506u, &r);
    TEST_ASSERT_TRUE(r.good);
    TEST_ASSERT_EQUAL_UINT32(1u, r.data_len);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_IN, r.dev_dir);

    /* cdb_len: INQUIRY に必要な長さより短いと、ILLEGAL REQUEST で失敗 */
    exec_cdb(inq, 3u, MSC_BOT_DIR_IN, 36u, out, sizeof(out), 505u, &r);
    TEST_ASSERT_FALSE(r.good);
    TEST_ASSERT_EQUAL_UINT32(0u, r.data_len);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_NONE, r.dev_dir);
    fx_teardown();
}

void test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, true);
    clear_unit_attention();
    uint8_t out[16];
    msc_bot_cmd_result_t r;

    /* READ(10) lba = 3、blocks = 2 */
    const uint8_t rd[10] = {0x28u, 0u, 0u, 0u, 0u, 3u, 0u, 0u, 2u, 0u};
    memset(&r, 0xA5, sizeof(r));
    exec_cdb(rd, 10u, MSC_BOT_DIR_IN, 2u * ISO_CD_BLOCK_BYTES, out, sizeof(out), 600u, &r);
    TEST_ASSERT_EQUAL_UINT32(600u, r.seq);
    TEST_ASSERT_TRUE(r.good);
    TEST_ASSERT_TRUE(r.is_read);
    TEST_ASSERT_EQUAL_UINT64(3u, r.read_lba);
    TEST_ASSERT_EQUAL_UINT32(2u, r.read_blocks);
    TEST_ASSERT_EQUAL_UINT32(0u, r.data_len); /* READ のデータは、EXECUTE では作らない */
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_IN, r.dev_dir);
    TEST_ASSERT_TRUE(mmc_scsi_holds_reference(&SCSI));
    mmc_scsi_command_done(&SCSI);

    /* READ(10) blocks = 1: 1 ブロックでも IN */
    const uint8_t rd1[10] = {0x28u, 0u, 0u, 0u, 0u, 1u, 0u, 0u, 1u, 0u};
    memset(&r, 0xA5, sizeof(r));
    exec_cdb(rd1, 10u, MSC_BOT_DIR_IN, ISO_CD_BLOCK_BYTES, out, sizeof(out), 603u, &r);
    TEST_ASSERT_TRUE(r.good);
    TEST_ASSERT_TRUE(r.is_read);
    TEST_ASSERT_EQUAL_UINT64(1u, r.read_lba);
    TEST_ASSERT_EQUAL_UINT32(1u, r.read_blocks);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_IN, r.dev_dir);
    mmc_scsi_command_done(&SCSI);

    /* READ(10) blocks = 0: 成功、READ、転送なし → NONE */
    const uint8_t rd0[10] = {0x28u, 0u, 0u, 0u, 0u, 5u, 0u, 0u, 0u, 0u};
    memset(&r, 0xA5, sizeof(r));
    exec_cdb(rd0, 10u, MSC_BOT_DIR_NONE, 0u, out, sizeof(out), 601u, &r);
    TEST_ASSERT_TRUE(r.good);
    TEST_ASSERT_TRUE(r.is_read);
    TEST_ASSERT_EQUAL_UINT64(5u, r.read_lba);
    TEST_ASSERT_EQUAL_UINT32(0u, r.read_blocks);
    TEST_ASSERT_EQUAL_UINT32(0u, r.data_len);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_NONE, r.dev_dir);
    TEST_ASSERT_FALSE(mmc_scsi_holds_reference(&SCSI));

    /* 範囲外の READ: 失敗。READ の結果は立たない */
    const uint8_t rdx[10] = {0x28u, 0u, 0u, 0u, 0u, 5u, 0u, 0u, 2u, 0u}; /* 5 + 2 > 6 */
    memset(&r, 0xA5, sizeof(r));
    exec_cdb(rdx, 10u, MSC_BOT_DIR_IN, 2u * ISO_CD_BLOCK_BYTES, out, sizeof(out), 602u, &r);
    TEST_ASSERT_FALSE(r.good);
    TEST_ASSERT_FALSE(r.is_read);
    TEST_ASSERT_EQUAL_UINT32(0u, r.read_blocks);
    TEST_ASSERT_EQUAL_UINT32(0u, r.data_len);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_NONE, r.dev_dir);
    fx_teardown();
}

void test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction(void) {
    /* 方向が効くのは、GET EVENT STATUS NOTIFICATION のイベントの消費だけ(IN で、expected_len >= 8 のときだけ消費する) */
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, true);
    clear_unit_attention();
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NEW_MEDIA, iso_media_take_event(&MEDIA)); /* マウントの NEW_MEDIA を消す */
    uint8_t out[16];
    msc_bot_cmd_result_t r;
    const uint8_t get_event[10] = {0x4Au, 0x01u, 0u, 0u, 0x10u, 0u, 0u, 0x00u, 0x08u, 0x00u};

    const struct {
        msc_bot_dir_t dir;
        uint32_t expected_len;
        bool consumed;
    } cases[] = {
        {MSC_BOT_DIR_OUT, 8u, false},
        {MSC_BOT_DIR_NONE, 8u, false},
        {(msc_bot_dir_t)99, 8u, false}, /* 未知の値は NONE */
        {MSC_BOT_DIR_IN, 7u, false},
        {MSC_BOT_DIR_IN, 8u, true},
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        TEST_ASSERT_TRUE(iso_media_request_eject(&MEDIA)); /* EJECT_REQUEST のイベントを立てる */
        TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_EJECT_REQUEST, iso_media_peek_event(&MEDIA));
        memset(out, 0, sizeof(out));
        exec_cdb(get_event, 10u, cases[k].dir, cases[k].expected_len, out, sizeof(out), 700u + (uint32_t)k, &r);
        TEST_ASSERT_TRUE(r.good);
        TEST_ASSERT_EQUAL_HEX8(0x01u, out[4]); /* EJECT REQUEST が報告される */
        if (cases[k].consumed) {
            TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(&MEDIA));
        } else {
            TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_EJECT_REQUEST, iso_media_peek_event(&MEDIA));
            (void)iso_media_take_event(&MEDIA); /* 次の周回のために消す */
        }
    }
    fx_teardown();
}

void test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, true);
    clear_unit_attention();
    uint8_t exp[6u * ISO_CD_BLOCK_BYTES];
    expected_image(EXT6, 1u, 6u * ISO_CD_BLOCK_BYTES, exp, sizeof(exp));

    /* READ(10) lba = 1、blocks = 3 の参照を取る */
    const uint8_t rd[10] = {0x28u, 0u, 0u, 0u, 0u, 1u, 0u, 0u, 3u, 0u};
    msc_bot_cmd_result_t r;
    uint8_t scratch[8];
    exec_cdb(rd, 10u, MSC_BOT_DIR_IN, 3u * ISO_CD_BLOCK_BYTES, scratch, sizeof(scratch), 800u, &r);
    TEST_ASSERT_TRUE(r.good);

    uint8_t *buf = (uint8_t *)malloc(4u * ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_NOT_NULL(buf);
    memset(buf, 0xA5, 4u * ISO_CD_BLOCK_BYTES);
    msc_bot_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = MSC_BOT_CMD_READ_BLOCKS;
    cmd.seq = 801u;
    cmd.lba = 2u;
    cmd.blocks = 2u;
    cmd.buf = buf;
    cmd.cap = 4u * ISO_CD_BLOCK_BYTES;
    memset(&r, 0xA5, sizeof(r));
    iso_usb_msc_exec_cmd(&SCSI, &cmd, &r);
    TEST_ASSERT_EQUAL_UINT32(801u, r.seq);
    TEST_ASSERT_TRUE(r.good);
    TEST_ASSERT_FALSE(r.is_read);
    TEST_ASSERT_EQUAL_UINT64(0u, r.read_lba);
    TEST_ASSERT_EQUAL_UINT32(0u, r.read_blocks);
    TEST_ASSERT_EQUAL_UINT32(0u, r.data_len);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_NONE, r.dev_dir);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp + 2u * ISO_CD_BLOCK_BYTES, buf, 2u * ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_EQUAL_HEX8(0xA5u, buf[2u * ISO_CD_BLOCK_BYTES]); /* blocks(2)を超えて書かない */

    /* cap が足りない(1 ブロック分): ILLEGAL REQUEST → 失敗 */
    cmd.cap = ISO_CD_BLOCK_BYTES;
    iso_usb_msc_exec_cmd(&SCSI, &cmd, &r);
    TEST_ASSERT_FALSE(r.good);
    TEST_ASSERT_EQUAL_UINT32(801u, r.seq);
    cmd.cap = 4u * ISO_CD_BLOCK_BYTES;

    /* バックエンドの失敗: MEDIUM ERROR → good = false */
    SD.fail_at = (int)SD.calls + 1;
    SD.fail_rc = ISO_ERR_IO;
    iso_usb_msc_exec_cmd(&SCSI, &cmd, &r);
    TEST_ASSERT_FALSE(r.good);
    SD.fail_at = 0;
    iso_usb_msc_exec_cmd(&SCSI, &cmd, &r);
    TEST_ASSERT_TRUE(r.good);
    free(buf);
    fx_teardown();
}

void test_exec_cmd_read_blocks_rejects_a_lba_above_32_bits_without_reading_but_passes_the_max_32_bit_lba(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, true);
    clear_unit_attention();
    const uint8_t rd[10] = {0x28u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 0u};
    msc_bot_cmd_result_t r;
    uint8_t scratch[8];
    exec_cdb(rd, 10u, MSC_BOT_DIR_IN, ISO_CD_BLOCK_BYTES, scratch, sizeof(scratch), 900u, &r);
    TEST_ASSERT_TRUE(r.good);
    mmc_sense_t sn;
    TEST_ASSERT_FALSE(mmc_scsi_last_sense(&SCSI, &sn)); /* READ の EXECUTE の後、センスはない */

    uint8_t *buf = (uint8_t *)malloc(ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_NOT_NULL(buf);
    msc_bot_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = MSC_BOT_CMD_READ_BLOCKS;
    cmd.seq = 901u;
    cmd.blocks = 1u;
    cmd.buf = buf;
    cmd.cap = ISO_CD_BLOCK_BYTES;

    /* 下位 32 ビットが 0(= 有効な LBA 0)になる 33 ビットの LBA: 読まずに失敗 */
    cmd.lba = 0x100000000ULL;
    memset(buf, 0xA5, ISO_CD_BLOCK_BYTES);
    msc_bot_cmd_result_t r2;
    memset(&r2, 0xA5, sizeof(r2));
    iso_usb_msc_exec_cmd(&SCSI, &cmd, &r2);
    TEST_ASSERT_EQUAL_UINT32(901u, r2.seq);
    TEST_ASSERT_FALSE(r2.good);
    TEST_ASSERT_EQUAL_UINT32(0u, SD.calls);
    TEST_ASSERT_EQUAL_HEX8(0xA5u, buf[0]);
    TEST_ASSERT_FALSE(mmc_scsi_last_sense(&SCSI, &sn)); /* mmc_scsi を呼んでいない(センスを記録しない) */

    /* 32 ビットの最大の LBA: mmc_scsi に渡す(範囲外なので ILLEGAL REQUEST 21h/00h で失敗。センスが残る) */
    cmd.lba = UINT32_MAX;
    iso_usb_msc_exec_cmd(&SCSI, &cmd, &r2);
    TEST_ASSERT_FALSE(r2.good);
    TEST_ASSERT_EQUAL_UINT32(0u, SD.calls);
    TEST_ASSERT_TRUE(mmc_scsi_last_sense(&SCSI, &sn));
    TEST_ASSERT_EQUAL_HEX8(MMC_SENSE_ILLEGAL_REQUEST, sn.key);
    TEST_ASSERT_EQUAL_HEX8(0x21u, sn.asc);
    free(buf);
    fx_teardown();
}

/* ======================================================================
 * バックエンド: size_bytes / acquire / release
 * ====================================================================== */

void test_backend_size_bytes_validates_arguments_and_looks_up_the_installed_table(void) {
    fx_init(1u, 5u * ISO_CD_BLOCK_BYTES + 300u, EXT6, 1u, true, false);
    const mmc_scsi_backend_t sb = scsi_be();
    uint64_t out = 0x1234u;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, sb.size_bytes(NULL, MEDIA_ID, &out));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, sb.size_bytes(sb.ctx, MEDIA_ID, NULL));
    TEST_ASSERT_EQUAL_UINT64(0x1234u, out);
    TEST_ASSERT_EQUAL_INT(0, LOCKS); /* 引数の検査は、ロックの前 */

    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.size_bytes(sb.ctx, MEDIA_ID, &out));
    TEST_ASSERT_EQUAL_UINT64(5u * ISO_CD_BLOCK_BYTES + 300u, out);
    TEST_ASSERT_EQUAL_INT(1, LOCKS);
    TEST_ASSERT_EQUAL_INT(1, UNLOCKS);

    out = 0x1234u;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, sb.size_bytes(sb.ctx, MEDIA_ID + 1u, &out)); /* 別の id */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, sb.size_bytes(sb.ctx, 0u, &out));
    TEST_ASSERT_EQUAL_UINT64(0x1234u, out);
    TEST_ASSERT_EQUAL_INT(3, LOCKS);
    TEST_ASSERT_EQUAL_INT(3, UNLOCKS);

    iso_usb_msc_extent_backend_clear(&BE); /* 表なし */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, sb.size_bytes(sb.ctx, MEDIA_ID, &out));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, sb.size_bytes(sb.ctx, 0u, &out)); /* clear の id 0 とも一致しない */
    TEST_ASSERT_EQUAL_UINT64(0x1234u, out);
    fx_teardown();
}

void test_backend_acquire_validates_arguments_takes_a_reference_and_clears_the_handle_first(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, false);
    const mmc_scsi_backend_t sb = scsi_be();
    void *h = (void *)&SD;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, sb.acquire(NULL, MEDIA_ID, &h));
    TEST_ASSERT_TRUE(h == (void *)&SD); /* be が NULL のとき、handle には触らない */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, sb.acquire(sb.ctx, MEDIA_ID, NULL));
    TEST_ASSERT_EQUAL_INT(0, LOCKS);

    /* 別の id: NOT_FOUND。handle は NULL に戻され、参照は増えない */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, sb.acquire(sb.ctx, MEDIA_ID + 1u, &h));
    TEST_ASSERT_NULL(h);
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(TBL)); /* テスト + バックエンド */
    h = (void *)&SD;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, sb.acquire(sb.ctx, 0u, &h));
    TEST_ASSERT_NULL(h);

    /* 一致: 参照が 1 つ増え、handle は表 */
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.acquire(sb.ctx, MEDIA_ID, &h));
    TEST_ASSERT_TRUE(h == (void *)TBL);
    TEST_ASSERT_EQUAL_UINT32(3u, iso_extent_table_refs(TBL));
    sb.release(sb.ctx, h);
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(TBL));
    TEST_ASSERT_EQUAL_INT(LOCKS, UNLOCKS);

    /* 表なし */
    iso_usb_msc_extent_backend_clear(&BE);
    h = (void *)&SD;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, sb.acquire(sb.ctx, MEDIA_ID, &h));
    TEST_ASSERT_NULL(h);
    h = (void *)&SD;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, sb.acquire(sb.ctx, 0u, &h));
    TEST_ASSERT_NULL(h);
    fx_teardown();
}

void test_backend_release_unrefs_a_handle_and_ignores_null_and_the_ctx(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, false);
    const mmc_scsi_backend_t sb = scsi_be();
    void *h = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.acquire(sb.ctx, MEDIA_ID, &h));
    TEST_ASSERT_EQUAL_UINT32(3u, iso_extent_table_refs(TBL));
    sb.release(NULL, h); /* ctx は使わない */
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(TBL));
    sb.release(sb.ctx, NULL); /* NULL は何もしない */
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(TBL));
    fx_teardown();
}

/* ---- リゾルバ ---- */

typedef struct {
    uint32_t size_calls;
    uint32_t acq_calls;
    uint32_t last_media_id;
    int depth_in_size;
    int depth_in_acq;
    int size_rc;
    uint64_t size_val;
    iso_extent_table_t *acq_tbl; /* acquire が返す表(参照は、ここで増やす) */
    unsigned bad_ctx;
} res_t;

static res_t RES;

static int r_size(void *ctx, uint32_t media_id, uint64_t *out) {
    if (ctx != &RES) {
        RES.bad_ctx++;
    }
    RES.size_calls++;
    RES.last_media_id = media_id;
    RES.depth_in_size = LOCK_DEPTH;
    if (RES.size_rc == 0) {
        *out = RES.size_val;
    }
    return RES.size_rc;
}

static iso_extent_table_t *r_acq(void *ctx, uint32_t media_id) {
    if (ctx != &RES) {
        RES.bad_ctx++;
    }
    RES.acq_calls++;
    RES.last_media_id = media_id;
    RES.depth_in_acq = LOCK_DEPTH;
    if (RES.acq_tbl != NULL) {
        iso_extent_table_ref(RES.acq_tbl);
    }
    return RES.acq_tbl;
}

static iso_usb_msc_table_resolver_t resolver_both(void) {
    iso_usb_msc_table_resolver_t r = {.ctx = &RES, .lookup_size_locked = r_size, .acquire_table_locked = r_acq};
    return r;
}

void test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, false);
    memset(&RES, 0, sizeof(RES));
    RES.size_val = 777u;
    RES.depth_in_size = -1;
    const iso_usb_msc_table_resolver_t res = resolver_both();
    iso_usb_msc_extent_backend_set_resolver(&BE, &res);
    LOCKS = 0;
    UNLOCKS = 0;
    const mmc_scsi_backend_t sb = scsi_be();
    uint64_t out = 1u;

    /* 表が入っていても、リゾルバが先。id は、そのまま渡す(表の id と違ってもよい) */
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.size_bytes(sb.ctx, 0xBEEFu, &out));
    TEST_ASSERT_EQUAL_UINT64(777u, out);
    TEST_ASSERT_EQUAL_UINT32(1u, RES.size_calls);
    TEST_ASSERT_EQUAL_UINT32(0xBEEFu, RES.last_media_id);
    TEST_ASSERT_EQUAL_INT(0, RES.depth_in_size); /* バックエンドのロックは、リゾルバの前に放している */
    TEST_ASSERT_EQUAL_INT(1, LOCKS);
    TEST_ASSERT_EQUAL_INT(1, UNLOCKS);

    /* 戻り値は、そのまま */
    RES.size_rc = ISO_ERR_NOT_FOUND;
    out = 5u;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, sb.size_bytes(sb.ctx, MEDIA_ID, &out));
    TEST_ASSERT_EQUAL_UINT64(5u, out);
    RES.size_rc = -77;
    TEST_ASSERT_EQUAL_INT(-77, sb.size_bytes(sb.ctx, MEDIA_ID, &out));
    TEST_ASSERT_EQUAL_UINT32(3u, RES.size_calls);
    TEST_ASSERT_EQUAL_INT(0, RES.bad_ctx);

    /* acquire だけのリゾルバ(lookup_size_locked = NULL)では、size_bytes は表を引く */
    iso_usb_msc_table_resolver_t only_acq = {.ctx = &RES, .lookup_size_locked = NULL, .acquire_table_locked = r_acq};
    iso_usb_msc_extent_backend_set_resolver(&BE, &only_acq);
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.size_bytes(sb.ctx, MEDIA_ID, &out));
    TEST_ASSERT_EQUAL_UINT64(6u * ISO_CD_BLOCK_BYTES, out);
    TEST_ASSERT_EQUAL_UINT32(3u, RES.size_calls);
    fx_teardown();
}

void test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, false);
    memset(&RES, 0, sizeof(RES));
    RES.acq_tbl = TBL;
    RES.depth_in_acq = -1;
    const iso_usb_msc_table_resolver_t res = resolver_both();
    iso_usb_msc_extent_backend_set_resolver(&BE, &res);
    const mmc_scsi_backend_t sb = scsi_be();

    void *h = (void *)&SD;
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.acquire(sb.ctx, 0xBEEFu, &h));
    TEST_ASSERT_TRUE(h == (void *)TBL);
    TEST_ASSERT_EQUAL_UINT32(1u, RES.acq_calls);
    TEST_ASSERT_EQUAL_UINT32(0xBEEFu, RES.last_media_id);
    TEST_ASSERT_EQUAL_INT(0, RES.depth_in_acq);
    TEST_ASSERT_EQUAL_UINT32(3u, iso_extent_table_refs(TBL)); /* テスト + バックエンドの表 + リゾルバの 1 つ(グルーは足さない) */
    sb.release(sb.ctx, h);
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(TBL));

    /* リゾルバが NULL を返す: NOT_FOUND。handle は NULL */
    RES.acq_tbl = NULL;
    h = (void *)&SD;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, sb.acquire(sb.ctx, MEDIA_ID, &h));
    TEST_ASSERT_NULL(h);
    TEST_ASSERT_EQUAL_UINT32(2u, RES.acq_calls);
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(TBL));
    TEST_ASSERT_EQUAL_INT(0, RES.bad_ctx);
    TEST_ASSERT_EQUAL_INT(LOCKS, UNLOCKS);

    /* lookup_size_locked だけのリゾルバ(acquire_table_locked = NULL)では、acquire は表を引く */
    iso_usb_msc_table_resolver_t only_size = {.ctx = &RES, .lookup_size_locked = r_size, .acquire_table_locked = NULL};
    iso_usb_msc_extent_backend_set_resolver(&BE, &only_size);
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.acquire(sb.ctx, MEDIA_ID, &h));
    TEST_ASSERT_TRUE(h == (void *)TBL);
    TEST_ASSERT_EQUAL_UINT32(2u, RES.acq_calls);
    TEST_ASSERT_EQUAL_UINT32(3u, iso_extent_table_refs(TBL));
    sb.release(sb.ctx, h);
    fx_teardown();
}

void test_set_resolver_copies_the_whole_struct_and_null_clears_it(void) {
    iso_usb_msc_extent_backend_t be;
    memset(&be, 0xA5, sizeof(be));
    iso_usb_msc_extent_backend_set_resolver(NULL, NULL); /* クラッシュしない */
    iso_usb_msc_extent_backend_init(&be, NULL, t_lock, t_unlock, &LOCK_TAG);

    int dummy;
    iso_usb_msc_table_resolver_t res = {.ctx = &dummy, .lookup_size_locked = r_size, .acquire_table_locked = r_acq};
    iso_usb_msc_extent_backend_set_resolver(&be, &res);
    TEST_ASSERT_TRUE(be.resolver.ctx == (void *)&dummy);
    TEST_ASSERT_TRUE(be.resolver.lookup_size_locked == r_size);
    TEST_ASSERT_TRUE(be.resolver.acquire_table_locked == r_acq);
    TEST_ASSERT_EQUAL_INT(1, LOCKS);
    TEST_ASSERT_EQUAL_INT(1, UNLOCKS);

    iso_usb_msc_extent_backend_set_resolver(&be, NULL);
    TEST_ASSERT_NULL(be.resolver.ctx);
    TEST_ASSERT_NULL(be.resolver.lookup_size_locked);
    TEST_ASSERT_NULL(be.resolver.acquire_table_locked);
    TEST_ASSERT_EQUAL_INT(2, LOCKS);
    TEST_ASSERT_EQUAL_INT(2, UNLOCKS);
    TEST_ASSERT_EQUAL_INT(0, BAD_LOCK_CTX);
}

/* ======================================================================
 * バックエンド: read
 * ====================================================================== */

void test_backend_read_rejects_null_arguments_in_the_documented_order(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, false);
    const mmc_scsi_backend_t sb = scsi_be();
    uint8_t *buf = (uint8_t *)malloc(ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_NOT_NULL(buf);

    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, sb.read(NULL, TBL, 0u, 1u, buf));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, sb.read(sb.ctx, NULL, 0u, 1u, buf));
    /* blocks == 0 は、buf が NULL でも、何も読まずに OK(handle の検査の後) */
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.read(sb.ctx, TBL, 0u, 0u, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, sb.read(NULL, TBL, 0u, 0u, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, sb.read(sb.ctx, NULL, 0u, 0u, buf));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, sb.read(sb.ctx, TBL, 0u, 1u, NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, SD.calls);
    TEST_ASSERT_EQUAL_INT(0, LOCKS); /* 引数の検査は、ロックの前 */

    /* bdev が NULL、read_sectors が NULL: NO_CARD */
    iso_usb_msc_extent_backend_set_blockdev(&BE, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_CARD, sb.read(sb.ctx, TBL, 0u, 1u, buf));
    iso_blockdev_t no_read = BDEV;
    no_read.read_sectors = NULL;
    iso_usb_msc_extent_backend_set_blockdev(&BE, &no_read);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_CARD, sb.read(sb.ctx, TBL, 0u, 1u, buf));
    TEST_ASSERT_EQUAL_UINT32(0u, SD.calls);

    /* 戻すと読める */
    iso_usb_msc_extent_backend_set_blockdev(&BE, &BDEV);
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.read(sb.ctx, TBL, 0u, 1u, buf));
    TEST_ASSERT_EQUAL_UINT32(1u, SD.calls);
    free(buf);
    fx_teardown();
}

void test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations(void) {
    /* 3 ブロック = 12 セクタ: [10..15](6)、[40..45](6) */
    const iso_extent_t ext[2] = {{10u, 6u}, {40u, 6u}};
    const uint64_t size = 3u * ISO_CD_BLOCK_BYTES;
    fx_init(0x1234u, size, ext, 2u, true, false);
    const mmc_scsi_backend_t sb = scsi_be();
    uint8_t exp[3u * ISO_CD_BLOCK_BYTES];
    expected_image(ext, 2u, size, exp, sizeof(exp));
    uint8_t *buf = (uint8_t *)malloc(sizeof(exp));
    TEST_ASSERT_NOT_NULL(buf);

    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.read(sb.ctx, TBL, 0u, 3u, buf));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp, buf, sizeof(exp));
    TEST_ASSERT_EQUAL_UINT32(2u, SD.calls);
    TEST_ASSERT_EQUAL_UINT32(0x1234u, SD.rec[0].epoch);
    TEST_ASSERT_EQUAL_UINT32(0x1234u, SD.rec[1].epoch);
    TEST_ASSERT_EQUAL_UINT64(10u, SD.rec[0].lba);
    TEST_ASSERT_EQUAL_UINT32(6u, SD.rec[0].n);
    TEST_ASSERT_TRUE(SD.rec[0].dst == buf);
    TEST_ASSERT_EQUAL_UINT64(40u, SD.rec[1].lba);
    TEST_ASSERT_EQUAL_UINT32(6u, SD.rec[1].n);
    TEST_ASSERT_TRUE(SD.rec[1].dst == buf + 6u * ISO_SECTOR_BYTES);
    TEST_ASSERT_EQUAL_INT(1, LOCKS); /* bdev のポインターを取るときだけ */
    TEST_ASSERT_EQUAL_INT(1, UNLOCKS);

    /* 途中から: block 1(ファイルのセクタ 4..7)= [14..15] と [40..41] */
    SD.calls = 0u;
    memset(buf, 0xA5, sizeof(exp));
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.read(sb.ctx, TBL, 1u, 1u, buf));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp + ISO_CD_BLOCK_BYTES, buf, ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_EQUAL_UINT32(2u, SD.calls);
    TEST_ASSERT_EQUAL_UINT64(14u, SD.rec[0].lba);
    TEST_ASSERT_EQUAL_UINT32(2u, SD.rec[0].n);
    TEST_ASSERT_EQUAL_UINT64(40u, SD.rec[1].lba);
    TEST_ASSERT_EQUAL_UINT32(2u, SD.rec[1].n);
    TEST_ASSERT_TRUE(SD.rec[1].dst == buf + 2u * ISO_SECTOR_BYTES);
    TEST_ASSERT_EQUAL_HEX8(0xA5u, buf[ISO_CD_BLOCK_BYTES]);
    free(buf);
    fx_teardown();
}

void test_backend_read_returns_the_blockdev_error_and_stops_at_the_first_failure(void) {
    const iso_extent_t ext[2] = {{10u, 6u}, {40u, 6u}};
    fx_init(5u, 3u * ISO_CD_BLOCK_BYTES, ext, 2u, true, false);
    const mmc_scsi_backend_t sb = scsi_be();
    uint8_t *buf = (uint8_t *)malloc(3u * ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_NOT_NULL(buf);

    SD.fail_at = 1;
    SD.fail_rc = -77;
    TEST_ASSERT_EQUAL_INT(-77, sb.read(sb.ctx, TBL, 0u, 3u, buf));
    TEST_ASSERT_EQUAL_UINT32(1u, SD.calls); /* 1 つ目で失敗したら、2 つ目は読まない */

    SD.calls = 0u;
    SD.fail_at = 2;
    SD.fail_rc = ISO_ERR_EPOCH;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_EPOCH, sb.read(sb.ctx, TBL, 0u, 3u, buf));
    TEST_ASSERT_EQUAL_UINT32(2u, SD.calls);

    /* カードの世代が変わった(ブロックデバイスが ISO_ERR_EPOCH を返す) */
    SD.fail_at = 0;
    SD.calls = 0u;
    SD.epoch = 6u;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_EPOCH, sb.read(sb.ctx, TBL, 0u, 3u, buf));
    free(buf);
    fx_teardown();
}

void test_backend_read_zero_fills_after_the_end_of_the_image_and_never_writes_past_the_request(void) {
    /* 2 ブロック + 300 バイト: 3 ブロック(容量)、9 セクタ。最後のセクタ(SD 上)の 300 バイト目以降は、他のデータ(0xAA)が入っている */
    const iso_extent_t ext[2] = {{100u, 5u}, {200u, 4u}};
    const uint64_t size = 2u * ISO_CD_BLOCK_BYTES + 300u;
    fx_init(1u, size, ext, 2u, true, false);
    memset(&SD.sectors[203][300], 0xAA, ISO_SECTOR_BYTES - 300u);
    const mmc_scsi_backend_t sb = scsi_be();
    uint8_t exp[3u * ISO_CD_BLOCK_BYTES];
    expected_image(ext, 2u, size, exp, sizeof(exp));

    /* 全体(ヒープに、ちょうどの大きさ: 範囲を超える書き込みを ASan が検出する) */
    uint8_t *buf = (uint8_t *)malloc(sizeof(exp));
    TEST_ASSERT_NOT_NULL(buf);
    memset(buf, 0xA5, sizeof(exp));
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.read(sb.ctx, TBL, 0u, 3u, buf));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp, buf, sizeof(exp));
    for (size_t i = (size_t)size; i < sizeof(exp); i++) {
        TEST_ASSERT_EQUAL_HEX8(0u, buf[i]);
    }
    free(buf);

    /* 最後の 1 ブロックだけ(有効なのは先頭の 300 バイト) */
    buf = (uint8_t *)malloc(ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_NOT_NULL(buf);
    memset(buf, 0xA5, ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.read(sb.ctx, TBL, 2u, 1u, buf));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp + 2u * ISO_CD_BLOCK_BYTES, buf, ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_EQUAL_HEX8(exp[2u * ISO_CD_BLOCK_BYTES + 299u], buf[299]);
    for (size_t i = 300u; i < ISO_CD_BLOCK_BYTES; i++) {
        TEST_ASSERT_EQUAL_HEX8(0u, buf[i]);
    }
    free(buf);

    /* 最後の 2 ブロック(最初のブロックは全部有効、次は 300 バイトだけ) */
    buf = (uint8_t *)malloc(2u * ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_NOT_NULL(buf);
    memset(buf, 0xA5, 2u * ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.read(sb.ctx, TBL, 1u, 2u, buf));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp + ISO_CD_BLOCK_BYTES, buf, 2u * ISO_CD_BLOCK_BYTES);
    free(buf);
    fx_teardown();
}

void test_backend_read_rejects_a_range_beyond_the_table_with_io_error_and_reads_nothing(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, false);
    const mmc_scsi_backend_t sb = scsi_be();
    uint8_t *buf = (uint8_t *)malloc(2u * ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_NOT_NULL(buf);
    memset(buf, 0xA5, 2u * ISO_CD_BLOCK_BYTES);
    /* 最後のブロックを含む範囲は読める */
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.read(sb.ctx, TBL, 4u, 2u, buf));
    SD.calls = 0u;
    /* 1 ブロックだけはみ出す: 何も読まない(最初のブロックだけを読んで進めない) */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, sb.read(sb.ctx, TBL, 5u, 2u, buf));
    TEST_ASSERT_EQUAL_UINT32(0u, SD.calls);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, sb.read(sb.ctx, TBL, 6u, 1u, buf));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, sb.read(sb.ctx, TBL, 100u, 1u, buf));
    TEST_ASSERT_EQUAL_UINT32(0u, SD.calls);
    free(buf);
    fx_teardown();
}

void test_backend_read_halves_the_step_when_the_spans_do_not_fit_and_keeps_going_to_the_end(void) {
    /* 20 ブロック(81920 バイトのうち、最後の 700 バイトは無い)= 79 セクタ。1 セクタずつの断片(1 つおき)= 79 個 > 64 */
    const uint64_t size = 20u * ISO_CD_BLOCK_BYTES - 700u;
    const uint32_t n_sectors = (uint32_t)((size + ISO_SECTOR_BYTES - 1u) / ISO_SECTOR_BYTES); /* 79 */
    TEST_ASSERT_EQUAL_UINT32(79u, n_sectors);
    iso_extent_t ext[80];
    for (uint32_t i = 0; i < 80u; i++) {
        ext[i].start_sector = 2u * i;
        ext[i].sector_count = 1u;
    }
    fx_init(1u, size, ext, 80u, true, false);
    const mmc_scsi_backend_t sb = scsi_be();
    uint8_t *exp = (uint8_t *)malloc(20u * ISO_CD_BLOCK_BYTES);
    uint8_t *buf = (uint8_t *)malloc(20u * ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_NOT_NULL(exp);
    TEST_ASSERT_NOT_NULL(buf);
    expected_image(ext, 80u, size, exp, 20u * ISO_CD_BLOCK_BYTES);
    memset(buf, 0xA5, 20u * ISO_CD_BLOCK_BYTES);

    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.read(sb.ctx, TBL, 0u, 20u, buf));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp, buf, 20u * ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_EQUAL_UINT32(79u, SD.calls); /* 1 セクタずつ、順に */
    for (uint32_t i = 0; i < 79u; i++) {
        TEST_ASSERT_EQUAL_UINT64(2u * i, SD.rec[i].lba);
        TEST_ASSERT_EQUAL_UINT32(1u, SD.rec[i].n);
        TEST_ASSERT_TRUE(SD.rec[i].dst == buf + (size_t)i * ISO_SECTOR_BYTES);
    }
    TEST_ASSERT_EQUAL_INT(1, LOCKS);

    /* 断片が 64 以下に収まる範囲(13 ブロック = 52 セクタ)は、そのまま 1 回で読む */
    SD.calls = 0u;
    memset(buf, 0xA5, 20u * ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_EQUAL_INT(ISO_OK, sb.read(sb.ctx, TBL, 3u, 13u, buf));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp + 3u * ISO_CD_BLOCK_BYTES, buf, 13u * ISO_CD_BLOCK_BYTES);
    TEST_ASSERT_EQUAL_UINT32(52u, SD.calls);
    free(buf);
    free(exp);
    fx_teardown();
}

/* ======================================================================
 * バックエンドの状態: init / set_blockdev / set_image / clear / sync / as_scsi
 * ====================================================================== */

void test_backend_init_clears_everything_and_stores_the_arguments(void) {
    iso_usb_msc_extent_backend_init(NULL, &BDEV, t_lock, t_unlock, &LOCK_TAG); /* クラッシュしない */
    iso_usb_msc_extent_backend_t be;
    memset(&be, 0xA5, sizeof(be));
    iso_usb_msc_extent_backend_init(&be, &BDEV, t_lock, t_unlock, &LOCK_TAG);
    TEST_ASSERT_TRUE(be.bdev == &BDEV);
    TEST_ASSERT_TRUE(be.lock == t_lock);
    TEST_ASSERT_TRUE(be.unlock == t_unlock);
    TEST_ASSERT_TRUE(be.lock_ctx == (void *)&LOCK_TAG);
    TEST_ASSERT_NULL(be.table);
    TEST_ASSERT_EQUAL_UINT32(0u, be.media_id);
    TEST_ASSERT_NULL(be.resolver.ctx);
    TEST_ASSERT_NULL(be.resolver.lookup_size_locked);
    TEST_ASSERT_NULL(be.resolver.acquire_table_locked);
    TEST_ASSERT_EQUAL_INT(0, LOCKS); /* init はロックしない */

    /* ロックなし(NULL)でも動く */
    memset(&be, 0xA5, sizeof(be));
    iso_usb_msc_extent_backend_init(&be, NULL, NULL, NULL, NULL);
    TEST_ASSERT_NULL(be.bdev);
    TEST_ASSERT_NULL(be.lock);
    TEST_ASSERT_NULL(be.unlock);
    TEST_ASSERT_NULL(be.lock_ctx);
    iso_usb_msc_extent_backend_set_blockdev(&be, &BDEV);
    TEST_ASSERT_TRUE(be.bdev == &BDEV);
    uint64_t out = 9u;
    const mmc_scsi_backend_t sb = iso_usb_msc_extent_backend_as_scsi(&be);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, sb.size_bytes(sb.ctx, 1u, &out));
}

void test_set_blockdev_replaces_the_pointer_under_the_lock_and_ignores_a_null_backend(void) {
    iso_usb_msc_extent_backend_set_blockdev(NULL, &BDEV); /* クラッシュしない */
    iso_usb_msc_extent_backend_t be;
    iso_usb_msc_extent_backend_init(&be, NULL, t_lock, t_unlock, &LOCK_TAG);
    iso_usb_msc_extent_backend_set_blockdev(&be, &BDEV);
    TEST_ASSERT_TRUE(be.bdev == &BDEV);
    TEST_ASSERT_EQUAL_INT(1, LOCKS);
    TEST_ASSERT_EQUAL_INT(1, UNLOCKS);
    iso_usb_msc_extent_backend_set_blockdev(&be, NULL);
    TEST_ASSERT_NULL(be.bdev);
    TEST_ASSERT_EQUAL_INT(2, LOCKS);
    TEST_ASSERT_EQUAL_INT(2, UNLOCKS);
    TEST_ASSERT_EQUAL_INT(0, BAD_LOCK_CTX);
}

void test_set_image_takes_its_own_reference_replaces_the_old_one_outside_the_lock_and_clear_resets_the_id(void) {
    iso_usb_msc_extent_backend_set_image(NULL, 5u, NULL); /* クラッシュしない */
    const iso_extent_t e1[1] = {{0u, 4u}};
    const iso_extent_t e2[1] = {{8u, 4u}};
    sd_reset(1u);
    iso_extent_table_t *t1 = build_table(1u, ISO_CD_BLOCK_BYTES, e1, 1u);
    iso_extent_table_t *t2 = build_table(1u, ISO_CD_BLOCK_BYTES, e2, 1u);
    iso_usb_msc_extent_backend_t be;
    iso_usb_msc_extent_backend_init(&be, &BDEV, t_lock, t_unlock, &LOCK_TAG);

    iso_usb_msc_extent_backend_set_image(&be, 11u, t1);
    TEST_ASSERT_TRUE(be.table == t1);
    TEST_ASSERT_EQUAL_UINT32(11u, be.media_id);
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(t1));
    TEST_ASSERT_EQUAL_INT(1, LOCKS);

    /* 同じ表をもう一度: 参照は増減しない(先に ref してから、古い表を unref する。0 を経由しない) */
    iso_usb_msc_extent_backend_set_image(&be, 12u, t1);
    TEST_ASSERT_TRUE(be.table == t1);
    TEST_ASSERT_EQUAL_UINT32(12u, be.media_id);
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(t1));

    /* 別の表: 古い表は放し、新しい表は参照を 1 つ取る */
    iso_usb_msc_extent_backend_set_image(&be, 13u, t2);
    TEST_ASSERT_TRUE(be.table == t2);
    TEST_ASSERT_EQUAL_UINT32(13u, be.media_id);
    TEST_ASSERT_EQUAL_UINT32(1u, iso_extent_table_refs(t1));
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(t2));

    /* NULL: 表を外す。id は、渡した値によらず 0 */
    iso_usb_msc_extent_backend_set_image(&be, 99u, NULL);
    TEST_ASSERT_NULL(be.table);
    TEST_ASSERT_EQUAL_UINT32(0u, be.media_id);
    TEST_ASSERT_EQUAL_UINT32(1u, iso_extent_table_refs(t2));

    /* clear は、(0, NULL) と同じ */
    iso_usb_msc_extent_backend_set_image(&be, 21u, t2);
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(t2));
    iso_usb_msc_extent_backend_clear(&be);
    TEST_ASSERT_NULL(be.table);
    TEST_ASSERT_EQUAL_UINT32(0u, be.media_id);
    TEST_ASSERT_EQUAL_UINT32(1u, iso_extent_table_refs(t2));
    iso_usb_msc_extent_backend_clear(NULL); /* クラッシュしない */

    /* 最後の参照の解放(表の解放)は、ロックの外 */
    iso_usb_msc_extent_backend_set_image(&be, 31u, t1);
    TEST_ASSERT_EQUAL_UINT32(1u, iso_extent_table_unref(t1)); /* テストの参照を放す。残りはバックエンドの 1 */
    iso_usb_msc_extent_backend_clear(&be);                    /* ここで t1 が解放される */
    TEST_ASSERT_EQUAL_INT(0, A_FREE_UNDER_LOCK);
    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(t2));
    TEST_ASSERT_EQUAL_INT(0, A_LIVE);
    TEST_ASSERT_EQUAL_INT(0, A_FREE_UNDER_LOCK);
    TEST_ASSERT_EQUAL_INT(0, LOCK_DEPTH);
    TEST_ASSERT_EQUAL_INT(LOCKS, UNLOCKS);
    TEST_ASSERT_EQUAL_INT(0, BAD_LOCK_CTX);
}

void test_sync_media_backend_clears_the_backend_only_when_the_media_is_not_loaded(void) {
    fx_init(1u, 6u * ISO_CD_BLOCK_BYTES, EXT6, 1u, true, false);
    iso_media_t media;
    iso_media_init(&media, NULL, NULL);

    iso_usb_msc_sync_media_backend(NULL, &BE); /* media が NULL: 何もしない */
    TEST_ASSERT_TRUE(BE.table == TBL);
    iso_usb_msc_sync_media_backend(&media, NULL); /* クラッシュしない */

    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&media, MEDIA_ID, false));
    iso_usb_msc_sync_media_backend(&media, &BE); /* ロード済み: そのまま */
    TEST_ASSERT_TRUE(BE.table == TBL);
    TEST_ASSERT_EQUAL_UINT32(MEDIA_ID, BE.media_id);
    TEST_ASSERT_EQUAL_UINT32(2u, iso_extent_table_refs(TBL));

    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&media, false));
    iso_usb_msc_sync_media_backend(&media, &BE); /* 空: 表を放す */
    TEST_ASSERT_NULL(BE.table);
    TEST_ASSERT_EQUAL_UINT32(0u, BE.media_id);
    TEST_ASSERT_EQUAL_UINT32(1u, iso_extent_table_refs(TBL));
    iso_usb_msc_sync_media_backend(&media, &BE); /* 2 回目も安全 */
    TEST_ASSERT_EQUAL_UINT32(1u, iso_extent_table_refs(TBL));
    fx_teardown();
}

void test_as_scsi_binds_the_backend_and_all_four_functions(void) {
    iso_usb_msc_extent_backend_t be;
    iso_usb_msc_extent_backend_init(&be, NULL, NULL, NULL, NULL);
    const mmc_scsi_backend_t sb = iso_usb_msc_extent_backend_as_scsi(&be);
    TEST_ASSERT_TRUE(sb.ctx == (void *)&be);
    TEST_ASSERT_NOT_NULL(sb.size_bytes);
    TEST_ASSERT_NOT_NULL(sb.acquire);
    TEST_ASSERT_NOT_NULL(sb.read);
    TEST_ASSERT_NOT_NULL(sb.release);
    /* mmc_scsi_init が受け付ける(4 つの関数が非 NULL) */
    iso_media_t media;
    iso_media_init(&media, NULL, NULL);
    mmc_scsi_t scsi;
    TEST_ASSERT_EQUAL_INT(0, mmc_scsi_init(&scsi, &media, &sb));
}

/* ======================================================================
 * 世代の一致の検査と、切断
 * ====================================================================== */

void test_epoch_matches_compares_the_table_epoch_with_the_blockdev_epoch_through_its_ctx(void) {
    sd_reset(9u);
    const iso_extent_t e[1] = {{0u, 4u}};
    iso_extent_table_t *t9 = build_table(9u, ISO_CD_BLOCK_BYTES, e, 1u);
    iso_extent_table_t *t0 = build_table(0u, ISO_CD_BLOCK_BYTES, e, 1u);

    TEST_ASSERT_TRUE(iso_usb_msc_table_epoch_matches(&BDEV, t9));
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(&BDEV, t0));
    TEST_ASSERT_EQUAL_UINT(0u, SD.bad_ctx); /* epoch には bdev->ctx を渡す */

    SD.epoch = 0u; /* 世代 0 同士も一致 */
    TEST_ASSERT_TRUE(iso_usb_msc_table_epoch_matches(&BDEV, t0));
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(&BDEV, t9));
    SD.epoch = 10u; /* 表の世代 + 1、- 1 は一致しない */
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(&BDEV, t9));
    SD.epoch = 8u;
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(&BDEV, t9));

    iso_blockdev_t no_epoch = BDEV;
    no_epoch.epoch = NULL;
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(&no_epoch, t0)); /* 世代を見られない: 不一致 */
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(NULL, t0));
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(&BDEV, NULL));
    SD.epoch = 0u; /* 表がないときの世代は 0(iso_extent_table_epoch(NULL))。ブロックデバイスの世代が 0 でも、一致とは言わない */
    TEST_ASSERT_FALSE(iso_usb_msc_table_epoch_matches(&BDEV, NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(t9));
    TEST_ASSERT_EQUAL_UINT32(0u, iso_extent_table_unref(t0));
    TEST_ASSERT_EQUAL_INT(0, A_LIVE);
}

void test_on_disconnect_media_is_a_bus_reset_that_keeps_the_media_loaded(void) {
    iso_usb_msc_on_disconnect_media(NULL); /* クラッシュしない */
    iso_media_t media;
    iso_media_init(&media, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&media, 7u, false));
    uint8_t asc = 0u;
    uint8_t ascq = 0u;
    TEST_ASSERT_TRUE(iso_media_take_unit_attention(&media, &asc, &ascq)); /* 28h/00h を消す */
    TEST_ASSERT_TRUE(iso_media_set_prevent(&media, true));
    TEST_ASSERT_TRUE(iso_media_prevent_removal(&media));
    const uint32_t gen = iso_media_abort_generation(&media);
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(&media));

    iso_usb_msc_on_disconnect_media(&media);
    TEST_ASSERT_FALSE(iso_media_prevent_removal(&media));         /* 取り外しの防止を解く */
    TEST_ASSERT_EQUAL_UINT32(gen + 1u, iso_media_abort_generation(&media)); /* 読み出しの世代を進める */
    TEST_ASSERT_TRUE(iso_media_unit_attention_pending(&media));   /* 29h/00h */
    TEST_ASSERT_TRUE(iso_media_take_unit_attention(&media, &asc, &ascq));
    TEST_ASSERT_EQUAL_HEX8(0x29u, asc);
    TEST_ASSERT_EQUAL_HEX8(0x00u, ascq);
    TEST_ASSERT_TRUE(iso_media_is_loaded(&media)); /* ロード済みのまま */
    uint32_t id = 0u;
    TEST_ASSERT_TRUE(iso_media_get_id(&media, &id));
    TEST_ASSERT_EQUAL_UINT32(7u, id);

    /* 空のメディアでも、同じ(防止は元からない)。世代だけ進む */
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&media, false));
    const uint32_t gen2 = iso_media_abort_generation(&media);
    iso_usb_msc_on_disconnect_media(&media);
    TEST_ASSERT_EQUAL_UINT32(gen2 + 1u, iso_media_abort_generation(&media));
    TEST_ASSERT_FALSE(iso_media_is_loaded(&media));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_exec_cmd_ignores_null_arguments_and_leaves_the_result_alone);
    RUN_TEST(test_exec_cmd_with_an_unknown_kind_fails_with_the_seq_and_nothing_else);
    RUN_TEST(test_exec_cmd_execute_copies_the_scsi_result_into_the_bot_result);
    RUN_TEST(test_exec_cmd_read_reports_the_range_and_the_direction_from_the_block_count);
    RUN_TEST(test_exec_cmd_maps_the_bot_direction_to_the_mmc_direction);
    RUN_TEST(test_exec_cmd_read_blocks_reads_the_range_and_reports_only_good);
    RUN_TEST(test_exec_cmd_read_blocks_rejects_a_lba_above_32_bits_without_reading_but_passes_the_max_32_bit_lba);
    RUN_TEST(test_backend_size_bytes_validates_arguments_and_looks_up_the_installed_table);
    RUN_TEST(test_backend_acquire_validates_arguments_takes_a_reference_and_clears_the_handle_first);
    RUN_TEST(test_backend_release_unrefs_a_handle_and_ignores_null_and_the_ctx);
    RUN_TEST(test_resolver_size_bytes_takes_precedence_passes_the_id_and_returns_its_result_without_the_backend_lock);
    RUN_TEST(test_resolver_acquire_returns_the_table_the_resolver_referenced_and_does_not_add_a_reference);
    RUN_TEST(test_set_resolver_copies_the_whole_struct_and_null_clears_it);
    RUN_TEST(test_backend_read_rejects_null_arguments_in_the_documented_order);
    RUN_TEST(test_backend_read_issues_one_read_per_span_with_the_table_epoch_and_contiguous_destinations);
    RUN_TEST(test_backend_read_returns_the_blockdev_error_and_stops_at_the_first_failure);
    RUN_TEST(test_backend_read_zero_fills_after_the_end_of_the_image_and_never_writes_past_the_request);
    RUN_TEST(test_backend_read_rejects_a_range_beyond_the_table_with_io_error_and_reads_nothing);
    RUN_TEST(test_backend_read_halves_the_step_when_the_spans_do_not_fit_and_keeps_going_to_the_end);
    RUN_TEST(test_backend_init_clears_everything_and_stores_the_arguments);
    RUN_TEST(test_set_blockdev_replaces_the_pointer_under_the_lock_and_ignores_a_null_backend);
    RUN_TEST(test_set_image_takes_its_own_reference_replaces_the_old_one_outside_the_lock_and_clear_resets_the_id);
    RUN_TEST(test_sync_media_backend_clears_the_backend_only_when_the_media_is_not_loaded);
    RUN_TEST(test_as_scsi_binds_the_backend_and_all_four_functions);
    RUN_TEST(test_epoch_matches_compares_the_table_epoch_with_the_blockdev_epoch_through_its_ctx);
    RUN_TEST(test_on_disconnect_media_is_a_bus_reset_that_keeps_the_media_loaded);
    return UNITY_END();
}

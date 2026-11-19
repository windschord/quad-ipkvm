#ifndef MM_COMMON_H
#define MM_COMMON_H

/*
 * mmc_scsi のテスト共通の部品: メモリ上のモックのバックエンド（参照の釈放を数える）、フィクスチャ、検査の補助。
 * 各テストの .c が include する（static inline だけ。ctest の自動発見は test_*.c だけを拾う）。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "iso_media.h"
#include "mmc_scsi.h"
#include "unity.h"

/* イメージ（モックのバックエンドが知っているもの）。size_bytes は 2048 の倍数でないものを含む。 */
#define ID_A 0xA1B2C3D4u
#define SIZE_A 204900ull /* 100 ブロック + 100 バイト -> capacity_blocks = 101、最終 LBA = 100 */
#define CAP_A 101u
#define ID_B 0x0000BEEFu
#define SIZE_B 102400ull /* ちょうど 50 ブロック */
#define CAP_B 50u
#define ID_BIG 0x0B16B16Au
#define SIZE_BIG 2684354560ull /* 2.5 GiB = 1,310,720 ブロック */
#define CAP_BIG 1310720u
#define ID_HUGE 0x0DEADBEEu
#define SIZE_HUGE 20000000000000ull /* 約 18 TiB。capacity_blocks が 32 ビットを超える */
#define ID_C 0xC0FFEE00u
#define SIZE_C 10240000ull /* 5000 ブロック（ちょうど）。MSF で分と秒が 0 でない（5000 + 150 = 01:08:50） */
#define CAP_C 5000u
#define ID_BIGP 0xB16B00B5u
#define SIZE_BIGP 5368709220ull /* 5 GiB + 100 バイト。最後のブロックが部分的で、LBA が 2^21 以上（バイトのオフセットが 32 ビットを超える） */
#define CAP_BIGP 2621441u
#define ID_ZERO 0x0000002Au /* size_bytes が成功（0）で、大きさ 0 を返す（契約違反に近い入力） */
#define ID_UNKNOWN 0x12345678u      /* size_bytes が ISO_ERR_NOT_FOUND を返す */
#define GARBAGE 0xEE                /* バックエンドが、size_bytes 以降に書くゴミ */

#define MOCK_MAX_LIVE 8

typedef struct {
    int size_rc;    /* 0 以外: size_bytes がこのエラーを返す */
    int acquire_rc; /* 0 以外: acquire がこのエラーを返す */
    int read_rc;    /* 0 以外: read がこのエラーを返す（read_fail_after 回の成功の後） */
    unsigned read_fail_after;
    unsigned size_calls, acquires, releases, reads;
    unsigned bad_use; /* 生きていないハンドルで read / release された回数 */
    int live;         /* 参照の数（acquires - releases） */
    uintptr_t next_handle;
    uintptr_t live_handles[MOCK_MAX_LIVE];
    uint32_t live_ids[MOCK_MAX_LIVE];
    uint32_t last_lba, last_blocks;
    int bump_epoch_on_read; /* 1: read の途中で media の世代を進める（リセットの模擬）。media は fixture のもの */
    void (*on_read)(void);  /* read の最中（バックエンドの read の中）に呼ぶ関数（command_done など、別のタスクの模擬） */
    int in_read;            /* read の最中なら 1 */
    unsigned release_in_read; /* read の最中に release が呼ばれた回数（0 でなければならない） */
} mock_t;

static iso_media_t MEDIA;
static mmc_scsi_t SCSI;
static mock_t MOCK;
static unsigned FORCE_EJECT_CALLS;

static inline uint64_t mock_size_of(uint32_t id) {
    switch (id) {
        case ID_A: return SIZE_A;
        case ID_B: return SIZE_B;
        case ID_BIG: return SIZE_BIG;
        case ID_HUGE: return SIZE_HUGE;
        case ID_C: return SIZE_C;
        case ID_BIGP: return SIZE_BIGP;
        default: return 0;
    }
}

/* バックエンドの「ファイルの中身」: オフセットから決まる 1 バイト */
static inline uint8_t mock_pattern(uint32_t id, uint64_t off) {
    return (uint8_t)((off * 31u + (off >> 8) * 7u + (id & 0xFFu) + 3u) & 0xFFu);
}

static inline int mock_size_bytes(void *ctx, uint32_t id, uint64_t *out) {
    mock_t *m = (mock_t *)ctx;
    m->size_calls++;
    if (m->size_rc != 0) {
        *out = 0x7777777777ull; /* 失敗のとき、*out の値は使ってはならない（ゴミを入れておく） */
        return m->size_rc;
    }
    if (id == ID_ZERO) {
        *out = 0;
        return 0;
    }
    if (mock_size_of(id) == 0) return ISO_ERR_NOT_FOUND;
    *out = mock_size_of(id);
    return 0;
}

static inline int mock_acquire(void *ctx, uint32_t id, void **handle) {
    mock_t *m = (mock_t *)ctx;
    if (m->acquire_rc != 0) return m->acquire_rc;
    for (int i = 0; i < MOCK_MAX_LIVE; i++) {
        if (m->live_handles[i] == 0) {
            m->live_handles[i] = ++m->next_handle;
            m->live_ids[i] = id;
            m->acquires++;
            m->live++;
            *handle = (void *)m->live_handles[i];
            return 0;
        }
    }
    return ISO_ERR_BUSY;
}

static inline int mock_find_live(mock_t *m, void *handle) {
    for (int i = 0; i < MOCK_MAX_LIVE; i++) {
        if (m->live_handles[i] != 0 && (void *)m->live_handles[i] == handle) return i;
    }
    return -1;
}

static inline int mock_read_body(void *ctx, void *handle, uint32_t lba, uint32_t blocks, uint8_t *buf) {
    mock_t *m = (mock_t *)ctx;
    int idx = mock_find_live(m, handle);
    m->reads++;
    m->last_lba = lba;
    m->last_blocks = blocks;
    if (idx < 0) {
        m->bad_use++;
        return ISO_ERR_INVALID;
    }
    if (m->bump_epoch_on_read) iso_media_bus_reset(&MEDIA);
    if (m->on_read) m->on_read();
    if (m->read_rc != 0 && m->reads > m->read_fail_after) {
        /* 失敗のときは、途中まで書いたふりをする（呼び出し側が 0 で埋めること） */
        if (blocks > 0) buf[0] = 0x5A;
        return m->read_rc;
    }
    {
        uint32_t id = m->live_ids[idx];
        uint64_t size = mock_size_of(id);
        for (uint64_t i = 0; i < (uint64_t)blocks * 2048u; i++) {
            uint64_t off = (uint64_t)lba * 2048u + i;
            buf[i] = off < size ? mock_pattern(id, off) : (uint8_t)GARBAGE;
        }
    }
    return 0;
}

static inline int mock_read(void *ctx, void *handle, uint32_t lba, uint32_t blocks, uint8_t *buf) {
    mock_t *m = (mock_t *)ctx;
    int rc;
    m->in_read++;
    rc = mock_read_body(ctx, handle, lba, blocks, buf);
    m->in_read--;
    return rc;
}

static inline void mock_release(void *ctx, void *handle) {
    mock_t *m = (mock_t *)ctx;
    if (m->in_read) m->release_in_read++;
    int idx = mock_find_live(m, handle);
    if (idx < 0) {
        m->bad_use++;
        return;
    }
    m->live_handles[idx] = 0;
    m->releases++;
    m->live--;
}

static inline void on_force_eject(void *ctx) { (void)ctx; FORCE_EJECT_CALLS++; }

static inline void fx_init(void) {
    mmc_scsi_backend_t be;
    memset(&MOCK, 0, sizeof(MOCK));
    FORCE_EJECT_CALLS = 0;
    iso_media_init(&MEDIA, on_force_eject, NULL);
    be.ctx = &MOCK;
    be.size_bytes = mock_size_bytes;
    be.acquire = mock_acquire;
    be.read = mock_read;
    be.release = mock_release;
    TEST_ASSERT_EQUAL_INT(0, mmc_scsi_init(&SCSI, &MEDIA, &be));
}

/* メディアを入れて、UNIT ATTENTION とイベントを消費した状態にする（コマンドの応答に、変更の通知が混ざらないように） */
static inline void fx_mount_quiet(uint32_t id) {
    uint8_t a, q;
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, id, false));
    while (iso_media_take_unit_attention(&MEDIA, &a, &q)) {
    }
    (void)iso_media_take_event(&MEDIA);
}

/* 実行と結果 */
static uint8_t OUT[8192];
static mmc_result_t RES;

/* CBW の dCBWDataTransferLength: IN なら十分大きな値（8192）、それ以外は 0 */
static inline void exec_expected(const uint8_t *cdb, size_t len, mmc_dir_t dir, uint32_t expected_len) {
    memset(OUT, 0xA5, sizeof(OUT));
    TEST_ASSERT_EQUAL_INT(0, mmc_scsi_execute(&SCSI, cdb, len, dir, expected_len, OUT, sizeof(OUT), &RES));
}
static inline void exec_dir(const uint8_t *cdb, size_t len, mmc_dir_t dir) {
    exec_expected(cdb, len, dir, dir == MMC_DIR_IN ? 8192u : 0u);
}
static inline void exec(const uint8_t *cdb, size_t len) { exec_dir(cdb, len, MMC_DIR_IN); }

#define EXEC(...)                                          \
    do {                                                   \
        const uint8_t cdb_[] = {__VA_ARGS__};              \
        exec(cdb_, sizeof(cdb_));                          \
    } while (0)

static inline void expect_good(void) {
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, RES.status);
    TEST_ASSERT_EQUAL_UINT8(0, RES.sense.key);
    TEST_ASSERT_EQUAL_UINT8(0, RES.sense.asc);
    TEST_ASSERT_EQUAL_UINT8(0, RES.sense.ascq);
}

static inline void expect_cc(uint8_t key, uint8_t asc, uint8_t ascq) {
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, RES.status);
    TEST_ASSERT_EQUAL_HEX8(key, RES.sense.key);
    TEST_ASSERT_EQUAL_HEX8(asc, RES.sense.asc);
    TEST_ASSERT_EQUAL_HEX8(ascq, RES.sense.ascq);
    TEST_ASSERT_EQUAL_UINT32(0, RES.data_len);
    TEST_ASSERT_FALSE(RES.read.is_read);
}

/* 応答のバイト列（手書きの期待値）と完全に一致し、その後ろを触っていない */
#define EXPECT_DATA(exp)                                                                    \
    do {                                                                                    \
        expect_good();                                                                      \
        TEST_ASSERT_EQUAL_UINT32(sizeof(exp), RES.data_len);                                \
        TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, OUT, sizeof(exp));                                \
        TEST_ASSERT_EQUAL_HEX8(0xA5, OUT[sizeof(exp)]);                                     \
    } while (0)

static inline void expect_no_data_good(void) {
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(0, RES.data_len);
    TEST_ASSERT_EQUAL_HEX8(0xA5, OUT[0]);
}

/* READ(10) の CDB */
static inline void exec_read10(uint32_t lba, uint16_t blocks) {
    uint8_t cdb[10] = {0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba, 0,
                       (uint8_t)(blocks >> 8), (uint8_t)blocks, 0};
    exec(cdb, sizeof(cdb));
}

static inline void exec_read12(uint32_t lba, uint32_t blocks) {
    uint8_t cdb[12] = {0xA8, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                       (uint8_t)(blocks >> 24), (uint8_t)(blocks >> 16), (uint8_t)(blocks >> 8), (uint8_t)blocks, 0, 0};
    exec(cdb, sizeof(cdb));
}

#endif /* MM_COMMON_H */

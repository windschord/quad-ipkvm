/*
 * mmc_scsi: メディアの大きさ・内容に依存するコマンド（READ CAPACITY、READ FORMAT CAPACITIES、READ(10)/(12)、
 * mmc_scsi_read_blocks、参照の釈放、READ TOC、GET CONFIGURATION、GET EVENT STATUS NOTIFICATION、
 * READ DISC INFORMATION、READ TRACK INFORMATION）。応答のバイト列は手書きの期待値。
 * 出典と、確認できなかった項目は docs/MMC_SCSI_TESTS.md 1 章。
 *
 * イメージ A: size = 204900 バイト = 100 ブロック + 100 バイト -> capacity_blocks = 101、最終 LBA = 100 = 0x64。
 * イメージ B: size = 102400 バイト = 50 ブロック（ちょうど）-> 最終 LBA = 49 = 0x31。
 */
#include "mm_common.h"

void setUp(void) { fx_init(); }
void tearDown(void) {}

/* ---------------------------------------------------------------- READ CAPACITY(10) */
static void test_read_capacity_10_exact_bytes(void) {
    static const uint8_t a[8] = {0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x08, 0x00};
    static const uint8_t b[8] = {0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x08, 0x00};
    static const uint8_t big[8] = {0x00, 0x13, 0xFF, 0xFF, 0x00, 0x00, 0x08, 0x00};
    static const uint8_t huge[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x08, 0x00};
    fx_mount_quiet(ID_A);
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    EXPECT_DATA(a);
    fx_mount_quiet(ID_B);
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    EXPECT_DATA(b);
    fx_mount_quiet(ID_BIG);
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    EXPECT_DATA(big);
    fx_mount_quiet(ID_HUGE); /* 最終 LBA が 32 ビットを超える（約 18 TiB）: 0xFFFFFFFF。BIGP（5 GiB + 100 B）は、最終 LBA が 0x00280000 で、丸めない */
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    EXPECT_DATA(huge);
}

static void test_read_capacity_10_without_media_is_not_ready(void) {
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    expect_cc(0x2, 0x3A, 0x00);
}

static void test_read_capacity_10_pmi_and_reserved_fields(void) {
    static const uint8_t a[8] = {0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x08, 0x00};
    fx_mount_quiet(ID_A);
    EXEC(0x25, 0, 0, 0, 0, 100, 0, 0, 0x01, 0); /* PMI = 1、LBA = 最終 LBA */
    EXPECT_DATA(a);
    EXEC(0x25, 0, 0, 0, 0, 101, 0, 0, 0x01, 0); /* LBA が範囲外 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x25, 0, 0, 0, 0, 5, 0, 0, 0x00, 0); /* PMI = 0 で LBA が 0 でない */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x25, 0x01, 0, 0, 0, 0, 0, 0, 0x00, 0); /* RELADR */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x25, 0, 0, 0, 0, 0, 0x01, 0, 0x00, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0x02, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x04);
    expect_cc(0x5, 0x24, 0x00);
}

/* ---------------------------------------------------------------- READ FORMAT CAPACITIES */
static void test_read_format_capacities_exact_bytes(void) {
    static const uint8_t with_media[12] = {0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x65, 0x02, 0x00, 0x08, 0x00};
    static const uint8_t no_media[12] = {0x00, 0x00, 0x00, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0x00, 0x08, 0x00};
    EXEC(0x23, 0, 0, 0, 0, 0, 0, 0x00, 0xFC, 0);
    EXPECT_DATA(no_media); /* メディアなしでも失敗にしない */
    fx_mount_quiet(ID_A);
    EXEC(0x23, 0, 0, 0, 0, 0, 0, 0x00, 0xFC, 0);
    EXPECT_DATA(with_media);
    EXEC(0x23, 0, 0, 0, 0, 0, 0, 0x00, 4, 0);
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(4, RES.data_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(with_media, OUT, 4);
    TEST_ASSERT_EQUAL_HEX8(0xA5, OUT[4]);
    EXEC(0x23, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0); /* 割り当て長 256（上位バイトが 1） */
    EXPECT_DATA(with_media);
    EXEC(0x23, 0x01, 0, 0, 0, 0, 0, 0x00, 0xFC, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x23, 0, 0, 0, 0, 0, 0x01, 0x00, 0xFC, 0);
    expect_cc(0x5, 0x24, 0x00);
}

/* ---------------------------------------------------------------- READ(10) / READ(12) */
static void test_read10_reports_range_and_holds_one_reference(void) {
    fx_mount_quiet(ID_A);
    exec_read10(3, 5);
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(0, RES.data_len); /* データは作らない */
    TEST_ASSERT_TRUE(RES.read.is_read);
    TEST_ASSERT_EQUAL_UINT32(3, RES.read.lba);
    TEST_ASSERT_EQUAL_UINT32(5, RES.read.blocks);
    TEST_ASSERT_EQUAL_HEX8(0xA5, OUT[0]);
    TEST_ASSERT_TRUE(mmc_scsi_holds_reference(&SCSI));
    TEST_ASSERT_EQUAL_UINT(1, MOCK.acquires);
    TEST_ASSERT_EQUAL_INT(1, MOCK.live);
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_FALSE(mmc_scsi_holds_reference(&SCSI));
    TEST_ASSERT_EQUAL_UINT(1, MOCK.releases);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
    TEST_ASSERT_EQUAL_UINT(0, MOCK.bad_use);
}

static void test_read12_reports_range_and_holds_one_reference(void) {
    fx_mount_quiet(ID_A);
    exec_read12(0x10, 0x30);
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(0, RES.data_len);
    TEST_ASSERT_TRUE(RES.read.is_read);
    TEST_ASSERT_EQUAL_UINT32(0x10, RES.read.lba);
    TEST_ASSERT_EQUAL_UINT32(0x30, RES.read.blocks);
    TEST_ASSERT_TRUE(mmc_scsi_holds_reference(&SCSI));
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
}

static void test_read_range_checks_at_every_boundary(void) {
    fx_mount_quiet(ID_A); /* capacity_blocks = 101 */
    exec_read10(100, 1); /* 最終ブロック */
    expect_good();
    mmc_scsi_command_done(&SCSI);
    exec_read10(0, 101); /* ちょうど全体 */
    expect_good();
    mmc_scsi_command_done(&SCSI);
    exec_read10(0, 102);
    expect_cc(0x5, 0x21, 0x00); /* ILLEGAL REQUEST / LOGICAL BLOCK ADDRESS OUT OF RANGE */
    exec_read10(101, 1);
    expect_cc(0x5, 0x21, 0x00);
    exec_read10(100, 2);
    expect_cc(0x5, 0x21, 0x00);
    exec_read10(0xFFFFFFFFu, 2); /* LBA + 転送長が 32 ビットを巻き戻る */
    expect_cc(0x5, 0x21, 0x00);
    exec_read10(0xFFFFFFFFu, 1);
    expect_cc(0x5, 0x21, 0x00);
    exec_read12(1, 0xFFFFFFFFu);
    expect_cc(0x5, 0x21, 0x00);
    exec_read12(0xFFFFFFFFu, 0xFFFFFFFFu);
    expect_cc(0x5, 0x21, 0x00);
    exec_read12(0x80000000u, 0x80000000u);
    expect_cc(0x5, 0x21, 0x00);
    TEST_ASSERT_EQUAL_UINT(2, MOCK.acquires); /* 範囲外では参照を取らない */
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
}

static void test_read_uses_the_full_width_of_the_lba_and_length_fields(void) {
    fx_mount_quiet(ID_A);
    exec_read10(0x00010000u, 1); /* 上位 16 ビットだけが 0 でない LBA */
    expect_cc(0x5, 0x21, 0x00);
    exec_read10(0x01000000u, 1);
    expect_cc(0x5, 0x21, 0x00);
    exec_read10(0, 0x0100); /* 転送長の上位バイトだけが 0 でない */
    expect_cc(0x5, 0x21, 0x00);
    exec_read12(0, 0x00010001u); /* 下位 16 ビットだけなら範囲内になってしまう値 */
    expect_cc(0x5, 0x21, 0x00);
    exec_read12(0, 0x01000001u);
    expect_cc(0x5, 0x21, 0x00);
    exec_read12(0, 0x00000100u);
    expect_cc(0x5, 0x21, 0x00);
    TEST_ASSERT_EQUAL_UINT(0, MOCK.acquires);
}

static void test_read_zero_blocks_is_good_without_reference_unless_lba_is_beyond_capacity(void) {
    fx_mount_quiet(ID_A);
    exec_read10(0, 0);
    expect_good();
    TEST_ASSERT_TRUE(RES.read.is_read);
    TEST_ASSERT_EQUAL_UINT32(0, RES.read.blocks);
    exec_read10(101, 0); /* LBA = capacity_blocks: 転送なし。エラーではない */
    expect_good();
    exec_read12(100, 0);
    expect_good();
    TEST_ASSERT_EQUAL_UINT(0, MOCK.acquires);
    TEST_ASSERT_FALSE(mmc_scsi_holds_reference(&SCSI));
    exec_read10(102, 0);
    expect_cc(0x5, 0x21, 0x00);
    exec_read12(0xFFFFFFFFu, 0);
    expect_cc(0x5, 0x21, 0x00);
}

static void test_read_without_media_is_not_ready_and_takes_no_reference(void) {
    exec_read10(0, 1);
    expect_cc(0x2, 0x3A, 0x00);
    exec_read12(0, 1);
    expect_cc(0x2, 0x3A, 0x00);
    TEST_ASSERT_EQUAL_UINT(0, MOCK.acquires);
}

static void test_read_flags_and_reserved_bits(void) {
    fx_mount_quiet(ID_A);
    {
        uint8_t c[10] = {0x28, 0x18, 0, 0, 0, 0, 0x05, 0, 1, 0}; /* DPO、FUA、GROUP NUMBER = 5 は受け付ける */
        exec(c, 10);
        expect_good();
        mmc_scsi_command_done(&SCSI);
        c[1] = 0x20; /* RDPROTECT */
        exec(c, 10);
        expect_cc(0x5, 0x24, 0x00);
        c[1] = 0x04;
        exec(c, 10);
        expect_cc(0x5, 0x24, 0x00);
        c[1] = 0x01; /* RELADR */
        exec(c, 10);
        expect_cc(0x5, 0x24, 0x00);
        c[1] = 0x80;
        exec(c, 10);
        expect_cc(0x5, 0x24, 0x00);
        c[1] = 0x40;
        exec(c, 10);
        expect_cc(0x5, 0x24, 0x00);
        c[1] = 0x02; /* FUA_NV は受け付ける */
        exec(c, 10);
        expect_good();
        mmc_scsi_command_done(&SCSI);
        c[1] = 0;
        c[6] = 0x20; /* GROUP NUMBER の上の予約ビット */
        exec(c, 10);
        expect_cc(0x5, 0x24, 0x00);
        c[6] = 0x40;
        exec(c, 10);
        expect_cc(0x5, 0x24, 0x00);
        c[6] = 0x80;
        exec(c, 10);
        expect_cc(0x5, 0x24, 0x00);
        c[6] = 0;
        c[9] = 0x04; /* NACA */
        exec(c, 10);
        expect_cc(0x5, 0x24, 0x00);
    }
    {
        uint8_t c[12] = {0xA8, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
        c[10] = 0x80; /* ストリーミング */
        exec(c, 12);
        expect_cc(0x5, 0x24, 0x00);
        c[10] = 0x20;
        exec(c, 12);
        expect_cc(0x5, 0x24, 0x00);
        c[10] = 0x40;
        exec(c, 12);
        expect_cc(0x5, 0x24, 0x00);
        c[10] = 0x05;
        exec(c, 12);
        expect_good();
        mmc_scsi_command_done(&SCSI);
        c[10] = 0;
        c[11] = 0x04;
        exec(c, 12);
        expect_cc(0x5, 0x24, 0x00);
    }
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
}

static void test_read_acquire_failures_map_to_sense_and_leave_no_reference(void) {
    fx_mount_quiet(ID_A);
    MOCK.acquire_rc = ISO_ERR_NOT_FOUND;
    exec_read10(0, 1);
    expect_cc(0x2, 0x3A, 0x00);
    MOCK.acquire_rc = ISO_ERR_NO_CARD;
    exec_read10(0, 1);
    expect_cc(0x2, 0x3A, 0x00);
    MOCK.acquire_rc = ISO_ERR_EPOCH;
    exec_read10(0, 1);
    expect_cc(0x3, 0x11, 0x00);
    MOCK.acquire_rc = ISO_ERR_IO;
    exec_read12(0, 1);
    expect_cc(0x3, 0x11, 0x00);
    TEST_ASSERT_FALSE(mmc_scsi_holds_reference(&SCSI));
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
    TEST_ASSERT_EQUAL_UINT(0, MOCK.releases);
    MOCK.acquire_rc = 0;
    exec_read10(0, 1); /* 失敗の後も動く */
    expect_good();
    mmc_scsi_command_done(&SCSI);
}

/* ---------------------------------------------------------------- 参照のリーク */
static void test_command_done_is_idempotent_and_releases_exactly_once(void) {
    fx_mount_quiet(ID_A);
    exec_read10(0, 1);
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(MMC_REF_IDLE, atomic_load(&SCSI.ref_state)); /* 解放の後は IDLE（RELEASING のまま残らない） */
    mmc_scsi_command_done(&SCSI);
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(MMC_REF_IDLE, atomic_load(&SCSI.ref_state));
    TEST_ASSERT_EQUAL_UINT(1, MOCK.acquires);
    TEST_ASSERT_EQUAL_UINT(1, MOCK.releases);
    TEST_ASSERT_EQUAL_UINT(0, MOCK.bad_use);
    mmc_scsi_command_done(&SCSI); /* 参照なしでも安全 */
    TEST_ASSERT_EQUAL_UINT(1, MOCK.releases);
}

static void test_next_execute_releases_a_forgotten_reference(void) {
    fx_mount_quiet(ID_A);
    exec_read10(0, 1);
    TEST_ASSERT_EQUAL_INT(1, MOCK.live);
    EXEC(0x00, 0, 0, 0, 0, 0); /* command_done を呼び損ねた */
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
    TEST_ASSERT_EQUAL_UINT(1, MOCK.releases);
    exec_read10(0, 1);
    exec_read10(1, 1);
    TEST_ASSERT_EQUAL_INT(1, MOCK.live); /* 常に 1 つ以下 */
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
    TEST_ASSERT_EQUAL_UINT(0, MOCK.bad_use);
}

static void test_failed_execute_after_a_read_leaves_no_reference(void) {
    fx_mount_quiet(ID_A);
    exec_read10(0, 1);
    exec_read10(0, 500); /* 範囲外 */
    expect_cc(0x5, 0x21, 0x00);
    TEST_ASSERT_FALSE(mmc_scsi_holds_reference(&SCSI));
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
}

static void test_bus_reset_and_force_eject_do_not_release_the_reference(void) {
    fx_mount_quiet(ID_A);
    exec_read10(0, 4);
    iso_media_bus_reset(&MEDIA);
    TEST_ASSERT_TRUE(mmc_scsi_holds_reference(&SCSI));
    TEST_ASSERT_EQUAL_INT(1, MOCK.live);
    TEST_ASSERT_EQUAL_UINT(0, MOCK.releases);
    iso_media_force_eject(&MEDIA);
    TEST_ASSERT_EQUAL_INT(1, MOCK.live);
    TEST_ASSERT_EQUAL_UINT(0, MOCK.releases);
    mmc_scsi_command_done(&SCSI); /* 放すのはこれだけ */
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
    TEST_ASSERT_EQUAL_UINT(1, MOCK.releases);
}

/* ---------------------------------------------------------------- mmc_scsi_read_blocks */
static void expect_pattern(uint32_t id, uint64_t first_off, const uint8_t *buf, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        if (buf[i] != mock_pattern(id, first_off + i)) {
            char m[80];
            snprintf(m, sizeof(m), "pattern mismatch at +%llu", (unsigned long long)i);
            TEST_FAIL_MESSAGE(m);
        }
    }
}

static void expect_zero(const uint8_t *buf, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        if (buf[i] != 0) {
            char m[80];
            snprintf(m, sizeof(m), "not zero at +%llu (0x%02X)", (unsigned long long)i, buf[i]);
            TEST_FAIL_MESSAGE(m);
        }
    }
}

static void test_read_blocks_returns_the_backend_data(void) {
    static uint8_t buf[4 * 2048];
    fx_mount_quiet(ID_A);
    exec_read10(3, 4);
    memset(buf, 0xCC, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 3, 4, buf, sizeof(buf)));
    expect_pattern(ID_A, 3u * 2048u, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(3, MOCK.last_lba);
    TEST_ASSERT_EQUAL_UINT32(4, MOCK.last_blocks);
    mmc_scsi_command_done(&SCSI);
}

static void test_read_blocks_in_chunks_of_one_transfer(void) {
    static uint8_t buf[16 * 2048];
    fx_mount_quiet(ID_A);
    exec_read10(10, 60);
    for (uint32_t lba = 10; lba < 70; lba += 16) {
        uint32_t n = (70 - lba) < 16 ? (70 - lba) : 16;
        TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, lba, n, buf, sizeof(buf)));
        expect_pattern(ID_A, (uint64_t)lba * 2048u, buf, (uint64_t)n * 2048u);
    }
    mmc_scsi_command_done(&SCSI);
}

static void test_read_blocks_zero_fills_after_size_bytes(void) {
    static uint8_t buf[2 * 2048];
    fx_mount_quiet(ID_A); /* 最後のブロック 100 は先頭の 100 バイトだけが実データ */
    exec_read10(100, 1);
    memset(buf, 0xCC, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 100, 1, buf, 2048));
    expect_pattern(ID_A, 100u * 2048u, buf, 100);
    expect_zero(buf + 100, 2048 - 100); /* バックエンドは 0xEE を書いていた */
    TEST_ASSERT_EQUAL_HEX8(0xCC, buf[2048]); /* 範囲の外は触らない */
    mmc_scsi_command_done(&SCSI);

    exec_read10(99, 2); /* 実データのブロックと、最後のブロックにまたがる */
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 99, 2, buf, sizeof(buf)));
    expect_pattern(ID_A, 99u * 2048u, buf, 2048u + 100u);
    expect_zero(buf + 2048 + 100, 2048 - 100);
    mmc_scsi_command_done(&SCSI);
}

static void test_read_blocks_exact_multiple_has_no_zero_fill(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_B);
    exec_read10(49, 1);
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 49, 1, buf, sizeof(buf)));
    expect_pattern(ID_B, 49u * 2048u, buf, 2048); /* 最終ブロックの全体が実データ */
    mmc_scsi_command_done(&SCSI);
}

static void test_read_blocks_uses_64_bit_offsets_for_large_images(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_BIG);
    exec_read10(CAP_BIG - 1, 1);
    expect_good();
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, CAP_BIG - 1, 1, buf, sizeof(buf)));
    expect_pattern(ID_BIG, (uint64_t)(CAP_BIG - 1) * 2048u, buf, 2048);
    mmc_scsi_command_done(&SCSI);

    fx_mount_quiet(ID_HUGE); /* LBA 0xFFFFFFFF のブロック（オフセットが 32 ビットを超える） */
    exec_read10(0xFFFFFFFFu, 1);
    expect_good();
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 0xFFFFFFFFu, 1, buf, sizeof(buf)));
    expect_pattern(ID_HUGE, 0xFFFFFFFFull * 2048ull, buf, 2048);
    mmc_scsi_command_done(&SCSI);
}

static void test_read_blocks_zero_fill_of_a_partial_last_block_beyond_4_gib(void) {
    static uint8_t buf[2 * 2048];
    fx_mount_quiet(ID_BIGP); /* 最後のブロック（LBA 2621440）は先頭の 100 バイトだけが実データ */
    exec_read10(CAP_BIGP - 1u, 1);
    expect_good();
    memset(buf, 0xCC, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, CAP_BIGP - 1u, 1, buf, 2048));
    expect_pattern(ID_BIGP, (uint64_t)(CAP_BIGP - 1u) * 2048u, buf, 100);
    expect_zero(buf + 100, 2048 - 100);
    TEST_ASSERT_EQUAL_HEX8(0xCC, buf[2048]);
    mmc_scsi_command_done(&SCSI);
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0); /* READ CAPACITY: 最終 LBA = 2621440 = 0x00280000 */
    {
        static const uint8_t exp[8] = {0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00};
        EXPECT_DATA(exp);
    }
}

static void test_read_blocks_misuse_senses(void) {
    static uint8_t buf[4096];
    mmc_sense_t s;
    fx_mount_quiet(ID_A);
    exec_read10(0, 1);
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 100, 2, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(mmc_scsi_last_sense(&SCSI, &s));
    TEST_ASSERT_EQUAL_HEX8(0x5, s.key); /* 範囲外: ILLEGAL REQUEST / LBA OUT OF RANGE */
    TEST_ASSERT_EQUAL_HEX8(0x21, s.asc);
    TEST_ASSERT_EQUAL_HEX8(0x00, s.ascq);
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 2, buf, 2048));
    TEST_ASSERT_TRUE(mmc_scsi_last_sense(&SCSI, &s));
    TEST_ASSERT_EQUAL_HEX8(0x5, s.key); /* バッファが足りない: ILLEGAL REQUEST / INVALID FIELD */
    TEST_ASSERT_EQUAL_HEX8(0x24, s.asc);
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, NULL, 2048));
    TEST_ASSERT_TRUE(mmc_scsi_last_sense(&SCSI, &s));
    TEST_ASSERT_EQUAL_HEX8(0x5, s.key);
    TEST_ASSERT_EQUAL_HEX8(0x24, s.asc);
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf))); /* 参照なし */
    TEST_ASSERT_TRUE(mmc_scsi_last_sense(&SCSI, &s));
    TEST_ASSERT_EQUAL_HEX8(0x2, s.key);
    TEST_ASSERT_EQUAL_HEX8(0x3A, s.asc);
}

static void test_read_blocks_io_error_is_medium_error_and_clears_the_buffer(void) {
    static uint8_t buf[2 * 2048];
    static const uint8_t medium_err[18] = {0x70, 0x00, 0x03, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x11, 0x00, 0, 0, 0, 0};
    fx_mount_quiet(ID_A);
    exec_read10(0, 2);
    MOCK.read_rc = ISO_ERR_IO;
    memset(buf, 0xCC, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 2, buf, sizeof(buf)));
    expect_zero(buf, sizeof(buf)); /* 古い内容やバックエンドの書きかけを残さない */
    {
        mmc_sense_t s;
        TEST_ASSERT_TRUE(mmc_scsi_last_sense(&SCSI, &s));
        TEST_ASSERT_EQUAL_HEX8(0x3, s.key);
        TEST_ASSERT_EQUAL_HEX8(0x11, s.asc);
        TEST_ASSERT_EQUAL_HEX8(0x00, s.ascq);
    }
    EXEC(0x03, 0, 0, 0, 18, 0); /* REQUEST SENSE */
    EXPECT_DATA(medium_err);
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
}

static void test_read_blocks_epoch_mismatch_is_medium_error_like_any_io_error(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_A);
    exec_read10(0, 1);
    MOCK.read_rc = ISO_ERR_EPOCH;
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    {
        mmc_sense_t s;
        TEST_ASSERT_TRUE(mmc_scsi_last_sense(&SCSI, &s));
        TEST_ASSERT_EQUAL_HEX8(0x3, s.key);
        TEST_ASSERT_EQUAL_HEX8(0x11, s.asc);
        TEST_ASSERT_EQUAL_HEX8(0x00, s.ascq);
    }
    expect_zero(buf, sizeof(buf));
    mmc_scsi_command_done(&SCSI);
}

static void test_read_blocks_failure_in_the_middle_of_a_transfer_then_recovery(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_A);
    exec_read10(0, 3);
    MOCK.read_rc = ISO_ERR_IO;
    MOCK.read_fail_after = 1; /* 1 回目は成功、2 回目から失敗 */
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 1, 1, buf, sizeof(buf)));
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
    TEST_ASSERT_EQUAL_UINT(1, MOCK.releases);
    /* エラーの注入を解除して、新しい READ が成功し、期待するデータを返す（失敗が次のコマンドに残らない） */
    MOCK.read_rc = 0;
    exec_read10(5, 1);
    expect_good();
    memset(buf, 0xCC, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 5, 1, buf, sizeof(buf)));
    expect_pattern(ID_A, 5u * 2048u, buf, 2048);
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
    TEST_ASSERT_EQUAL_UINT(2, MOCK.releases);
}

/* ---------------------------------------------------------------- read_blocks の実行中の command_done（参照の解放の順序） */
static unsigned HOOK_CALLS;
static mmc_result_t HOOK_RES;
static int HOOK_STATE_SEEN;
static int HOOK_RELEASES_SEEN;
static bool HOOK_HOLDS_SEEN;

static void hook_command_done_twice(void) {
    HOOK_CALLS++;
    mmc_scsi_command_done(&SCSI); /* BOT 層が、完了を待たずにリセット / 切断で呼んだ場合の模擬 */
    HOOK_STATE_SEEN = atomic_load(&SCSI.ref_state);
    HOOK_RELEASES_SEEN = (int)MOCK.releases;
    HOOK_HOLDS_SEEN = mmc_scsi_holds_reference(&SCSI);
    mmc_scsi_command_done(&SCSI); /* 冪等 */
}

static void hook_bus_reset_and_command_done(void) {
    HOOK_CALLS++;
    iso_media_bus_reset(&MEDIA);
    mmc_scsi_command_done(&SCSI);
}

static void hook_force_eject(void) {
    HOOK_CALLS++;
    iso_media_force_eject(&MEDIA);
}

static void hook_command_done_then_execute(void) {
    static const uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    static uint8_t out[8];
    HOOK_CALLS++;
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(0, mmc_scsi_execute(&SCSI, cdb, 10, MMC_DIR_IN, 2048, out, sizeof(out), &HOOK_RES));
}

static void test_command_done_during_read_blocks_defers_the_release_until_it_returns(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_A);
    exec_read10(0, 2);
    HOOK_CALLS = 0;
    MOCK.on_read = hook_command_done_twice;
    memset(buf, 0xCC, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT(1, HOOK_CALLS);
    /* 読み出しの最中: 解放は予約されただけで、実行されていない（backend.read の最中に backend.release を呼ばない） */
    TEST_ASSERT_EQUAL_INT(MMC_REF_READING_RELEASE_PENDING, HOOK_STATE_SEEN);
    TEST_ASSERT_EQUAL_INT(0, HOOK_RELEASES_SEEN);
    TEST_ASSERT_TRUE(HOOK_HOLDS_SEEN);
    TEST_ASSERT_EQUAL_UINT(0, MOCK.release_in_read);
    /* 読み出しは、取得済みの表で完了している */
    expect_pattern(ID_A, 0, buf, 2048);
    /* 復帰の後: ちょうど 1 回だけ解放された */
    TEST_ASSERT_EQUAL_UINT(1, MOCK.releases);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
    TEST_ASSERT_FALSE(mmc_scsi_holds_reference(&SCSI));
    TEST_ASSERT_EQUAL_INT(MMC_REF_IDLE, atomic_load(&SCSI.ref_state));
    TEST_ASSERT_EQUAL_UINT(0, MOCK.bad_use);
    mmc_scsi_command_done(&SCSI); /* 後からの command_done も冪等 */
    TEST_ASSERT_EQUAL_UINT(1, MOCK.releases);
    /* 次のコマンドは、通常どおり動く */
    MOCK.on_read = NULL;
    exec_read10(1, 1);
    expect_good();
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 1, 1, buf, sizeof(buf)));
    expect_pattern(ID_A, 2048, buf, 2048);
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_UINT(2, MOCK.releases);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
}

static void test_reset_and_command_done_during_read_blocks_abort_it_and_release_once_after(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_A);
    exec_read10(0, 2);
    MOCK.on_read = hook_bus_reset_and_command_done;
    memset(buf, 0xCC, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    expect_zero(buf, sizeof(buf)); /* 読めたデータを返さない */
    {
        mmc_sense_t s;
        TEST_ASSERT_TRUE(mmc_scsi_last_sense(&SCSI, &s));
        TEST_ASSERT_EQUAL_HEX8(0xB, s.key);
    }
    TEST_ASSERT_EQUAL_UINT(0, MOCK.release_in_read);
    TEST_ASSERT_EQUAL_UINT(1, MOCK.releases);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
    TEST_ASSERT_EQUAL_INT(MMC_REF_IDLE, atomic_load(&SCSI.ref_state));
}

static void test_execute_during_read_blocks_does_not_replace_the_reference(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_A);
    exec_read10(0, 2);
    MOCK.on_read = hook_command_done_then_execute; /* 契約違反: 実行中に次のコマンドが来る */
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, HOOK_RES.status);
    TEST_ASSERT_EQUAL_HEX8(0xB, HOOK_RES.sense.key); /* 新しい READ は、参照を取らず、ABORTED COMMAND */
    TEST_ASSERT_FALSE(HOOK_RES.read.is_read);
    TEST_ASSERT_EQUAL_UINT(1, MOCK.acquires);
    TEST_ASSERT_EQUAL_UINT(0, MOCK.release_in_read);
    TEST_ASSERT_EQUAL_UINT(1, MOCK.releases); /* 元の参照は、復帰の直前に、1 回だけ解放 */
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
    TEST_ASSERT_EQUAL_UINT(0, MOCK.bad_use);
}

static void test_read_blocks_without_the_right_to_use_the_reference_is_not_ready(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_A);
    exec_read10(0, 1);
    atomic_store(&SCSI.ref_state, MMC_REF_RELEASING); /* 他のタスクが解放中の模擬 */
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    {
        mmc_sense_t s;
        TEST_ASSERT_TRUE(mmc_scsi_last_sense(&SCSI, &s));
        TEST_ASSERT_EQUAL_HEX8(0x2, s.key); /* NOT READY / メディアなし（参照を使えない） */
        TEST_ASSERT_EQUAL_HEX8(0x3A, s.asc);
    }
    TEST_ASSERT_EQUAL_UINT(0, MOCK.reads);
    atomic_store(&SCSI.ref_state, MMC_REF_READING_RELEASE_PENDING);
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT(0, MOCK.reads);
    mmc_scsi_command_done(&SCSI); /* RELEASING / PENDING の間は、何もしない */
    TEST_ASSERT_EQUAL_UINT(0, MOCK.releases);
    atomic_store(&SCSI.ref_state, MMC_REF_IDLE);
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_UINT(1, MOCK.releases);
}

static void test_abort_is_decided_before_the_backend_error_is_classified(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_A);
    exec_read10(0, 3);
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf))); /* 先行のチャンクは成功 */
    MOCK.on_read = hook_force_eject; /* 次の read の最中に force_eject。バックエンドは ISO_ERR_EPOCH を返す */
    MOCK.read_rc = ISO_ERR_EPOCH;
    MOCK.read_fail_after = MOCK.reads;
    memset(buf, 0xCC, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 1, 1, buf, sizeof(buf)));
    {
        mmc_sense_t s;
        TEST_ASSERT_TRUE(mmc_scsi_last_sense(&SCSI, &s));
        TEST_ASSERT_EQUAL_HEX8(0xB, s.key); /* MEDIUM ERROR ではなく ABORTED COMMAND */
        TEST_ASSERT_EQUAL_HEX8(0x00, s.asc);
    }
    expect_zero(buf, sizeof(buf));
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
    /* 世代が変わっていない ISO_ERR_EPOCH は、MEDIUM ERROR（test_read_blocks_epoch_mismatch_is_medium_error...） */
}

static void test_aborted_sense_survives_until_request_sense(void) {
    static uint8_t buf[2048];
    static const uint8_t aborted[18] = {0x70, 0x00, 0x0B, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x00, 0x00, 0, 0, 0, 0};
    static const uint8_t no_sense[18] = {0x70, 0x00, 0x00, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x00, 0x00, 0, 0, 0, 0};
    fx_mount_quiet(ID_A);
    exec_read10(0, 2);
    iso_media_force_eject(&MEDIA); /* READ の開始後に取り外し（世代が進む） */
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    mmc_scsi_command_done(&SCSI);
    EXEC(0x03, 0, 0, 0, 18, 0); /* 世代が進んだ後に記録したセンスなので、捨てない */
    EXPECT_DATA(aborted);
    EXEC(0x03, 0, 0, 0, 18, 0);
    EXPECT_DATA(no_sense);
}

static void test_bus_reset_after_the_sense_was_recorded_still_discards_it(void) {
    static uint8_t buf[2048];
    static const uint8_t no_sense[18] = {0x70, 0x00, 0x00, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x00, 0x00, 0, 0, 0, 0};
    fx_mount_quiet(ID_A);
    exec_read10(0, 2);
    iso_media_force_eject(&MEDIA);
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    mmc_scsi_command_done(&SCSI);
    iso_media_bus_reset(&MEDIA); /* さらに世代が進んだ */
    EXEC(0x03, 0, 0, 0, 18, 0);
    EXPECT_DATA(no_sense);
}

static void test_read_blocks_misuse_is_rejected_without_calling_the_backend(void) {
    static uint8_t buf[2048];
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf))); /* 参照なし */
    {
        mmc_sense_t s;
        TEST_ASSERT_TRUE(mmc_scsi_last_sense(&SCSI, &s));
        TEST_ASSERT_EQUAL_HEX8(0x2, s.key);
        TEST_ASSERT_EQUAL_HEX8(0x3A, s.asc);
    }
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(NULL, 0, 1, buf, sizeof(buf)));
    fx_mount_quiet(ID_A);
    exec_read10(0, 1);
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 100, 2, buf, 2 * 2048)); /* 範囲外 */
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 2, buf, 2048 + 2047)); /* cap が足りない */
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, NULL, 2048));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0xFFFFFFFFu, 2, buf, 4096));
    TEST_ASSERT_EQUAL_UINT(0, MOCK.reads);
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 0, 0, NULL, 0)); /* 0 ブロックは何もしない */
    TEST_ASSERT_EQUAL_UINT(0, MOCK.reads);
    mmc_scsi_command_done(&SCSI);
}

static void test_read_blocks_length_in_bytes_does_not_wrap_at_32_bits(void) {
    static uint8_t buf[4096];
    fx_mount_quiet(ID_HUGE);
    exec_read10(0, 1);
    expect_good();
    /* 0x200000 ブロック = 2^32 バイト。32 ビットで計算すると 0 になり、cap の検査をすり抜ける */
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 0x200000u, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 0x200001u, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT(0, MOCK.reads);
    mmc_scsi_command_done(&SCSI);
}

static void test_web_eject_during_a_read_lets_the_command_finish_on_its_snapshot(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_A);
    exec_read10(0, 2);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&MEDIA, false));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    expect_pattern(ID_A, 0, buf, 2048);
    mmc_scsi_command_done(&SCSI);
    EXEC(0x00, 0, 0, 0, 0, 0); /* 次のコマンドからは、メディアなし */
    expect_cc(0x2, 0x3A, 0x00);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
}

static void test_bus_reset_aborts_the_running_read_but_keeps_the_reference_until_done(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_A);
    exec_read10(0, 2);
    iso_media_bus_reset(&MEDIA);
    memset(buf, 0xCC, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    {
        mmc_sense_t s;
        TEST_ASSERT_TRUE(mmc_scsi_last_sense(&SCSI, &s));
        TEST_ASSERT_EQUAL_HEX8(0xB, s.key); /* ABORTED COMMAND */
    }
    expect_zero(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(0, MOCK.reads);
    TEST_ASSERT_EQUAL_INT(1, MOCK.live);
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
}

static void test_force_eject_aborts_the_running_read(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_A);
    exec_read10(0, 2);
    iso_media_force_eject(&MEDIA);
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(1, MOCK.live);
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
}

static void test_reset_during_the_backend_read_is_detected_after_it_returns(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_A);
    exec_read10(0, 1);
    MOCK.bump_epoch_on_read = 1; /* バックエンドの read の途中でバスリセット */
    memset(buf, 0xCC, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_CHECK_CONDITION, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT(1, MOCK.reads);
    expect_zero(buf, sizeof(buf)); /* 読めたデータを返さない */
    mmc_scsi_command_done(&SCSI);
    TEST_ASSERT_EQUAL_INT(0, MOCK.live);
}

static void test_a_new_command_after_bus_reset_reads_normally(void) {
    static uint8_t buf[2048];
    fx_mount_quiet(ID_A);
    iso_media_bus_reset(&MEDIA);
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x6, 0x29, 0x00);
    exec_read10(0, 1);
    expect_good();
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, mmc_scsi_read_blocks(&SCSI, 0, 1, buf, sizeof(buf)));
    expect_pattern(ID_A, 0, buf, 2048);
    mmc_scsi_command_done(&SCSI);
}

/* ---------------------------------------------------------------- READ TOC/PMA/ATIP */
static void test_read_toc_format0_lba_and_msf_exact(void) {
    static const uint8_t lba[20] = {0x00, 0x12, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x14, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x65};
    static const uint8_t msf[20] = {0x00, 0x12, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00,
                                    0x00, 0x14, 0xAA, 0x00, 0x00, 0x00, 0x03, 0x1A}; /* (101 + 150) = 251 = 3 s + 26 frames */
    fx_mount_quiet(ID_A);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x00, 0x00, 0xFF, 0x00);
    EXPECT_DATA(lba);
    EXEC(0x43, 0x02, 0x00, 0, 0, 0, 0x01, 0x00, 0xFF, 0x00);
    EXPECT_DATA(msf);
}

static void test_read_toc_format0_starting_track(void) {
    static const uint8_t leadout[12] = {0x00, 0x0A, 0x01, 0x01, 0x00, 0x14, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x65};
    static const uint8_t all[20] = {0x00, 0x12, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x14, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x65};
    fx_mount_quiet(ID_A);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0xAA, 0x00, 0xFF, 0x00);
    EXPECT_DATA(leadout);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x01, 0x00, 0xFF, 0x00);
    EXPECT_DATA(all);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x02, 0x00, 0xFF, 0x00); /* 最後のトラックより大きく、AAh でもない */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0xA9, 0x00, 0xFF, 0x00);
    expect_cc(0x5, 0x24, 0x00);
}

static void test_read_toc_format1_multi_session_info(void) {
    static const uint8_t lba[12] = {0x00, 0x0A, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t msf[12] = {0x00, 0x0A, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00};
    fx_mount_quiet(ID_A);
    EXEC(0x43, 0x00, 0x01, 0, 0, 0, 0x00, 0x00, 0xFF, 0x00);
    EXPECT_DATA(lba);
    EXEC(0x43, 0x02, 0x01, 0, 0, 0, 0x00, 0x00, 0xFF, 0x00);
    EXPECT_DATA(msf);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x00, 0x00, 0xFF, 0x40); /* SFF-8020i 形式: 形式は byte 9 のビット 7-6（Linux の sr が使う） */
    EXPECT_DATA(lba);
    EXEC(0x43, 0x00, 0x01, 0, 0, 0, 0x00, 0x00, 0xFF, 0x80); /* byte 2 が 0 でなければ、byte 2 が優先 */
    EXPECT_DATA(lba);
}

static void test_read_toc_format2_full_toc_is_always_msf(void) {
    static const uint8_t full[48] = {
        0x00, 0x2E, 0x01, 0x01,                                                         /* 長さ 46、最初 / 最後のセッション */
        0x01, 0x14, 0x00, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,               /* A0: 最初のトラック = 1、種別 0 */
        0x01, 0x14, 0x00, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,               /* A1: 最後のトラック = 1 */
        0x01, 0x14, 0x00, 0xA2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x1A,               /* A2: リードアウト 00:03:26 */
        0x01, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00};              /* トラック 1: 00:02:00 */
    fx_mount_quiet(ID_A);
    EXEC(0x43, 0x00, 0x02, 0, 0, 0, 0x01, 0x00, 0xFF, 0x00); /* MSF ビットなしでも MSF */
    EXPECT_DATA(full);
    EXEC(0x43, 0x02, 0x02, 0, 0, 0, 0x00, 0x00, 0xFF, 0x00);
    EXPECT_DATA(full);
    EXEC(0x43, 0x02, 0x02, 0, 0, 0, 0x02, 0x00, 0xFF, 0x00); /* セッション 2 はない */
    expect_cc(0x5, 0x24, 0x00);
}

static void test_read_toc_msf_minutes_seconds_and_frames(void) {
    /* capacity_blocks = 5000: 5000 + 150 = 5150 フレーム = 1 分 8 秒 50 フレーム（01:08:50 = 01 08 32） */
    static const uint8_t msf[20] = {0x00, 0x12, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00,
                                    0x00, 0x14, 0xAA, 0x00, 0x00, 0x01, 0x08, 0x32};
    static const uint8_t lba[8] = {0x00, 0x14, 0xAA, 0x00, 0x00, 0x00, 0x13, 0x88};
    fx_mount_quiet(ID_C);
    EXEC(0x43, 0x02, 0x00, 0, 0, 0, 0x01, 0x00, 0xFF, 0x00);
    EXPECT_DATA(msf);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0xAA, 0x00, 0x0C, 0x00);
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(12, RES.data_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(lba, OUT + 4, 8);
}

static void test_read_toc_unsupported_formats_and_bad_fields(void) {
    fx_mount_quiet(ID_A);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x00, 0x00, 0xFF, 0x10); /* byte 9 の下位の予約ビット */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x00, 0x00, 0xFF, 0x20);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x00, 0x03, 0, 0, 0, 0x00, 0x00, 0xFF, 0x00); /* PMA */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x00, 0x04, 0, 0, 0, 0x00, 0x00, 0xFF, 0x00); /* ATIP */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x00, 0x05, 0, 0, 0, 0x00, 0x00, 0xFF, 0x00); /* CD-TEXT */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x00, 0x0F, 0, 0, 0, 0x00, 0x00, 0xFF, 0x00);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x00, 0x00, 0xFF, 0xC0); /* byte 9 の形式 3 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x01, 0x00, 0, 0, 0, 0x00, 0x00, 0xFF, 0x00); /* MSF ビット以外の予約 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x00, 0x10, 0, 0, 0, 0x00, 0x00, 0xFF, 0x00);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x00, 0x00, 0x01, 0, 0, 0x00, 0x00, 0xFF, 0x00);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x00, 0x00, 0, 0x01, 0, 0x00, 0x00, 0xFF, 0x00);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0x01, 0x00, 0x00, 0xFF, 0x00);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x00, 0x00, 0xFF, 0x04);
    expect_cc(0x5, 0x24, 0x00);
}

static void test_read_toc_without_media_and_with_allocation_length(void) {
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x01, 0x00, 0xFF, 0x00);
    expect_cc(0x2, 0x3A, 0x00);
    fx_mount_quiet(ID_A);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x01, 0x00, 0x04, 0x00); /* 見出しの 4 バイトだけ。長さの欄は全体の長さ */
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(4, RES.data_len);
    {
        static const uint8_t head[4] = {0x00, 0x12, 0x01, 0x01};
        TEST_ASSERT_EQUAL_HEX8_ARRAY(head, OUT, 4);
    }
    TEST_ASSERT_EQUAL_HEX8(0xA5, OUT[4]);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x01, 0x00, 0x00, 0x00);
    expect_no_data_good();
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x01, 0x00, 0x0C, 0x00); /* 途中で切れる */
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(12, RES.data_len);
}

static void test_read_toc_large_image_saturates_msf_at_255_59_74_but_not_lba(void) {
    static const uint8_t lba[20] = {0x00, 0x12, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x14, 0xAA, 0x00, 0x00, 0x14, 0x00, 0x00}; /* 1,310,720 = 0x00140000 */
    static const uint8_t msf[20] = {0x00, 0x12, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00,
                                    0x00, 0x14, 0xAA, 0x00, 0x00, 0xFF, 0x3B, 0x4A};
    fx_mount_quiet(ID_BIG);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x01, 0x00, 0xFF, 0x00);
    EXPECT_DATA(lba);
    EXEC(0x43, 0x02, 0x00, 0, 0, 0, 0x01, 0x00, 0xFF, 0x00);
    EXPECT_DATA(msf);
}

/* ---------------------------------------------------------------- 全コマンドの共通: 予約バイトとコントロールバイト */
typedef struct {
    uint8_t op;
    size_t len;
    size_t control; /* CONTROL バイトの位置 */
    uint8_t base[16];
    const uint8_t *reserved; /* 予約のバイトの位置（0 で終わる） */
} cdb_case_t;

/* 有効な CDB（メディア A が入っている前提）。reserved は、そのバイトを 1 にすると INVALID FIELD になるバイトの位置（0 終端） */
static const uint8_t RES_TUR[] = {1, 2, 3, 4, 0};
static const uint8_t RES_RS[] = {1, 2, 3, 0};
static const uint8_t RES_MS10[] = {4, 5, 6, 0};
static const uint8_t RES_SSU[] = {2, 3, 0};
static const uint8_t RES_PREV[] = {1, 2, 3, 0};
static const uint8_t RES_RCAP[] = {1, 6, 7, 0};
static const uint8_t RES_RFC[] = {1, 2, 3, 4, 5, 6, 0};
static const uint8_t RES_TOC[] = {3, 4, 5, 0};
static const uint8_t RES_CFG[] = {4, 5, 6, 0};
static const uint8_t RES_GESN[] = {2, 3, 5, 6, 0};
static const uint8_t RES_DISC[] = {2, 3, 4, 5, 6, 0};
static const uint8_t RES_TRK[] = {6, 0};
static const uint8_t RES_NONE[] = {0};

static const cdb_case_t CASES[] = {
    {0x00, 6, 5, {0x00}, RES_TUR},
    {0x03, 6, 5, {0x03, 0, 0, 0, 18}, RES_RS},
    {0x12, 6, 5, {0x12, 0, 0, 0, 36}, RES_NONE},
    {0x1A, 6, 5, {0x1A, 0, 0x2A, 0, 0xFF}, RES_NONE},
    {0x1B, 6, 5, {0x1B, 0, 0, 0, 0x01}, RES_SSU},
    {0x1E, 6, 5, {0x1E, 0, 0, 0, 0x00}, RES_PREV},
    {0x23, 10, 9, {0x23, 0, 0, 0, 0, 0, 0, 0, 0xFC}, RES_RFC},
    {0x25, 10, 9, {0x25}, RES_RCAP},
    {0x28, 10, 9, {0x28, 0, 0, 0, 0, 0, 0, 0, 1}, RES_NONE},
    {0x43, 10, 9, {0x43, 0, 0, 0, 0, 0, 0x01, 0, 0xFF}, RES_TOC},
    {0x46, 10, 9, {0x46, 0, 0, 0, 0, 0, 0, 0, 0xFF}, RES_CFG},
    {0x4A, 10, 9, {0x4A, 0x01, 0, 0, 0x10, 0, 0, 0, 0x08}, RES_GESN},
    {0x51, 10, 9, {0x51, 0, 0, 0, 0, 0, 0, 0, 0xFF}, RES_DISC},
    {0x52, 10, 9, {0x52, 0x01, 0, 0, 0, 0x01, 0, 0, 0xFF}, RES_TRK},
    {0x5A, 10, 9, {0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 0xFF}, RES_MS10},
    {0xA8, 12, 11, {0xA8, 0, 0, 0, 0, 0, 0, 0, 0, 1}, RES_NONE},
};

static void run_case(const cdb_case_t *k, const uint8_t *cdb) {
    exec(cdb, k->len);
    mmc_scsi_command_done(&SCSI);
}

static void test_every_command_rejects_each_reserved_control_bit_and_accepts_vendor_bits(void) {
    static const uint8_t bad_bits[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20};
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        const cdb_case_t *k = &CASES[i];
        uint8_t cdb[16];
        char msg[64];
        snprintf(msg, sizeof(msg), "opcode %02X", k->op);
        fx_init();
        fx_mount_quiet(ID_A);
        memcpy(cdb, k->base, sizeof(cdb));
        run_case(k, cdb);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MMC_STATUS_GOOD, RES.status, msg); /* 基準の CDB が有効 */
        for (size_t b = 0; b < sizeof(bad_bits); b++) {
            memcpy(cdb, k->base, sizeof(cdb));
            cdb[k->control] = bad_bits[b];
            run_case(k, cdb);
            TEST_ASSERT_EQUAL_INT_MESSAGE(MMC_STATUS_CHECK_CONDITION, RES.status, msg);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x24, RES.sense.asc, msg);
        }
        if (k->op != 0x43) { /* READ TOC の byte 9 のビット 7-6 は、形式（SFF-8020i）。下の別のテストで見る */
            static const uint8_t vendor[] = {0x40, 0x80, 0xC0};
            for (size_t b = 0; b < sizeof(vendor); b++) {
                memcpy(cdb, k->base, sizeof(cdb));
                cdb[k->control] = vendor[b];
                run_case(k, cdb);
                TEST_ASSERT_EQUAL_INT_MESSAGE(MMC_STATUS_GOOD, RES.status, msg);
            }
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, MOCK.live, msg);
    }
}

static void test_every_command_rejects_each_reserved_byte(void) {
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        const cdb_case_t *k = &CASES[i];
        for (const uint8_t *r = k->reserved; *r != 0; r++) {
            uint8_t cdb[16];
            char msg[64];
            snprintf(msg, sizeof(msg), "opcode %02X byte %u", k->op, *r);
            fx_init();
            fx_mount_quiet(ID_A);
            memcpy(cdb, k->base, sizeof(cdb));
            cdb[*r] = 0x01;
            run_case(k, cdb);
            TEST_ASSERT_EQUAL_INT_MESSAGE(MMC_STATUS_CHECK_CONDITION, RES.status, msg);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x24, RES.sense.asc, msg);
            memcpy(cdb, k->base, sizeof(cdb));
            cdb[*r] = 0x80;
            run_case(k, cdb);
            TEST_ASSERT_EQUAL_INT_MESSAGE(MMC_STATUS_CHECK_CONDITION, RES.status, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, MOCK.live, msg);
        }
    }
}

static void test_two_byte_allocation_length_is_honoured_by_every_command_that_has_one(void) {
    /* 割り当て長（ビッグエンディアン 16 ビット）の上位バイトの位置。0x0100 = 256 で、全ての応答が収まる */
    static const struct { uint8_t op; size_t hi; } K[] = {{0x12, 3}, {0x23, 7}, {0x43, 7}, {0x46, 7}, {0x4A, 7}, {0x51, 7}, {0x52, 7}, {0x5A, 7}};
    for (size_t i = 0; i < sizeof(K) / sizeof(K[0]); i++) {
        for (size_t c = 0; c < sizeof(CASES) / sizeof(CASES[0]); c++) {
            uint8_t cdb[16];
            uint32_t full;
            char msg[48];
            if (CASES[c].op != K[i].op) continue;
            snprintf(msg, sizeof(msg), "opcode %02X", K[i].op);
            fx_init();
            fx_mount_quiet(ID_A);
            memcpy(cdb, CASES[c].base, sizeof(cdb));
            cdb[K[i].hi] = 0x00;
            cdb[K[i].hi + 1] = 0xFF; /* 255 */
            exec(cdb, CASES[c].len);
            full = RES.data_len;
            TEST_ASSERT_TRUE_MESSAGE(full > 0 && RES.status == MMC_STATUS_GOOD, msg);
            fx_init();
            fx_mount_quiet(ID_A);
            cdb[K[i].hi] = 0x01; /* 256: 上位バイトだけが 1 */
            cdb[K[i].hi + 1] = 0x00;
            exec(cdb, CASES[c].len);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(full, RES.data_len, msg);
        }
    }
}

static void test_read_toc_two_byte_allocation_length_and_huge_lba_form(void) {
    static const uint8_t huge_lba[20] = {0x00, 0x12, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x14, 0xAA, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    fx_mount_quiet(ID_A);
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x01, 0x01, 0x00, 0x00); /* 割り当て長 256 */
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(20, RES.data_len);
    fx_mount_quiet(ID_HUGE); /* リードアウトが 32 ビットを超える: LBA 形式は 0xFFFFFFFF */
    EXEC(0x43, 0x00, 0x00, 0, 0, 0, 0x01, 0x00, 0xFF, 0x00);
    EXPECT_DATA(huge_lba);
    for (uint8_t bit = 0x10; bit != 0; bit = (uint8_t)(bit << 1)) { /* byte 2 の上位ニブルは予約 */
        EXEC(0x43, 0x00, bit, 0, 0, 0, 0x01, 0x00, 0xFF, 0x00);
        expect_cc(0x5, 0x24, 0x00);
    }
}

/* ---------------------------------------------------------------- GET CONFIGURATION */
#define CFG_PROFILE_ON 0x00, 0x00, 0x03, 0x04, 0x00, 0x10, 0x01, 0x00
#define CFG_PROFILE_OFF 0x00, 0x00, 0x03, 0x04, 0x00, 0x10, 0x00, 0x00
#define CFG_CORE 0x00, 0x01, 0x0B, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00
#define CFG_REMOVABLE 0x00, 0x03, 0x0B, 0x04, 0x29, 0x00, 0x00, 0x00
#define CFG_RANDOM_ON 0x00, 0x10, 0x01, 0x08, 0x00, 0x00, 0x08, 0x00, 0x00, 0x10, 0x00, 0x00
#define CFG_RANDOM_OFF 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x00, 0x00, 0x10, 0x00, 0x00

static void test_get_configuration_all_features_with_media(void) {
    static const uint8_t exp[48] = {0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x10, /* 現在のプロファイル = DVD-ROM */
                                    CFG_PROFILE_ON, CFG_CORE, CFG_REMOVABLE, CFG_RANDOM_ON};
    fx_mount_quiet(ID_A);
    EXEC(0x46, 0x00, 0x00, 0x00, 0, 0, 0, 0xFF, 0xFF, 0);
    EXPECT_DATA(exp);
    EXEC(0x46, 0x01, 0x00, 0x00, 0, 0, 0, 0xFF, 0xFF, 0); /* RT = 1: 現在のものすべて（メディアありは同じ） */
    EXPECT_DATA(exp);
}

static void test_get_configuration_without_media(void) {
    static const uint8_t all[48] = {0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, /* 現在のプロファイル = なし */
                                    CFG_PROFILE_OFF, CFG_CORE, CFG_REMOVABLE, CFG_RANDOM_OFF};
    static const uint8_t current[36] = {0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, CFG_PROFILE_OFF, CFG_CORE, CFG_REMOVABLE};
    EXEC(0x46, 0x00, 0x00, 0x00, 0, 0, 0, 0xFF, 0xFF, 0);
    EXPECT_DATA(all);
    EXEC(0x46, 0x01, 0x00, 0x00, 0, 0, 0, 0xFF, 0xFF, 0); /* メディアがなければ Random Readable は current でない */
    EXPECT_DATA(current);
}

static void test_get_configuration_starting_feature_and_one_feature(void) {
    static const uint8_t from3[28] = {0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x10, CFG_REMOVABLE, CFG_RANDOM_ON};
    static const uint8_t only_core[20] = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, CFG_CORE};
    static const uint8_t none[8] = {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x10};
    static const uint8_t only_random_no_media[20] = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, CFG_RANDOM_OFF};
    fx_mount_quiet(ID_A);
    EXEC(0x46, 0x00, 0x00, 0x02, 0, 0, 0, 0xFF, 0xFF, 0); /* 0002h 以上 */
    EXPECT_DATA(from3);
    EXEC(0x46, 0x02, 0x00, 0x01, 0, 0, 0, 0xFF, 0xFF, 0); /* RT = 2: Core だけ */
    EXPECT_DATA(only_core);
    EXEC(0x46, 0x02, 0x00, 0x02, 0, 0, 0, 0xFF, 0xFF, 0); /* 未対応のフィーチャー: 見出しだけ */
    EXPECT_DATA(none);
    EXEC(0x46, 0x00, 0x00, 0x11, 0, 0, 0, 0xFF, 0xFF, 0); /* 範囲の上: 見出しだけ */
    EXPECT_DATA(none);
    EXEC(0x46, 0x03, 0x00, 0x00, 0, 0, 0, 0xFF, 0xFF, 0); /* RT = 3: 予約 */
    expect_cc(0x5, 0x24, 0x00);
    fx_init();
    EXEC(0x46, 0x02, 0x00, 0x10, 0, 0, 0, 0xFF, 0xFF, 0);
    EXPECT_DATA(only_random_no_media);
}

static void test_get_configuration_allocation_length_and_reserved(void) {
    fx_mount_quiet(ID_A);
    EXEC(0x46, 0x00, 0x00, 0x00, 0, 0, 0, 0x00, 0x08, 0); /* 見出しの 8 バイトだけ。長さの欄は全体 */
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(8, RES.data_len);
    TEST_ASSERT_EQUAL_HEX8(0x2C, OUT[3]);
    EXEC(0x46, 0x00, 0x00, 0x00, 0, 0, 0, 0x00, 0x00, 0);
    expect_no_data_good();
    EXEC(0x46, 0x04, 0x00, 0x00, 0, 0, 0, 0xFF, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x46, 0x00, 0x00, 0x00, 0x01, 0, 0, 0xFF, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x46, 0x00, 0x00, 0x00, 0, 0x01, 0, 0xFF, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x46, 0x00, 0x00, 0x00, 0, 0, 0x01, 0xFF, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
}

/* ---------------------------------------------------------------- GET EVENT STATUS NOTIFICATION */
#define GESN_MEDIA 0x4A, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x00

static void test_gesn_reports_new_media_once_then_no_change(void) {
    static const uint8_t new_media[8] = {0x00, 0x06, 0x04, 0x10, 0x02, 0x02, 0x00, 0x00};
    static const uint8_t no_change[8] = {0x00, 0x06, 0x04, 0x10, 0x00, 0x02, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, ID_A, false));
    EXEC(0x00, 0, 0, 0, 0, 0); /* UNIT ATTENTION を消費 */
    EXEC(GESN_MEDIA);
    EXPECT_DATA(new_media);
    EXEC(GESN_MEDIA);
    EXPECT_DATA(no_change);
}

static void test_gesn_two_byte_allocation_length(void) {
    static const uint8_t new_media[8] = {0x00, 0x06, 0x04, 0x10, 0x02, 0x02, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, ID_A, false));
    EXEC(0x00, 0, 0, 0, 0, 0);
    EXEC(0x4A, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00); /* 割り当て長 256 */
    EXPECT_DATA(new_media);
}

static void test_gesn_reports_removal_for_web_eject_target_eject_and_force_eject(void) {
    static const uint8_t removal[8] = {0x00, 0x06, 0x04, 0x10, 0x03, 0x00, 0x00, 0x00};
    static const uint8_t no_change_empty[8] = {0x00, 0x06, 0x04, 0x10, 0x00, 0x00, 0x00, 0x00};
    fx_mount_quiet(ID_A);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&MEDIA, false));
    EXEC(GESN_MEDIA);
    EXPECT_DATA(removal);
    EXEC(GESN_MEDIA);
    EXPECT_DATA(no_change_empty);

    fx_mount_quiet(ID_A);
    EXEC(0x1B, 0, 0, 0, 0x02, 0); /* ターゲットの取り外し */
    EXEC(GESN_MEDIA);
    EXPECT_DATA(removal);

    fx_mount_quiet(ID_A);
    iso_media_force_eject(&MEDIA);
    EXEC(0x00, 0, 0, 0, 0, 0); /* UNIT ATTENTION 28h/00h */
    expect_cc(0x6, 0x28, 0x00);
    EXEC(GESN_MEDIA);
    EXPECT_DATA(removal);
}

static void test_gesn_eject_request_event(void) {
    static const uint8_t req[8] = {0x00, 0x06, 0x04, 0x10, 0x01, 0x02, 0x00, 0x00};
    fx_mount_quiet(ID_A);
    TEST_ASSERT_TRUE(iso_media_request_eject(&MEDIA));
    EXEC(GESN_MEDIA);
    EXPECT_DATA(req);
}

static void test_gesn_without_the_media_class_is_nea_and_keeps_the_event(void) {
    static const uint8_t nea[4] = {0x00, 0x02, 0x80, 0x10};
    static const uint8_t new_media[8] = {0x00, 0x06, 0x04, 0x10, 0x02, 0x02, 0x00, 0x00};
    fx_mount_quiet(ID_A);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, ID_B, false));
    EXEC(0x00, 0, 0, 0, 0, 0);
    EXEC(0x4A, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00); /* クラス 0 */
    EXPECT_DATA(nea);
    EXEC(0x4A, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00); /* 動作の変化だけ */
    EXPECT_DATA(nea);
    EXEC(0x4A, 0x01, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x08, 0x00); /* メディアを含む複数 */
    EXPECT_DATA(new_media);
}

static void test_gesn_consumes_the_event_only_when_the_host_asked_for_the_whole_descriptor(void) {
    static const uint8_t new_media[8] = {0x00, 0x06, 0x04, 0x10, 0x02, 0x02, 0x00, 0x00};
    static const uint8_t no_change[8] = {0x00, 0x06, 0x04, 0x10, 0x00, 0x02, 0x00, 0x00};
    static const uint8_t cdb[10] = {0x4A, 0x01, 0, 0, 0x10, 0, 0, 0x00, 0x08, 0};
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, ID_A, false));
    EXEC(0x00, 0, 0, 0, 0, 0); /* UNIT ATTENTION を消費 */
    /* CBW の dCBWDataTransferLength が 8 未満、方向が IN でない: 応答は作るが、イベントは消えない */
    exec_expected(cdb, 10, MMC_DIR_IN, 7);
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(8, RES.data_len);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NEW_MEDIA, iso_media_peek_event(&MEDIA));
    exec_expected(cdb, 10, MMC_DIR_IN, 0);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NEW_MEDIA, iso_media_peek_event(&MEDIA));
    exec_expected(cdb, 10, MMC_DIR_NONE, 8);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NEW_MEDIA, iso_media_peek_event(&MEDIA));
    exec_expected(cdb, 10, MMC_DIR_OUT, 8);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NEW_MEDIA, iso_media_peek_event(&MEDIA));
    /* 8 バイト以上を要求した IN: 消費する */
    exec_expected(cdb, 10, MMC_DIR_IN, 8);
    EXPECT_DATA(new_media);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(&MEDIA));
    exec_expected(cdb, 10, MMC_DIR_IN, 4096);
    EXPECT_DATA(no_change);
    /* 消費されなかった場合も、次の（十分な長さの）ポーリングで、同じイベントを受け取れる */
    iso_media_force_eject(&MEDIA);
    EXEC(0x00, 0, 0, 0, 0, 0); /* UNIT ATTENTION */
    exec_expected(cdb, 10, MMC_DIR_IN, 4);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_MEDIA_REMOVAL, iso_media_peek_event(&MEDIA));
    exec_expected(cdb, 10, MMC_DIR_IN, 8);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(&MEDIA));
}

static void test_gesn_truncated_reply_does_not_consume_the_event(void) {
    static const uint8_t head[4] = {0x00, 0x06, 0x04, 0x10};
    static const uint8_t new_media[8] = {0x00, 0x06, 0x04, 0x10, 0x02, 0x02, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, ID_A, false));
    EXEC(0x00, 0, 0, 0, 0, 0);
    EXEC(0x4A, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x04, 0x00); /* 見出しだけ */
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(4, RES.data_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, OUT, 4);
    TEST_ASSERT_EQUAL_HEX8(0xA5, OUT[4]);
    EXEC(0x4A, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00); /* 割り当て長 0 */
    expect_no_data_good();
    {
        uint8_t cdb[10] = {0x4A, 0x01, 0, 0, 0x10, 0, 0, 0, 0x08, 0};
        uint8_t small[8];
        TEST_ASSERT_EQUAL_INT(0, mmc_scsi_execute(&SCSI, cdb, 10, MMC_DIR_IN, 8, small, 6, &RES)); /* out_cap が足りない */
        TEST_ASSERT_EQUAL_UINT32(6, RES.data_len);
    }
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NEW_MEDIA, iso_media_peek_event(&MEDIA)); /* まだ消えていない */
    EXEC(GESN_MEDIA);
    EXPECT_DATA(new_media);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(&MEDIA));
}

static void test_gesn_with_pending_unit_attention_reports_the_attention_first(void) {
    static const uint8_t new_media[8] = {0x00, 0x06, 0x04, 0x10, 0x02, 0x02, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, ID_A, false));
    EXEC(GESN_MEDIA);
    expect_cc(0x6, 0x28, 0x00); /* Linux の sr は、これを「メディア変更」として扱う */
    EXEC(GESN_MEDIA);
    EXPECT_DATA(new_media); /* イベントは消えていない */
}

static void test_gesn_requires_polled_and_checks_reserved(void) {
    fx_mount_quiet(ID_A);
    EXEC(0x4A, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x00); /* 非同期は未対応 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x4A, 0x03, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x00);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x4A, 0x01, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x00);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x4A, 0x01, 0x00, 0x01, 0x10, 0x00, 0x00, 0x00, 0x08, 0x00);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x4A, 0x01, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x08, 0x00);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x4A, 0x01, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00, 0x08, 0x00);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x4A, 0x01, 0x00, 0x00, 0x10, 0x01, 0x00, 0x00, 0x08, 0x00);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x4A, 0x01, 0x00, 0x00, 0x10, 0x00, 0x01, 0x00, 0x08, 0x00);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x4A, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x04);
    expect_cc(0x5, 0x24, 0x00);
}

/* ---------------------------------------------------------------- READ DISC INFORMATION / READ TRACK INFORMATION */
static void test_read_disc_information_exact(void) {
    static const uint8_t exp[34] = {0x00, 0x20, 0x0E, 0x01, 0x01, 0x01, 0x01, 0x20};
    fx_mount_quiet(ID_A);
    EXEC(0x51, 0x00, 0, 0, 0, 0, 0, 0x00, 0xFF, 0);
    EXPECT_DATA(exp);
    EXEC(0x51, 0x01, 0, 0, 0, 0, 0, 0x00, 0xFF, 0); /* トラックの資源の情報は未対応 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x51, 0x02, 0, 0, 0, 0, 0, 0x00, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x51, 0x04, 0, 0, 0, 0, 0, 0x00, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x51, 0x08, 0, 0, 0, 0, 0, 0x00, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x51, 0x00, 0, 0x01, 0, 0, 0, 0x00, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x51, 0x00, 0, 0, 0, 0, 0, 0x00, 0x08, 0); /* 割り当て長 8 */
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(8, RES.data_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, OUT, 8);
    fx_init();
    EXEC(0x51, 0x00, 0, 0, 0, 0, 0, 0x00, 0xFF, 0);
    expect_cc(0x2, 0x3A, 0x00);
}

static void test_read_track_information_exact(void) {
    static const uint8_t exp[36] = {0x00, 0x22, 0x01, 0x01, 0x00, 0x04, 0x01, 0x00, [27] = 0x65}; /* トラックの大きさ = 101 ブロック */
    static const uint8_t exp_b[36] = {0x00, 0x22, 0x01, 0x01, 0x00, 0x04, 0x01, 0x00, [27] = 0x32};
    fx_mount_quiet(ID_A);
    EXEC(0x52, 0x01, 0, 0, 0, 0x01, 0, 0x00, 0xFF, 0); /* トラック番号 1 */
    EXPECT_DATA(exp);
    EXEC(0x52, 0x00, 0, 0, 0, 0x00, 0, 0x00, 0xFF, 0); /* LBA 0 */
    EXPECT_DATA(exp);
    EXEC(0x52, 0x00, 0, 0, 0, 100, 0, 0x00, 0xFF, 0); /* 最終 LBA */
    EXPECT_DATA(exp);
    EXEC(0x52, 0x02, 0, 0, 0, 0x01, 0, 0x00, 0xFF, 0); /* セッション番号 1 */
    EXPECT_DATA(exp);
    EXEC(0x52, 0x05, 0, 0, 0, 0x01, 0, 0x00, 0xFF, 0); /* OPEN ビット（受け付ける） */
    EXPECT_DATA(exp);
    fx_mount_quiet(ID_B);
    EXEC(0x52, 0x01, 0, 0, 0, 0x01, 0, 0x00, 0xFF, 0);
    EXPECT_DATA(exp_b);
}

static void test_read_track_information_bad_addresses_and_fields(void) {
    fx_mount_quiet(ID_A);
    EXEC(0x52, 0x00, 0, 0, 0, 101, 0, 0x00, 0xFF, 0); /* 範囲外の LBA */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x52, 0x01, 0, 0, 0, 0x02, 0, 0x00, 0xFF, 0); /* トラック 2 はない */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x52, 0x01, 0, 0, 0, 0x00, 0, 0x00, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x52, 0x01, 0, 0, 0, 0xFF, 0, 0x00, 0xFF, 0); /* 見えないトラック */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x52, 0x02, 0, 0, 0, 0x02, 0, 0x00, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x52, 0x03, 0, 0, 0, 0x01, 0, 0x00, 0xFF, 0); /* 予約の型 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x52, 0x08, 0, 0, 0, 0x01, 0, 0x00, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x52, 0x01, 0, 0, 0, 0x01, 0x01, 0x00, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x52, 0x01, 0, 0, 0, 0x01, 0, 0x00, 0x08, 0); /* 割り当て長 8 */
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(8, RES.data_len);
    TEST_ASSERT_EQUAL_HEX8(0x22, OUT[1]);
    fx_init();
    EXEC(0x52, 0x01, 0, 0, 0, 0x01, 0, 0x00, 0xFF, 0);
    expect_cc(0x2, 0x3A, 0x00);
}

/* ---------------------------------------------------------------- メディアの入れ替えと応答 */
static void test_replacing_the_image_changes_the_answers_after_one_unit_attention(void) {
    static const uint8_t a[8] = {0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x08, 0x00};
    static const uint8_t b[8] = {0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x08, 0x00};
    fx_mount_quiet(ID_A);
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    EXPECT_DATA(a);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, ID_B, false));
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    expect_cc(0x6, 0x28, 0x00);
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    EXPECT_DATA(b);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_read_capacity_10_exact_bytes);
    RUN_TEST(test_read_capacity_10_without_media_is_not_ready);
    RUN_TEST(test_read_capacity_10_pmi_and_reserved_fields);
    RUN_TEST(test_read_format_capacities_exact_bytes);
    RUN_TEST(test_read10_reports_range_and_holds_one_reference);
    RUN_TEST(test_read12_reports_range_and_holds_one_reference);
    RUN_TEST(test_read_range_checks_at_every_boundary);
    RUN_TEST(test_read_uses_the_full_width_of_the_lba_and_length_fields);
    RUN_TEST(test_read_zero_blocks_is_good_without_reference_unless_lba_is_beyond_capacity);
    RUN_TEST(test_read_without_media_is_not_ready_and_takes_no_reference);
    RUN_TEST(test_read_flags_and_reserved_bits);
    RUN_TEST(test_read_acquire_failures_map_to_sense_and_leave_no_reference);
    RUN_TEST(test_command_done_is_idempotent_and_releases_exactly_once);
    RUN_TEST(test_next_execute_releases_a_forgotten_reference);
    RUN_TEST(test_failed_execute_after_a_read_leaves_no_reference);
    RUN_TEST(test_bus_reset_and_force_eject_do_not_release_the_reference);
    RUN_TEST(test_read_blocks_returns_the_backend_data);
    RUN_TEST(test_read_blocks_in_chunks_of_one_transfer);
    RUN_TEST(test_read_blocks_zero_fills_after_size_bytes);
    RUN_TEST(test_read_blocks_exact_multiple_has_no_zero_fill);
    RUN_TEST(test_read_blocks_uses_64_bit_offsets_for_large_images);
    RUN_TEST(test_read_blocks_zero_fill_of_a_partial_last_block_beyond_4_gib);
    RUN_TEST(test_read_blocks_misuse_senses);
    RUN_TEST(test_read_blocks_io_error_is_medium_error_and_clears_the_buffer);
    RUN_TEST(test_read_blocks_epoch_mismatch_is_medium_error_like_any_io_error);
    RUN_TEST(test_read_blocks_failure_in_the_middle_of_a_transfer_then_recovery);
    RUN_TEST(test_command_done_during_read_blocks_defers_the_release_until_it_returns);
    RUN_TEST(test_reset_and_command_done_during_read_blocks_abort_it_and_release_once_after);
    RUN_TEST(test_execute_during_read_blocks_does_not_replace_the_reference);
    RUN_TEST(test_read_blocks_without_the_right_to_use_the_reference_is_not_ready);
    RUN_TEST(test_abort_is_decided_before_the_backend_error_is_classified);
    RUN_TEST(test_aborted_sense_survives_until_request_sense);
    RUN_TEST(test_bus_reset_after_the_sense_was_recorded_still_discards_it);
    RUN_TEST(test_read_blocks_misuse_is_rejected_without_calling_the_backend);
    RUN_TEST(test_read_blocks_length_in_bytes_does_not_wrap_at_32_bits);
    RUN_TEST(test_web_eject_during_a_read_lets_the_command_finish_on_its_snapshot);
    RUN_TEST(test_bus_reset_aborts_the_running_read_but_keeps_the_reference_until_done);
    RUN_TEST(test_force_eject_aborts_the_running_read);
    RUN_TEST(test_reset_during_the_backend_read_is_detected_after_it_returns);
    RUN_TEST(test_a_new_command_after_bus_reset_reads_normally);
    RUN_TEST(test_read_toc_format0_lba_and_msf_exact);
    RUN_TEST(test_read_toc_format0_starting_track);
    RUN_TEST(test_read_toc_format1_multi_session_info);
    RUN_TEST(test_read_toc_format2_full_toc_is_always_msf);
    RUN_TEST(test_read_toc_msf_minutes_seconds_and_frames);
    RUN_TEST(test_read_toc_unsupported_formats_and_bad_fields);
    RUN_TEST(test_read_toc_without_media_and_with_allocation_length);
    RUN_TEST(test_read_toc_large_image_saturates_msf_at_255_59_74_but_not_lba);
    RUN_TEST(test_every_command_rejects_each_reserved_control_bit_and_accepts_vendor_bits);
    RUN_TEST(test_every_command_rejects_each_reserved_byte);
    RUN_TEST(test_two_byte_allocation_length_is_honoured_by_every_command_that_has_one);
    RUN_TEST(test_read_toc_two_byte_allocation_length_and_huge_lba_form);
    RUN_TEST(test_get_configuration_all_features_with_media);
    RUN_TEST(test_get_configuration_without_media);
    RUN_TEST(test_get_configuration_starting_feature_and_one_feature);
    RUN_TEST(test_get_configuration_allocation_length_and_reserved);
    RUN_TEST(test_gesn_reports_new_media_once_then_no_change);
    RUN_TEST(test_gesn_two_byte_allocation_length);
    RUN_TEST(test_gesn_reports_removal_for_web_eject_target_eject_and_force_eject);
    RUN_TEST(test_gesn_eject_request_event);
    RUN_TEST(test_gesn_without_the_media_class_is_nea_and_keeps_the_event);
    RUN_TEST(test_gesn_consumes_the_event_only_when_the_host_asked_for_the_whole_descriptor);
    RUN_TEST(test_gesn_truncated_reply_does_not_consume_the_event);
    RUN_TEST(test_gesn_with_pending_unit_attention_reports_the_attention_first);
    RUN_TEST(test_gesn_requires_polled_and_checks_reserved);
    RUN_TEST(test_read_disc_information_exact);
    RUN_TEST(test_read_track_information_exact);
    RUN_TEST(test_read_track_information_bad_addresses_and_fields);
    RUN_TEST(test_replacing_the_image_changes_the_answers_after_one_unit_attention);
    return UNITY_END();
}

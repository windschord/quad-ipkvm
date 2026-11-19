/*
 * mmc_scsi: メディアの内容に依存しないコマンド（INQUIRY、TEST UNIT READY、REQUEST SENSE、UNIT ATTENTION、MODE SENSE、
 * START STOP UNIT、PREVENT ALLOW、書き込み系、未対応、CDB の長さと予約ビット）。応答のバイト列は手書きの期待値。
 * 出典と、確認できなかった項目は docs/MMC_SCSI_TESTS.md 1 章。
 */
#include "mm_common.h"

void setUp(void) { fx_init(); }
void tearDown(void) {}

/* ---------------------------------------------------------------- 初期化 */
static void test_init_rejects_null_and_incomplete_backend(void) {
    mmc_scsi_t s;
    mmc_scsi_backend_t be = {&MOCK, mock_size_bytes, mock_acquire, mock_read, mock_release};
    TEST_ASSERT_EQUAL_INT(-1, mmc_scsi_init(NULL, &MEDIA, &be));
    TEST_ASSERT_EQUAL_INT(-1, mmc_scsi_init(&s, NULL, &be));
    TEST_ASSERT_EQUAL_INT(-1, mmc_scsi_init(&s, &MEDIA, NULL));
    be.size_bytes = NULL;
    TEST_ASSERT_EQUAL_INT(-1, mmc_scsi_init(&s, &MEDIA, &be));
    be.size_bytes = mock_size_bytes;
    be.acquire = NULL;
    TEST_ASSERT_EQUAL_INT(-1, mmc_scsi_init(&s, &MEDIA, &be));
    be.acquire = mock_acquire;
    be.read = NULL;
    TEST_ASSERT_EQUAL_INT(-1, mmc_scsi_init(&s, &MEDIA, &be));
    be.read = mock_read;
    be.release = NULL;
    TEST_ASSERT_EQUAL_INT(-1, mmc_scsi_init(&s, &MEDIA, &be));
}

static void test_execute_rejects_null_arguments_without_touching_result(void) {
    const uint8_t cdb[6] = {0};
    mmc_result_t r;
    memset(&r, 0x77, sizeof(r));
    TEST_ASSERT_EQUAL_INT(-1, mmc_scsi_execute(NULL, cdb, 6, MMC_DIR_NONE, 0, OUT, sizeof(OUT), &r));
    TEST_ASSERT_EQUAL_INT(-1, mmc_scsi_execute(&SCSI, NULL, 6, MMC_DIR_NONE, 0, OUT, sizeof(OUT), &r));
    TEST_ASSERT_EQUAL_INT(-1, mmc_scsi_execute(&SCSI, cdb, 6, MMC_DIR_NONE, 0, OUT, sizeof(OUT), NULL));
    TEST_ASSERT_EQUAL_HEX8(0x77, ((uint8_t *)&r)[0]);
    mmc_scsi_command_done(NULL); /* クラッシュしない */
    TEST_ASSERT_FALSE(mmc_scsi_holds_reference(NULL));
    TEST_ASSERT_FALSE(mmc_scsi_last_sense(NULL, NULL));
}

/* ---------------------------------------------------------------- INQUIRY（SPC-4 の INQUIRY の標準データ） */
static const uint8_t K_INQUIRY[36] = {
    0x05, 0x80, 0x05, 0x02, 0x1F, 0x00, 0x00, 0x00,                                 /* 種別 5、RMB、SPC-3、形式 2、長さ 31 */
    0x51, 0x55, 0x41, 0x44, 0x4B, 0x56, 0x4D, 0x20,                                 /* "QUADKVM " */
    0x56, 0x69, 0x72, 0x74, 0x75, 0x61, 0x6C, 0x20, 0x43, 0x44, 0x2D, 0x52, 0x4F, 0x4D, 0x20, 0x20, /* "Virtual CD-ROM  " */
    0x30, 0x30, 0x30, 0x31};                                                        /* "0001" */

static void test_inquiry_standard_data_is_exact_with_and_without_media(void) {
    EXEC(0x12, 0, 0, 0, 0xFF, 0);
    EXPECT_DATA(K_INQUIRY);
    fx_mount_quiet(ID_A);
    EXEC(0x12, 0, 0, 0, 36, 0);
    EXPECT_DATA(K_INQUIRY);
    EXEC(0x12, 0, 0, 0x01, 0x00, 0); /* 割り当て長 256 */
    EXPECT_DATA(K_INQUIRY);
}

static void test_inquiry_is_cut_by_allocation_length_and_out_cap(void) {
    EXEC(0x12, 0, 0, 0, 5, 0);
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(5, RES.data_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(K_INQUIRY, OUT, 5);
    TEST_ASSERT_EQUAL_HEX8(0xA5, OUT[5]);
    EXEC(0x12, 0, 0, 0, 0, 0);
    expect_no_data_good();
    {
        const uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0};
        uint8_t small[16];
        memset(small, 0xA5, sizeof(small));
        TEST_ASSERT_EQUAL_INT(0, mmc_scsi_execute(&SCSI, cdb, 6, MMC_DIR_IN, 36, small, 10, &RES));
        TEST_ASSERT_EQUAL_UINT32(10, RES.data_len); /* out_cap で切り詰める */
        TEST_ASSERT_EQUAL_HEX8_ARRAY(K_INQUIRY, small, 10);
        TEST_ASSERT_EQUAL_HEX8(0xA5, small[10]);
        TEST_ASSERT_EQUAL_INT(0, mmc_scsi_execute(&SCSI, cdb, 6, MMC_DIR_IN, 36, NULL, 0, &RES));
        TEST_ASSERT_EQUAL_UINT32(0, RES.data_len);
        TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, RES.status);
    }
}

static void test_inquiry_vpd_and_page_code_and_cmddt_are_illegal_request(void) {
    EXEC(0x12, 0x01, 0x00, 0, 0xFF, 0); /* EVPD = 1 の 00h（対応するページの一覧）も未対応 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x12, 0x01, 0x80, 0, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x12, 0x00, 0x80, 0, 0xFF, 0); /* EVPD = 0 でページコードが 0 でない */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x12, 0x02, 0x00, 0, 0xFF, 0); /* CMDDT（廃止） */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x12, 0x20, 0x00, 0, 0xFF, 0); /* 予約ビット */
    expect_cc(0x5, 0x24, 0x00);
}

/* ---------------------------------------------------------------- TEST UNIT READY / UNIT ATTENTION / REQUEST SENSE */
static void test_tur_good_with_media_and_not_ready_without(void) {
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x2, 0x3A, 0x00); /* NOT READY / MEDIUM NOT PRESENT */
    fx_mount_quiet(ID_A);
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_no_data_good();
}

static void test_tur_media_size_lookup_failure_is_medium_not_present(void) {
    fx_mount_quiet(ID_UNKNOWN); /* バックエンドが知らない id */
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x2, 0x3A, 0x00);
    fx_mount_quiet(ID_A);
    MOCK.size_rc = ISO_ERR_IO;
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x2, 0x3A, 0x00);
}

static void test_unit_attention_media_is_reported_once_by_the_next_command(void) {
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, ID_A, false));
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x6, 0x28, 0x00); /* UNIT ATTENTION / NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED */
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_no_data_good();
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_no_data_good();
}

static void test_unit_attention_is_not_consumed_by_inquiry_or_request_sense(void) {
    static const uint8_t no_sense[18] = {0x70, 0, 0, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, ID_A, false));
    EXEC(0x12, 0, 0, 0, 36, 0);
    EXPECT_DATA(K_INQUIRY);
    EXEC(0x03, 0, 0, 0, 18, 0);
    EXPECT_DATA(no_sense); /* REQUEST SENSE は、保留中の UNIT ATTENTION を報告せず、消さない */
    TEST_ASSERT_TRUE(iso_media_unit_attention_pending(&MEDIA));
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x6, 0x28, 0x00);
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(&MEDIA));
}

static void test_unit_attention_precedes_every_other_command_including_invalid_ones(void) {
    const uint8_t opcodes[] = {0x25, 0x28, 0x43, 0x46, 0x4A, 0x5A, 0x1A, 0x1B, 0x1E, 0x51, 0x52, 0x23, 0xA8, 0x15, 0x2A, 0xFF};
    for (size_t i = 0; i < sizeof(opcodes); i++) {
        uint8_t cdb[16] = {0};
        cdb[0] = opcodes[i];
        fx_init();
        TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, ID_A, false));
        exec(cdb, 12);
        expect_cc(0x6, 0x28, 0x00);
        TEST_ASSERT_FALSE(mmc_scsi_holds_reference(&SCSI));
    }
}

static void test_bus_reset_reports_29h_then_28h_each_once(void) {
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, ID_A, false));
    iso_media_bus_reset(&MEDIA);
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x6, 0x29, 0x00); /* POWER ON, RESET, OR BUS DEVICE RESET OCCURRED が先 */
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x6, 0x28, 0x00);
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_no_data_good();
}

static void test_request_sense_returns_fixed_format_and_clears(void) {
    static const uint8_t not_ready[18] = {0x70, 0x00, 0x02, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x3A, 0x00, 0, 0, 0, 0};
    static const uint8_t no_sense[18] = {0x70, 0x00, 0x00, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x00, 0x00, 0, 0, 0, 0};
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x2, 0x3A, 0x00);
    EXEC(0x03, 0, 0, 0, 18, 0);
    EXPECT_DATA(not_ready);
    EXEC(0x03, 0, 0, 0, 18, 0);
    EXPECT_DATA(no_sense); /* 返したら消える */
}

static void test_request_sense_after_unit_attention_and_after_illegal_request(void) {
    static const uint8_t ua[18] = {0x70, 0x00, 0x06, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x28, 0x00, 0, 0, 0, 0};
    static const uint8_t ir[18] = {0x70, 0x00, 0x05, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x20, 0x00, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&MEDIA, ID_A, false));
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x6, 0x28, 0x00);
    EXEC(0x03, 0, 0, 0, 18, 0);
    EXPECT_DATA(ua);
    EXEC(0x55, 0, 0, 0, 0, 0, 0, 0, 0, 0); /* MODE SELECT(10): 未対応 */
    expect_cc(0x5, 0x20, 0x00);
    EXEC(0x03, 0, 0, 0, 18, 0);
    EXPECT_DATA(ir);
}

static void test_request_sense_is_cut_and_zero_length_keeps_the_sense(void) {
    EXEC(0x00, 0, 0, 0, 0, 0); /* NOT READY 3A/00 */
    EXEC(0x03, 0, 0, 0, 0, 0);
    expect_no_data_good(); /* 割り当て長 0: 何も返さず、センスを消さない */
    EXEC(0x03, 0, 0, 0, 8, 0);
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(8, RES.data_len);
    {
        static const uint8_t first8[8] = {0x70, 0x00, 0x02, 0, 0, 0, 0, 0x0A};
        TEST_ASSERT_EQUAL_HEX8_ARRAY(first8, OUT, 8);
        TEST_ASSERT_EQUAL_HEX8(0xA5, OUT[8]);
    }
    EXEC(0x03, 0, 0, 0, 252, 0); /* 大きな割り当て長でも 18 バイト */
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(18, RES.data_len);
}

static void test_request_sense_descriptor_format_and_reserved_are_rejected(void) {
    EXEC(0x03, 0x01, 0, 0, 18, 0); /* DESC = 1 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x03, 0x02, 0, 0, 18, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x03, 0, 0x01, 0, 18, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x03, 0, 0, 0x01, 18, 0);
    expect_cc(0x5, 0x24, 0x00);
}

static void test_inquiry_does_not_clear_the_stored_sense_but_other_commands_do(void) {
    static const uint8_t not_ready[18] = {0x70, 0x00, 0x02, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x3A, 0x00, 0, 0, 0, 0};
    static const uint8_t no_sense[18] = {0x70, 0x00, 0x00, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x00, 0x00, 0, 0, 0, 0};
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x2, 0x3A, 0x00);
    EXEC(0x12, 0, 0, 0, 36, 0);
    EXEC(0x03, 0, 0, 0, 18, 0);
    EXPECT_DATA(not_ready); /* INQUIRY を挟んでも残る */
    EXEC(0x00, 0, 0, 0, 0, 0);
    EXEC(0x1A, 0x00, 0x3F, 0, 255, 0); /* GOOD の MODE SENSE（INQUIRY / REQUEST SENSE 以外）が、センスを消す */
    expect_good();
    EXEC(0x03, 0, 0, 0, 18, 0);
    EXPECT_DATA(no_sense);
}

static void test_bus_reset_discards_the_stored_sense(void) {
    static const uint8_t no_sense[18] = {0x70, 0x00, 0x00, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x00, 0x00, 0, 0, 0, 0};
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x2, 0x3A, 0x00);
    iso_media_bus_reset(&MEDIA);
    EXEC(0x03, 0, 0, 0, 18, 0);
    EXPECT_DATA(no_sense);
    TEST_ASSERT_FALSE(mmc_scsi_last_sense(&SCSI, NULL));
}

static void test_build_sense_layout(void) {
    uint8_t b[MMC_SCSI_SENSE_LEN];
    static const uint8_t exp_medium[18] = {0x70, 0x00, 0x03, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x11, 0x00, 0, 0, 0, 0};
    static const uint8_t exp_aborted[18] = {0x70, 0x00, 0x0B, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x00, 0x00, 0, 0, 0, 0};
    static const uint8_t exp_ascq[18] = {0x70, 0x00, 0x05, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x53, 0x02, 0, 0, 0, 0};
    mmc_sense_t s = {0x3, 0x11, 0x00};
    memset(b, 0xCC, sizeof(b));
    mmc_scsi_build_sense(&s, b);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp_medium, b, 18);
    s.key = 0xB;
    s.asc = 0x00;
    memset(b, 0xCC, sizeof(b));
    mmc_scsi_build_sense(&s, b);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp_aborted, b, 18); /* センスキーは 4 ビット */
    s.key = 0x5;
    s.asc = 0x53;
    s.ascq = 0x02;
    memset(b, 0xCC, sizeof(b));
    mmc_scsi_build_sense(&s, b);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp_ascq, b, 18);
}

static void test_request_sense_after_bus_reset_reports_the_29h_sense_of_the_failed_command(void) {
    static const uint8_t ua29[18] = {0x70, 0x00, 0x06, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x29, 0x00, 0, 0, 0, 0};
    fx_mount_quiet(ID_A);
    iso_media_bus_reset(&MEDIA);
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x6, 0x29, 0x00);
    EXEC(0x03, 0, 0, 0, 18, 0); /* リセット後の最初のコマンドのセンスは、消えない */
    EXPECT_DATA(ua29);
}

static void test_zero_length_cdb_is_rejected_whatever_the_first_byte_is(void) {
    uint8_t cdb[16] = {0x2A}; /* 長さ 0 では、先頭のバイトを見ない */
    exec(cdb, 0);
    expect_cc(0x5, 0x24, 0x00);
}

static void test_inconsistent_null_out_with_capacity_returns_no_data(void) {
    const uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0};
    TEST_ASSERT_EQUAL_INT(0, mmc_scsi_execute(&SCSI, cdb, 6, MMC_DIR_IN, 36, NULL, 100, &RES));
    TEST_ASSERT_EQUAL_INT(MMC_STATUS_GOOD, RES.status);
    TEST_ASSERT_EQUAL_UINT32(0, RES.data_len);
}

static void test_medium_with_zero_size_or_failing_size_lookup_is_not_present(void) {
    fx_mount_quiet(ID_ZERO);
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x2, 0x3A, 0x00);
    fx_mount_quiet(ID_A);
    MOCK.size_rc = ISO_ERR_IO; /* 失敗のとき、バックエンドは *out にゴミを入れる */
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x2, 0x3A, 0x00);
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    expect_cc(0x2, 0x3A, 0x00);
}

/* ---------------------------------------------------------------- 未対応と書き込み系 */
static void test_unsupported_opcodes_are_illegal_request_20h(void) {
    /* MODE SELECT(6/10)、READ SUB-CHANNEL、READ CD、GET PERFORMANCE、READ(16)、REPORT LUNS、SYNCHRONIZE CACHE、SEEK、
     * 予約のグループ（60h）、ベンダー固有（C0h、FFh）、READ(6)、READ HEADER、SET CD SPEED、MECHANISM STATUS */
    const uint8_t opcodes[] = {0x15, 0x55, 0x42, 0xBE, 0xAC, 0x88, 0xA0, 0x35, 0x2B, 0x60, 0xC0, 0xFF, 0x08, 0x44, 0xBB, 0xBD};
    for (int media = 0; media < 2; media++) {
        fx_init();
        if (media) fx_mount_quiet(ID_A);
        for (size_t i = 0; i < sizeof(opcodes); i++) {
            uint8_t cdb[16] = {0};
            cdb[0] = opcodes[i];
            exec(cdb, 16);
            expect_cc(0x5, 0x20, 0x00);
            TEST_ASSERT_FALSE(mmc_scsi_holds_reference(&SCSI));
        }
    }
}

static void test_write_commands_are_data_protect_and_take_no_data(void) {
    /* WRITE(6/10/12/16)、WRITE AND VERIFY(10/12/16)、WRITE SAME(10/16)、FORMAT UNIT、BLANK */
    const uint8_t opcodes[] = {0x0A, 0x2A, 0xAA, 0x8A, 0x2E, 0xAE, 0x8E, 0x41, 0x93, 0x04, 0xA1};
    for (int media = 0; media < 2; media++) {
        fx_init();
        if (media) fx_mount_quiet(ID_A);
        for (size_t i = 0; i < sizeof(opcodes); i++) {
            uint8_t cdb[16] = {0};
            cdb[0] = opcodes[i];
            cdb[8] = 1; /* 転送長（あっても受け取らない） */
            exec_dir(cdb, 16, MMC_DIR_OUT);
            expect_cc(0x7, 0x27, 0x00); /* DATA PROTECT / WRITE PROTECTED */
            TEST_ASSERT_FALSE(mmc_scsi_holds_reference(&SCSI));
            TEST_ASSERT_EQUAL_HEX8(0xA5, OUT[0]);
        }
    }
    /* 短い CDB でも、書き込みは DATA PROTECT（長さの検査より先） */
    fx_init();
    EXEC(0x2A);
    expect_cc(0x7, 0x27, 0x00);
}

/* ---------------------------------------------------------------- CDB の長さと予約ビット */
static void test_cdb_length_is_checked_per_opcode_group(void) {
    fx_mount_quiet(ID_A);
    {
        uint8_t cdb[16] = {0};
        exec(cdb, 0);
        expect_cc(0x5, 0x24, 0x00);
        exec(cdb, 17);
        expect_cc(0x5, 0x24, 0x00);
        cdb[0] = 0x00; /* TUR は 6 バイト */
        exec(cdb, 5);
        expect_cc(0x5, 0x24, 0x00);
        exec(cdb, 6);
        expect_no_data_good();
        exec(cdb, 12); /* パディングは許す */
        expect_no_data_good();
        exec(cdb, 16);
        expect_no_data_good();
        cdb[0] = 0x25; /* READ CAPACITY(10) は 10 バイト */
        exec(cdb, 9);
        expect_cc(0x5, 0x24, 0x00);
        exec(cdb, 10);
        expect_good();
        cdb[0] = 0xA8; /* READ(12) は 12 バイト */
        cdb[9] = 1;
        exec(cdb, 11);
        expect_cc(0x5, 0x24, 0x00);
        exec(cdb, 12);
        expect_good();
        TEST_ASSERT_TRUE(mmc_scsi_holds_reference(&SCSI));
        mmc_scsi_command_done(&SCSI);
        cdb[0] = 0x1A;
        cdb[2] = 0x3F;
        exec(cdb, 5);
        expect_cc(0x5, 0x24, 0x00);
    }
}

static void test_reserved_fields_and_control_byte_are_checked(void) {
    fx_mount_quiet(ID_A);
    EXEC(0x00, 0x01, 0, 0, 0, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x00, 0, 0x01, 0, 0, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x00, 0, 0, 0, 0x80, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x00, 0, 0, 0, 0, 0x04); /* NACA */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x00, 0, 0, 0, 0, 0x01); /* LINK */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x00, 0, 0, 0, 0, 0x08); /* 予約 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x00, 0, 0, 0, 0, 0x40); /* ベンダー固有のビットは許す */
    expect_no_data_good();
    EXEC(0x00, 0, 0, 0, 0, 0x80);
    expect_no_data_good();
}

/* ---------------------------------------------------------------- MODE SENSE */
/* ページ 2Ah（22 バイト）: 長さ 0x14、DVD-ROM 読み出し（0x08）、ローディング機構 = トレイ + イジェクト + ロック（0x29） */
#define PAGE_2A_UNLOCKED                                                                                          \
    0x2A, 0x14, 0x08, 0x00, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
        0x00, 0x00, 0x00, 0x00

static void test_mode_sense_10_page_2a_exact(void) {
    static const uint8_t exp[] = {0x00, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, PAGE_2A_UNLOCKED};
    EXEC(0x5A, 0x00, 0x2A, 0x00, 0, 0, 0, 0x00, 0xFF, 0);
    EXPECT_DATA(exp);
    TEST_ASSERT_EQUAL_UINT32(30, RES.data_len);
}

static void test_mode_sense_6_page_2a_and_all_pages_exact(void) {
    static const uint8_t exp[] = {0x19, 0x00, 0x00, 0x00, PAGE_2A_UNLOCKED};
    EXEC(0x1A, 0x00, 0x2A, 0x00, 0xFF, 0);
    EXPECT_DATA(exp);
    EXEC(0x1A, 0x08, 0x3F, 0x00, 0xFF, 0); /* DBD = 1、全ページ */
    EXPECT_DATA(exp);
    EXEC(0x1A, 0x00, 0x3F, 0xFF, 0xFF, 0); /* 全ページと全サブページ */
    EXPECT_DATA(exp);
}

static void test_mode_sense_works_without_media_and_shows_lock_state(void) {
    static const uint8_t locked[] = {0x19, 0x00, 0x00, 0x00, 0x2A, 0x14, 0x08, 0x00, 0x00, 0x00, 0x2B, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXEC(0x1A, 0x00, 0x2A, 0x00, 0xFF, 0);
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(26, RES.data_len);
    fx_mount_quiet(ID_A);
    EXEC(0x1E, 0, 0, 0, 0x01, 0); /* PREVENT */
    expect_no_data_good();
    EXEC(0x1A, 0x00, 0x2A, 0x00, 0xFF, 0);
    EXPECT_DATA(locked); /* ロックの状態（ページの byte 6 のビット 1）が立つ */
}

static void test_mode_sense_changeable_values_are_all_zero_and_saved_is_rejected(void) {
    static const uint8_t chg[] = {0x19, 0x00, 0x00, 0x00, 0x2A, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t dflt[] = {0x19, 0x00, 0x00, 0x00, PAGE_2A_UNLOCKED};
    EXEC(0x1A, 0x00, 0x6A, 0x00, 0xFF, 0); /* PC = 01b */
    EXPECT_DATA(chg);
    EXEC(0x1A, 0x00, 0xAA, 0x00, 0xFF, 0); /* PC = 10b（既定値） */
    EXPECT_DATA(dflt);
    EXEC(0x1A, 0x00, 0xEA, 0x00, 0xFF, 0); /* PC = 11b（保存値） */
    expect_cc(0x5, 0x39, 0x00);            /* SAVING PARAMETERS NOT SUPPORTED */
}

static void test_mode_sense_unsupported_pages_and_bad_fields(void) {
    EXEC(0x1A, 0x00, 0x01, 0x00, 0xFF, 0); /* 読み出しエラー回復ページは未対応 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1A, 0x00, 0x2A, 0x01, 0xFF, 0); /* サブページ */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1A, 0x00, 0x2A, 0xFF, 0xFF, 0); /* 2Ah の FFh は無効 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1A, 0x00, 0x3F, 0x01, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1A, 0x10, 0x2A, 0x00, 0xFF, 0); /* LLBAA は MODE SENSE(6) にない */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x5A, 0x00, 0x2A, 0x00, 0x01, 0, 0, 0, 0xFF, 0); /* (10) の予約 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x5A, 0x04, 0x2A, 0x00, 0, 0, 0, 0, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
}

static void test_mode_sense_reserved_bytes_control_and_two_byte_allocation_length(void) {
    EXEC(0x5A, 0x00, 0x2A, 0x00, 0, 0x01, 0, 0, 0xFF, 0); /* (10) の byte 5、6 は予約 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x5A, 0x00, 0x2A, 0x00, 0, 0, 0x01, 0, 0xFF, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x5A, 0x00, 0x2A, 0x00, 0, 0, 0, 0, 0xFF, 0x04); /* NACA */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x5A, 0x00, 0x2A, 0x00, 0, 0, 0, 0, 0xFF, 0x40); /* ベンダー固有のビットは許す */
    expect_good();
    EXEC(0x5A, 0x08, 0x2A, 0x00, 0, 0, 0, 0, 0xFF, 0x00); /* DBD = 1（ブロック記述子なしなので、そのまま返す） */
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(30, RES.data_len);
    EXEC(0x1A, 0x00, 0x2A, 0x00, 0xFF, 0x04);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1A, 0x00, 0x2A, 0x00, 0xFF, 0x80);
    expect_good();
    EXEC(0x5A, 0x00, 0x2A, 0x00, 0, 0, 0, 0x01, 0x00, 0); /* 割り当て長 256（上位バイトが 1） */
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(30, RES.data_len);
}

static void test_mode_sense_allocation_length_cuts_but_length_field_is_full(void) {
    EXEC(0x1A, 0x00, 0x2A, 0x00, 6, 0);
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(6, RES.data_len);
    TEST_ASSERT_EQUAL_HEX8(0x19, OUT[0]); /* 全体の長さを示す */
    EXEC(0x1A, 0x00, 0x2A, 0x00, 0, 0);
    expect_no_data_good();
    EXEC(0x5A, 0x10, 0x2A, 0x00, 0, 0, 0, 0x00, 0x08, 0); /* LLBAA（ブロック記述子なし。許す）、割り当て長 8 */
    expect_good();
    TEST_ASSERT_EQUAL_UINT32(8, RES.data_len);
    TEST_ASSERT_EQUAL_HEX8(0x1C, OUT[1]);
}

/* ---------------------------------------------------------------- START STOP UNIT */
static void test_start_stop_unit_load_and_stop_do_nothing(void) {
    fx_mount_quiet(ID_A);
    EXEC(0x1B, 0, 0, 0, 0x03, 0); /* LoEj = 1、Start = 1: ロード */
    expect_no_data_good();
    EXEC(0x1B, 0, 0, 0, 0x01, 0); /* Start */
    expect_no_data_good();
    EXEC(0x1B, 0, 0, 0, 0x00, 0); /* Stop（LoEj = 0） */
    expect_no_data_good();
    EXEC(0x1B, 0x01, 0, 0, 0x01, 0); /* IMMED */
    expect_no_data_good();
    TEST_ASSERT_TRUE(iso_media_is_loaded(&MEDIA));
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_no_data_good();
}

static void test_start_stop_unit_eject_removes_media_and_records_it_for_the_web(void) {
    iso_media_status_t st;
    fx_mount_quiet(ID_A);
    EXEC(0x1B, 0, 0, 0, 0x02, 0); /* LoEj = 1、Start = 0 */
    expect_no_data_good();
    iso_media_get_status(&MEDIA, &st);
    TEST_ASSERT_FALSE(st.mounted);
    TEST_ASSERT_TRUE(st.ejected_by_target);
    EXEC(0x00, 0, 0, 0, 0, 0);
    expect_cc(0x2, 0x3A, 0x00);
    EXEC(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    expect_cc(0x2, 0x3A, 0x00);
    EXEC(0x1B, 0, 0, 0, 0x02, 0); /* すでに EMPTY: GOOD（何も変えない） */
    expect_no_data_good();
    iso_media_get_status(&MEDIA, &st);
    TEST_ASSERT_TRUE(st.ejected_by_target);
}

static void test_start_stop_unit_eject_is_refused_when_removal_is_prevented(void) {
    fx_mount_quiet(ID_A);
    EXEC(0x1E, 0, 0, 0, 0x01, 0);
    expect_no_data_good();
    EXEC(0x1B, 0, 0, 0, 0x02, 0);
    expect_cc(0x5, 0x53, 0x02); /* ILLEGAL REQUEST / MEDIA REMOVAL PREVENTED */
    TEST_ASSERT_TRUE(iso_media_is_loaded(&MEDIA));
    {
        iso_media_status_t st;
        iso_media_get_status(&MEDIA, &st);
        TEST_ASSERT_FALSE(st.ejected_by_target); /* 取り外していないので、記録もしない */
        TEST_ASSERT_TRUE(st.locked);
    }
    EXEC(0x1E, 0, 0, 0, 0x00, 0); /* ALLOW */
    expect_no_data_good();
    EXEC(0x1B, 0, 0, 0, 0x02, 0);
    expect_no_data_good();
    TEST_ASSERT_FALSE(iso_media_is_loaded(&MEDIA));
}

static void test_start_stop_unit_rejects_power_conditions_and_reserved_bits(void) {
    fx_mount_quiet(ID_A);
    EXEC(0x1B, 0, 0, 0, 0x10, 0); /* POWER CONDITION = 1 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1B, 0, 0, 0, 0x04, 0); /* 予約 */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1B, 0, 0, 0, 0x08, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1B, 0x02, 0, 0, 0x01, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1B, 0, 0x01, 0, 0x01, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1B, 0, 0, 0x01, 0x01, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1B, 0, 0, 0, 0x01, 0x04);
    expect_cc(0x5, 0x24, 0x00);
    TEST_ASSERT_TRUE(iso_media_is_loaded(&MEDIA)); /* どれも取り外していない */
}

/* ---------------------------------------------------------------- PREVENT ALLOW MEDIUM REMOVAL */
static void test_prevent_allow_sets_and_clears_the_flag_seen_by_the_web(void) {
    iso_media_status_t st;
    fx_mount_quiet(ID_A);
    EXEC(0x1E, 0, 0, 0, 0x01, 0);
    expect_no_data_good();
    iso_media_get_status(&MEDIA, &st);
    TEST_ASSERT_TRUE(st.locked);
    EXEC(0x1E, 0, 0, 0, 0x00, 0);
    expect_no_data_good();
    iso_media_get_status(&MEDIA, &st);
    TEST_ASSERT_FALSE(st.locked);
}

static void test_prevent_without_media_is_not_ready_and_allow_is_good(void) {
    EXEC(0x1E, 0, 0, 0, 0x01, 0);
    expect_cc(0x2, 0x3A, 0x00);
    TEST_ASSERT_FALSE(iso_media_prevent_removal(&MEDIA));
    EXEC(0x1E, 0, 0, 0, 0x00, 0);
    expect_no_data_good();
}

static void test_prevent_rejects_persistent_and_reserved_bits(void) {
    fx_mount_quiet(ID_A);
    EXEC(0x1E, 0, 0, 0, 0x02, 0); /* PERSISTENT */
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1E, 0, 0, 0, 0x03, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1E, 0, 0, 0, 0x04, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1E, 0x01, 0, 0, 0x01, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1E, 0, 0x01, 0, 0x01, 0);
    expect_cc(0x5, 0x24, 0x00);
    EXEC(0x1E, 0, 0, 0x01, 0x01, 0);
    expect_cc(0x5, 0x24, 0x00);
    TEST_ASSERT_FALSE(iso_media_prevent_removal(&MEDIA));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_null_and_incomplete_backend);
    RUN_TEST(test_execute_rejects_null_arguments_without_touching_result);
    RUN_TEST(test_inquiry_standard_data_is_exact_with_and_without_media);
    RUN_TEST(test_inquiry_is_cut_by_allocation_length_and_out_cap);
    RUN_TEST(test_inquiry_vpd_and_page_code_and_cmddt_are_illegal_request);
    RUN_TEST(test_tur_good_with_media_and_not_ready_without);
    RUN_TEST(test_tur_media_size_lookup_failure_is_medium_not_present);
    RUN_TEST(test_unit_attention_media_is_reported_once_by_the_next_command);
    RUN_TEST(test_unit_attention_is_not_consumed_by_inquiry_or_request_sense);
    RUN_TEST(test_unit_attention_precedes_every_other_command_including_invalid_ones);
    RUN_TEST(test_bus_reset_reports_29h_then_28h_each_once);
    RUN_TEST(test_request_sense_returns_fixed_format_and_clears);
    RUN_TEST(test_request_sense_after_unit_attention_and_after_illegal_request);
    RUN_TEST(test_request_sense_is_cut_and_zero_length_keeps_the_sense);
    RUN_TEST(test_request_sense_descriptor_format_and_reserved_are_rejected);
    RUN_TEST(test_inquiry_does_not_clear_the_stored_sense_but_other_commands_do);
    RUN_TEST(test_bus_reset_discards_the_stored_sense);
    RUN_TEST(test_build_sense_layout);
    RUN_TEST(test_request_sense_after_bus_reset_reports_the_29h_sense_of_the_failed_command);
    RUN_TEST(test_zero_length_cdb_is_rejected_whatever_the_first_byte_is);
    RUN_TEST(test_inconsistent_null_out_with_capacity_returns_no_data);
    RUN_TEST(test_medium_with_zero_size_or_failing_size_lookup_is_not_present);
    RUN_TEST(test_unsupported_opcodes_are_illegal_request_20h);
    RUN_TEST(test_write_commands_are_data_protect_and_take_no_data);
    RUN_TEST(test_cdb_length_is_checked_per_opcode_group);
    RUN_TEST(test_reserved_fields_and_control_byte_are_checked);
    RUN_TEST(test_mode_sense_10_page_2a_exact);
    RUN_TEST(test_mode_sense_6_page_2a_and_all_pages_exact);
    RUN_TEST(test_mode_sense_works_without_media_and_shows_lock_state);
    RUN_TEST(test_mode_sense_changeable_values_are_all_zero_and_saved_is_rejected);
    RUN_TEST(test_mode_sense_unsupported_pages_and_bad_fields);
    RUN_TEST(test_mode_sense_reserved_bytes_control_and_two_byte_allocation_length);
    RUN_TEST(test_mode_sense_allocation_length_cuts_but_length_field_is_full);
    RUN_TEST(test_start_stop_unit_load_and_stop_do_nothing);
    RUN_TEST(test_start_stop_unit_eject_removes_media_and_records_it_for_the_web);
    RUN_TEST(test_start_stop_unit_eject_is_refused_when_removal_is_prevented);
    RUN_TEST(test_start_stop_unit_rejects_power_conditions_and_reserved_bits);
    RUN_TEST(test_prevent_allow_sets_and_clears_the_flag_seen_by_the_web);
    RUN_TEST(test_prevent_without_media_is_not_ready_and_allow_is_good);
    RUN_TEST(test_prevent_rejects_persistent_and_reserved_bits);
    return UNITY_END();
}

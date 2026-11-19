/*
 * msc_bot: CBW の検証と、Reset Recovery。
 * 出典（BOT 1.0）:
 *   5.1  CBW の形（署名 43425355h、31 バイト、予約ビット、bCBWLUN、bCBWCBLength 1..16）
 *   6.2.1 valid CBW: 31 バイトで、署名が一致   6.2.2 meaningful CBW: 予約ビット = 0、LUN が有効、CDB の長さが範囲内
 *   6.6.1 CBW が不正: Bulk-IN を STALL。Bulk-OUT も STALL（または読み捨て）。Reset Recovery まで保つ
 *   3.1  Bulk-Only Mass Storage Reset: STALL の状態を保つ。   5.3.4 Reset Recovery = Reset、CLEAR_FEATURE(HALT) IN、同 OUT
 * meaningful でない CBW への応答は、BOT 6.4 で未規定。issue #143 の指示により、不正な CBW と同じ（両 Bulk を STALL）にしている。
 */
#include "mb_common.h"

void setUp(void) {}
void tearDown(void) {}

static void make_valid(uint8_t raw[31]) {
    const uint8_t cdb[6] = {0x00, 0, 0, 0, 0, 0};
    mb_make_cbw(raw, 0xABCD0001u, 0, false, cdb, 6);
}

/* 不正な CBW を送り、BOT 6.6.1 の応答（両 Bulk を STALL、依頼なし、command_done なし）を確かめる。 */
static void expect_invalid(const uint8_t *raw, uint32_t actual_len, const char *name) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    mb_clear_log(m);
    mb_send_raw_cbw(m, raw, actual_len);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("RECOVERY", msc_bot_state_name(msc_bot_state(&m->bot)), name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("stallI stallO", mb_log_str(m), name);
    TEST_ASSERT_TRUE_MESSAGE(m->hw_in_stalled && m->hw_out_stalled, name);
    TEST_ASSERT_FALSE_MESSAGE(m->out_pending, name); /* STALL 中の Bulk-OUT に、受信を発行しない */
    TEST_ASSERT_FALSE_MESSAGE(m->in_pending, name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, m->n_exec, name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, m->n_done, name); /* 実行していないので、command_done も呼ばない */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, msc_bot_stats(&m->bot).invalid_cbw, name);
    TEST_ASSERT_TRUE_MESSAGE(msc_bot_halted(&m->bot, MSC_BOT_EP_IN) && msc_bot_halted(&m->bot, MSC_BOT_EP_OUT), name);
    mb_assert_clean(m);
}

/* ---- 有効な CBW ----------------------------------------------------------- */
static void test_valid_cbw_all_cdb_lengths_pass_the_cdb_through(void) {
    for (uint8_t len = 1; len <= 16; len++) {
        mb_t *m = mb_init(2048, false);
        m->hold_cmd = true;
        mb_start(m);
        uint8_t cdb[16];
        for (int i = 0; i < 16; i++) cdb[i] = (uint8_t)(0x30 + i);
        cdb[0] = 0x00;
        uint8_t raw[31];
        mb_make_cbw(raw, 7, 0, false, cdb, len);
        for (int i = 15 + len; i < 31; i++) raw[i] = 0xEE; /* CBWCB の有効長より後ろは、無視される（BOT 5.1） */
        mb_send_raw_cbw(m, raw, 31);
        MB_ASSERT_STATE(m, MSC_BOT_EXEC);
        TEST_ASSERT_TRUE(m->cmd_pending);
        TEST_ASSERT_EQUAL_UINT8(len, m->cmd.cdb_len);
        TEST_ASSERT_EQUAL_UINT8(0x00, m->cmd.cdb[0]);
        for (int i = 1; i < len; i++) TEST_ASSERT_EQUAL_HEX8((uint8_t)(0x30 + i), m->cmd.cdb[i]);
        for (int i = len; i < 16; i++) TEST_ASSERT_EQUAL_HEX8(0, m->cmd.cdb[i]); /* 有効長より後ろは渡さない */
        mb_assert_clean(m);
    }
}

static void test_valid_cbw_with_direction_bit_and_zero_length(void) {
    /* BOT 5.1: dCBWDataTransferLength = 0 のとき、方向のビットは無視される。 */
    for (int in = 0; in < 2; in++) {
        mb_t *m = mb_init(2048, false);
        m->hold_cmd = true;
        mb_start(m);
        const uint8_t cdb[6] = {0};
        mb_send_cbw(m, 1, 0, in != 0, cdb, 6);
        MB_ASSERT_STATE(m, MSC_BOT_EXEC);
        TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_NONE, m->cmd.dir);
        TEST_ASSERT_EQUAL_UINT32(0, m->cmd.expected_len);
        mb_assert_clean(m);
    }
}

static void test_execute_command_fields(void) {
    mb_t *m = mb_init(4096, false);
    m->hold_cmd = true;
    mb_start(m);
    const uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 7, 0, 0, 3, 0};
    mb_send_cbw(m, 0x55, 6144, true, cdb, 10);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_CMD_EXECUTE, m->cmd.kind);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_IN, m->cmd.dir);
    TEST_ASSERT_EQUAL_UINT32(6144, m->cmd.expected_len);
    TEST_ASSERT_TRUE(m->cmd.out == m->buf[0]);
    TEST_ASSERT_EQUAL_UINT32(4096, m->cmd.out_cap);
    mb_complete_cmd(m);
    mb_settle(m);
    mb_assert_clean(m);

    m = mb_init(4096, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_cbw(m, 0x56, 512, false, cdb, 10);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_OUT, m->cmd.dir);
    TEST_ASSERT_EQUAL_UINT32(512, m->cmd.expected_len);
}

/* ---- 不正な CBW（BOT 6.2.1: 署名と長さ）------------------------------------ */
static void test_invalid_signature_every_byte_and_values(void) {
    for (int byte = 0; byte < 4; byte++) {
        uint8_t raw[31];
        make_valid(raw);
        raw[byte] ^= 0x01;
        expect_invalid(raw, 31, "signature byte flipped");
    }
    uint8_t raw[31];
    make_valid(raw);
    memset(raw, 0, 4);
    expect_invalid(raw, 31, "signature 0");
    make_valid(raw);
    raw[0] = 'U'; raw[1] = 'S'; raw[2] = 'B'; raw[3] = 'S'; /* CSW の署名 */
    expect_invalid(raw, 31, "CSW signature in a CBW");
    make_valid(raw);
    raw[0] = 'u'; raw[1] = 's'; raw[2] = 'b'; raw[3] = 'c';
    expect_invalid(raw, 31, "lowercase signature");
    make_valid(raw);
    raw[0] = 'C'; raw[1] = 'B'; raw[2] = 'S'; raw[3] = 'U'; /* 逆順 */
    expect_invalid(raw, 31, "byte-swapped signature");
}

static void test_invalid_length(void) {
    uint8_t raw[31];
    make_valid(raw);
    expect_invalid(raw, 30, "30 bytes");
    expect_invalid(raw, 32, "32 bytes");
    expect_invalid(raw, 0, "zero-length packet");
    expect_invalid(raw, 1, "1 byte");
    expect_invalid(raw, 13, "13 bytes (CSW length)");
    expect_invalid(raw, 64, "64 bytes");
}

static void test_invalid_transfer_error_of_the_cbw(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    mb_clear_log(m);
    uint8_t raw[31];
    make_valid(raw);
    memcpy(m->cbw, raw, 31); /* 中身は有効な CBW でも、転送が失敗なら、使わない */
    m->out_pending = false;
    msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_OUT, 31, false); /* 転送の失敗 */
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    MB_ASSERT_LOG(m, "stallI stallO");
    mb_assert_clean(m);
}

/* ---- meaningful でない CBW（BOT 6.2.2）------------------------------------- */
static void test_invalid_lun(void) {
    uint8_t raw[31];
    uint8_t luns[] = {0x01, 0x02, 0x03, 0x04, 0x08, 0x0F};
    for (size_t i = 0; i < sizeof(luns); i++) {
        make_valid(raw);
        raw[13] = luns[i];
        expect_invalid(raw, 31, "LUN out of range");
    }
    uint8_t reserved[] = {0x10, 0x80, 0xF0};
    for (size_t i = 0; i < sizeof(reserved); i++) {
        make_valid(raw);
        raw[13] = reserved[i];
        expect_invalid(raw, 31, "reserved bits of the LUN byte");
    }
    make_valid(raw);
    raw[13] = 0x00; /* LUN 0 は有効 */
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_raw_cbw(m, raw, 31);
    MB_ASSERT_STATE(m, MSC_BOT_EXEC);
}

static void test_invalid_cdb_length(void) {
    uint8_t raw[31];
    uint8_t lens[] = {0, 17, 18, 31, 0x20, 0x26 /* reserved bit + 6 */, 0xE6, 0xFF};
    for (size_t i = 0; i < sizeof(lens); i++) {
        make_valid(raw);
        raw[14] = lens[i];
        expect_invalid(raw, 31, "CDB length");
    }
}

static void test_invalid_reserved_flag_bits(void) {
    uint8_t raw[31];
    uint8_t flags[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x81, 0xC0, 0xFF, 0x7F};
    for (size_t i = 0; i < sizeof(flags); i++) {
        make_valid(raw);
        raw[12] = flags[i];
        expect_invalid(raw, 31, "reserved bits of bmCBWFlags");
    }
    /* 0x00（OUT）と 0x80（IN）だけが有効 */
    for (int f = 0; f < 2; f++) {
        make_valid(raw);
        raw[12] = f ? 0x80 : 0x00;
        mb_t *m = mb_init(2048, false);
        m->hold_cmd = true;
        mb_start(m);
        mb_send_raw_cbw(m, raw, 31);
        MB_ASSERT_STATE(m, MSC_BOT_EXEC);
    }
}

/* H = 0 でも、予約ビットは検査する（方向のビットだけが無視される。BOT 5.1）。 */
static void test_reserved_flag_bits_checked_even_when_length_zero(void) {
    uint8_t raw[31];
    make_valid(raw);
    raw[8] = raw[9] = raw[10] = raw[11] = 0;
    raw[12] = 0x01;
    expect_invalid(raw, 31, "reserved flag with H=0");
}

/* ---- 不正な CBW の後の状態（6.6.1: Reset Recovery まで保つ）---------------- */
static void test_invalid_cbw_keeps_state_until_reset_recovery(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    uint8_t raw[31];
    make_valid(raw);
    raw[0] = 'X';
    mb_send_raw_cbw(m, raw, 31);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    mb_clear_log(m);

    /* 回復の前に、ホストが CLEAR_FEATURE(HALT) を送っても、STALL を保つ（再度 STALL する）。 */
    mb_host_clear_halt(m, MSC_BOT_EP_IN);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    TEST_ASSERT_TRUE(m->hw_in_stalled);
    mb_host_clear_halt(m, MSC_BOT_EP_OUT);
    TEST_ASSERT_TRUE(m->hw_out_stalled);
    MB_ASSERT_LOG(m, "stallI stallO");
    TEST_ASSERT_FALSE(m->out_pending);
    TEST_ASSERT_TRUE(msc_bot_halted(&m->bot, MSC_BOT_EP_IN) && msc_bot_halted(&m->bot, MSC_BOT_EP_OUT));

    /* 他のイベント（完了）も、状態を変えない。 */
    msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_OUT, 31, true);
    msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_IN, 13, true);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    mb_assert_clean(m);
}

/* BOT 5.3.4: Reset、CLEAR_FEATURE(HALT) IN、同 OUT の順。OUT をクリアするまで、次の CBW を受けない。 */
static void test_reset_recovery_conforming_order(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    uint8_t raw[31];
    make_valid(raw);
    raw[0] = 'X';
    mb_send_raw_cbw(m, raw, 31);
    mb_clear_log(m);

    msc_bot_reset(&m->bot); /* (a) Bulk-Only Mass Storage Reset */
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    TEST_ASSERT_EQUAL_UINT(1, m->n_reset_done);
    TEST_ASSERT_TRUE(m->hw_in_stalled && m->hw_out_stalled); /* BOT 3.1: Reset は STALL の状態を保つ */
    TEST_ASSERT_FALSE(m->out_pending);
    mb_host_clear_halt(m, MSC_BOT_EP_IN); /* (b) */
    TEST_ASSERT_FALSE(m->hw_in_stalled);
    TEST_ASSERT_FALSE(m->out_pending); /* OUT がまだ STALL 中 */
    mb_host_clear_halt(m, MSC_BOT_EP_OUT); /* (c) */
    TEST_ASSERT_TRUE(m->out_pending && m->out_ptr == m->cbw);
    MB_ASSERT_LOG(m, "rdone out31");
    mb_assert_clean(m);

    /* 回復後は、普通に動く。 */
    mb_send_tur(m, 3);
    mb_settle(m);
    mb_assert_csw(m, 3, 0, 0);
    mb_assert_clean(m);
}

/* OUT を先にクリアしても、IN をクリアするまで受けない（両方の halt が解けてから受ける）。 */
static void test_reset_recovery_clears_in_either_order(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    uint8_t raw[31];
    make_valid(raw);
    raw[0] = 'X';
    mb_send_raw_cbw(m, raw, 31);
    msc_bot_reset(&m->bot);
    mb_host_clear_halt(m, MSC_BOT_EP_OUT);
    TEST_ASSERT_FALSE(m->out_pending);
    mb_host_clear_halt(m, MSC_BOT_EP_IN);
    TEST_ASSERT_TRUE(m->out_pending && m->out_ptr == m->cbw);
    mb_assert_clean(m);
}

/* Reset だけで、CLEAR_FEATURE が来ない: STALL は残り、CBW は受けない（BOT 3.1: Reset は STALL を解かない）。 */
static void test_reset_alone_does_not_clear_halts(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    uint8_t raw[31];
    make_valid(raw);
    raw[0] = 'X';
    mb_send_raw_cbw(m, raw, 31);
    msc_bot_reset(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    TEST_ASSERT_TRUE(m->hw_in_stalled && m->hw_out_stalled);
    TEST_ASSERT_FALSE(m->out_pending);
    mb_assert_clean(m);
}

/* Phase Error の CSW の後の Reset Recovery（STALL がない場合）: 受信は発行済みのまま、再発行しない。 */
static void test_reset_recovery_after_phase_error_without_stalls(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    mb_send_inquiry(m, 1, 0); /* ケース 2: Hn < Di -> CSW Phase Error。STALL なし */
    mb_settle(m);
    mb_assert_csw(m, 1, 2, 0);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    TEST_ASSERT_TRUE(m->out_pending);
    mb_clear_log(m);
    msc_bot_reset(&m->bot);
    mb_host_clear_halt(m, MSC_BOT_EP_IN);
    mb_host_clear_halt(m, MSC_BOT_EP_OUT);
    MB_ASSERT_LOG(m, "rdone"); /* 受信を二重に発行しない（xfer_out なし） */
    TEST_ASSERT_TRUE(m->out_pending);
    mb_send_tur(m, 2);
    mb_settle(m);
    mb_assert_csw(m, 2, 0, 0);
    mb_assert_clean(m);
}

/* STALL した Bulk-IN が、ホストのクリアの前に Reset を受けたら: CSW は送らず、IDLE。クリアされてから CBW を受ける。 */
static void test_reset_while_waiting_for_in_unstall(void) {
    mb_t *m = mb_init(2048, false);
    m->auto_clear = false;
    mb_start(m);
    const uint8_t cdb[6] = {0};
    mb_send_tur(m, 1);
    mb_settle(m);
    /* TUR with H = 36 IN -> case 4: STALL IN、クリア待ち */
    mb_send_cbw(m, 2, 36, true, cdb, 6);
    mb_settle(m);
    MB_ASSERT_STATE(m, MSC_BOT_STATUS_WAIT_UNSTALL);
    TEST_ASSERT_TRUE(m->hw_in_stalled);
    mb_clear_log(m);
    msc_bot_reset(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    TEST_ASSERT_FALSE(m->in_pending); /* CSW は送らない */
    TEST_ASSERT_FALSE(m->out_pending); /* IN が STALL 中 */
    mb_host_clear_halt(m, MSC_BOT_EP_IN);
    TEST_ASSERT_TRUE(m->out_pending);
    MB_ASSERT_LOG(m, "rdone out31");
    mb_assert_clean(m);
}

/* 不正な CBW を続けて受けても（回復の前に OUT のデータが来ても）、command_done は呼ばれず、数が合う。 */
static void test_invalid_cbw_between_valid_commands_keeps_counts(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    mb_send_tur(m, 1);
    mb_settle(m);
    uint8_t raw[31];
    make_valid(raw);
    raw[3] = 'Z';
    mb_send_raw_cbw(m, raw, 31);
    msc_bot_reset(&m->bot);
    mb_host_clear_halt(m, MSC_BOT_EP_IN);
    mb_host_clear_halt(m, MSC_BOT_EP_OUT);
    mb_send_tur(m, 3);
    mb_settle(m);
    TEST_ASSERT_EQUAL_UINT(2, m->n_exec);
    TEST_ASSERT_EQUAL_UINT(2, m->n_done);
    TEST_ASSERT_EQUAL_UINT32(1, msc_bot_stats(&m->bot).invalid_cbw);
    mb_assert_clean(m);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_cbw_all_cdb_lengths_pass_the_cdb_through);
    RUN_TEST(test_valid_cbw_with_direction_bit_and_zero_length);
    RUN_TEST(test_execute_command_fields);
    RUN_TEST(test_invalid_signature_every_byte_and_values);
    RUN_TEST(test_invalid_length);
    RUN_TEST(test_invalid_transfer_error_of_the_cbw);
    RUN_TEST(test_invalid_lun);
    RUN_TEST(test_invalid_cdb_length);
    RUN_TEST(test_invalid_reserved_flag_bits);
    RUN_TEST(test_reserved_flag_bits_checked_even_when_length_zero);
    RUN_TEST(test_invalid_cbw_keeps_state_until_reset_recovery);
    RUN_TEST(test_reset_recovery_conforming_order);
    RUN_TEST(test_reset_recovery_clears_in_either_order);
    RUN_TEST(test_reset_alone_does_not_clear_halts);
    RUN_TEST(test_reset_recovery_after_phase_error_without_stalls);
    RUN_TEST(test_reset_while_waiting_for_in_unstall);
    RUN_TEST(test_invalid_cbw_between_valid_commands_keeps_counts);
    return UNITY_END();
}

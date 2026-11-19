/*
 * msc_bot: データの段階。
 *   - データ IN: ダブルバッファの分割転送（READ_BLOCKS と xfer_in の重なり、バッファの交互、転送の大きさの上限 = バッファ）
 *   - IN の途中の失敗（MEDIUM ERROR 相当 -> CSW Failed、STALL、residue = H - 送った量。command_done は失敗の直後に 1 回）
 *   - データ OUT の読み捨て（BOT 6.7.3 ケース 9..13 の「H バイトを受け取る」）、短い OUT、OUT の転送エラー
 *   - 内部エラー（注入した関数の拒否、契約違反の結果）-> 6.6.2 の「STALL し、Reset Recovery まで保つ」
 *   - 契約外のイベント（対応する依頼がない完了、seq の違う完了）は捨てる
 */
#include "mb_common.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- ダブルバッファ ------------------------------------------------------- */
/* READ(10) 4 ブロック、バッファ 2048。USB の転送が先に終わる順序で、全部を手で進める。 */
static void test_double_buffer_in_first_order(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_read10(m, 0x21, 40, 4, 8192);
    MB_ASSERT_STATE(m, MSC_BOT_EXEC);
    TEST_ASSERT_TRUE(m->cmd_pending);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_CMD_EXECUTE, m->cmd.kind);
    TEST_ASSERT_EQUAL_UINT32(8192, m->cmd.expected_len);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DIR_IN, m->cmd.dir);

    mb_complete_cmd(m); /* EXECUTE の完了 -> 1 つ目の READ_BLOCKS（buf[0]） */
    MB_ASSERT_STATE(m, MSC_BOT_DATA_IN);
    TEST_ASSERT_TRUE(m->cmd_pending);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_CMD_READ_BLOCKS, m->cmd.kind);
    TEST_ASSERT_TRUE(m->cmd.buf == m->buf[0]);
    TEST_ASSERT_EQUAL_UINT32(1, m->cmd.blocks);
    TEST_ASSERT_EQUAL_UINT64(40, m->cmd.lba);
    TEST_ASSERT_FALSE(m->in_pending);

    mb_complete_cmd(m); /* buf[0] が埋まる -> buf[0] を送りながら、buf[1] を読む（重なり） */
    TEST_ASSERT_TRUE(m->in_pending);
    TEST_ASSERT_TRUE(m->in_ptr == m->buf[0]);
    TEST_ASSERT_EQUAL_UINT32(2048, m->in_len);
    TEST_ASSERT_TRUE(m->cmd_pending);
    TEST_ASSERT_TRUE(m->cmd.buf == m->buf[1]);
    TEST_ASSERT_EQUAL_UINT64(41, m->cmd.lba);

    mb_deliver_in(m, true); /* buf[0] を送り終わった。buf[1] はまだ読み出し中 */
    TEST_ASSERT_FALSE(m->in_pending);
    TEST_ASSERT_TRUE(m->cmd_pending);

    mb_complete_cmd(m); /* buf[1] が埋まる -> buf[1] を送り、buf[0] を読む */
    TEST_ASSERT_TRUE(m->in_pending);
    TEST_ASSERT_TRUE(m->in_ptr == m->buf[1]);
    TEST_ASSERT_TRUE(m->cmd_pending);
    TEST_ASSERT_TRUE(m->cmd.buf == m->buf[0]);
    TEST_ASSERT_EQUAL_UINT64(42, m->cmd.lba);

    mb_deliver_in(m, true);
    mb_complete_cmd(m); /* 3 つ目 */
    TEST_ASSERT_TRUE(m->in_ptr == m->buf[0]);
    TEST_ASSERT_TRUE(m->cmd.buf == m->buf[1]);
    TEST_ASSERT_EQUAL_UINT64(43, m->cmd.lba);
    mb_deliver_in(m, true);
    TEST_ASSERT_EQUAL_UINT(0, m->n_done); /* 最後の READ がまだ実行中 */

    mb_complete_cmd(m); /* 4 つ目（最後）が埋まる -> 送る。以後 mmc_scsi は呼ばないので command_done */
    TEST_ASSERT_TRUE(m->in_ptr == m->buf[1]);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    TEST_ASSERT_TRUE(m->in_pending);
    MB_ASSERT_STATE(m, MSC_BOT_DATA_IN);
    mb_deliver_in(m, true);
    MB_ASSERT_STATE(m, MSC_BOT_STATUS_SEND);
    mb_deliver_in(m, true); /* CSW */
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    mb_assert_clean(m);
    mb_assert_csw(m, 0x21, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(8192, m->rx_len);
    mb_assert_rx_pattern(m, 40, 0, 8192);
    MB_ASSERT_LOG(m, "exec read1 read1 in2048 read1 in2048 read1 in2048 in2048 done in13 out31");
}

/* 読み出しが USB の転送より先に終わる順序（バッファが READY のまま、USB の完了を待つ）。 */
static void test_double_buffer_read_first_order(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_read10(m, 0x22, 0, 4, 8192);
    mb_complete_cmd(m);
    mb_complete_cmd(m); /* buf[0] 完了 -> in(buf[0]) + read buf[1] */
    TEST_ASSERT_TRUE(m->in_pending && m->in_ptr == m->buf[0]);
    mb_complete_cmd(m); /* buf[1] が先に埋まる。USB はまだ buf[0] を送っている -> 何も発行されない */
    TEST_ASSERT_FALSE(m->cmd_pending);
    TEST_ASSERT_TRUE(m->in_pending && m->in_ptr == m->buf[0]);
    MB_ASSERT_LOG(m, "exec read1 read1 in2048");
    mb_deliver_in(m, true); /* buf[0] が空く -> 次の読み出しは buf[0]、送るのは buf[1] */
    TEST_ASSERT_TRUE(m->in_pending && m->in_ptr == m->buf[1]);
    TEST_ASSERT_TRUE(m->cmd_pending && m->cmd.buf == m->buf[0]);
    TEST_ASSERT_EQUAL_UINT64(2, m->cmd.lba);
    MB_ASSERT_LOG(m, "exec read1 read1 in2048 read1 in2048");
    m->hold_cmd = false;
    mb_settle(m);
    mb_assert_clean(m);
    mb_assert_csw(m, 0x22, 0, 0);
    mb_assert_rx_pattern(m, 0, 0, 8192);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
}

/* 1 回の転送の大きさは、バッファの大きさ（2048 の倍数）が上限。最後の転送は端数。 */
static void test_transfer_sizes_are_bounded_by_buffer(void) {
    mb_t *m = mb_init(4096, false);
    mb_start(m);
    mb_send_read10(m, 1, 0, 5, 10240); /* 5 ブロック = 10240 */
    mb_settle(m);
    mb_assert_clean(m);
    TEST_ASSERT_EQUAL_UINT32(3, m->in_xfers);
    TEST_ASSERT_EQUAL_UINT32(4096, m->in_xfer_sizes[0]);
    TEST_ASSERT_EQUAL_UINT32(4096, m->in_xfer_sizes[1]);
    TEST_ASSERT_EQUAL_UINT32(2048, m->in_xfer_sizes[2]);
    mb_assert_rx_pattern(m, 0, 0, 10240);
    mb_assert_csw(m, 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT(3, m->n_read);
}

/* H が 2048 の倍数でない（case 7）。最後の転送は端数のバイト数で、読むブロック数は切り上げ。 */
static void test_tail_not_multiple_of_block(void) {
    mb_t *m = mb_init(4096, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_read10(m, 1, 3, 4, 5000);
    mb_complete_cmd(m); /* EXECUTE */
    mb_complete_cmd(m); /* 1 つ目（4096） -> in + 2 つ目の読み出し */
    mb_complete_cmd(m); /* 2 つ目（端数 904）。これで、以後 mmc_scsi を呼ばない */
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);   /* 送信がまだ終わっていなくても、command_done は済み */
    TEST_ASSERT_TRUE(m->in_pending);
    m->hold_cmd = false;
    mb_settle(m);
    mb_assert_clean(m);
    TEST_ASSERT_EQUAL_UINT32(2, m->in_xfers);
    TEST_ASSERT_EQUAL_UINT32(4096, m->in_xfer_sizes[0]);
    TEST_ASSERT_EQUAL_UINT32(904, m->in_xfer_sizes[1]);
    mb_assert_rx_pattern(m, 3, 0, 5000);
    mb_assert_csw(m, 1, 2, 0);
    TEST_ASSERT_EQUAL_UINT(2, m->n_read);
}

/* 大きな転送（1.2 MB、バッファ 8192）。全バイトを検査。同期と遅延の両方。 */
static void test_large_read_sync_and_delayed(void) {
    for (int sync = 0; sync < 2; sync++) {
        mb_t *m = mb_init(8192, false);
        m->sync = sync != 0;
        mb_start(m);
        mb_send_read12(m, 0x77, 1000, 600, 600u * 2048u);
        mb_settle(m);
        mb_assert_clean(m);
        TEST_ASSERT_EQUAL_UINT32(600u * 2048u, m->rx_len);
        mb_assert_rx_pattern(m, 1000, 0, 600u * 2048u);
        mb_assert_csw(m, 0x77, 0, 0);
        TEST_ASSERT_EQUAL_UINT(1, m->n_done);
        TEST_ASSERT_EQUAL_UINT(150, m->n_read);
        TEST_ASSERT_EQUAL_UINT32(150, m->in_xfers);
        for (uint32_t i = 0; i < 150; i++) TEST_ASSERT_EQUAL_UINT32(8192, m->in_xfer_sizes[i]);
    }
}

/* READ 以外の IN のデータは、EXECUTE の out（buf[0]）から、1 回の転送で送る。 */
static void test_non_read_in_data_comes_from_out_buffer(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_inquiry(m, 9, 36);
    mb_complete_cmd(m);
    TEST_ASSERT_TRUE(m->in_pending && m->in_ptr == m->buf[0]);
    TEST_ASSERT_EQUAL_UINT32(36, m->in_len);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done); /* 読み出しがないので、EXECUTE の直後に command_done */
    mb_deliver_in(m, true);
    mb_deliver_in(m, true);
    mb_assert_clean(m);
    mb_assert_csw(m, 9, 0, 0);
}

/* ---- IN の途中の失敗 ------------------------------------------------------ */
/* 3 つ目の READ_BLOCKS が失敗。送った 4096 バイトまでが有効。CSW Failed、residue = 8192 - 4096、STALL。 */
static void test_in_mid_failure_third_read(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    m->read_fail_nth = 2;
    mb_start(m);
    mb_clear_log(m);
    mb_send_read10(m, 0x31, 0, 4, 8192);
    mb_complete_cmd(m);
    mb_complete_cmd(m); /* r1 -> in(b0) + r2 */
    mb_deliver_in(m, true);
    mb_complete_cmd(m); /* r2 -> in(b1) + r3 */
    TEST_ASSERT_TRUE(m->in_pending && m->in_ptr == m->buf[1]);
    mb_complete_cmd(m); /* r3 が失敗（MEDIUM ERROR） */
    TEST_ASSERT_EQUAL_UINT(1, m->n_done); /* 失敗の完了の直後に command_done */
    MB_ASSERT_STATE(m, MSC_BOT_DATA_IN);  /* 転送中の in(b1) を待つ */
    TEST_ASSERT_FALSE(m->hw_in_stalled);
    mb_deliver_in(m, true);
    MB_ASSERT_STATE(m, MSC_BOT_STATUS_WAIT_UNSTALL);
    TEST_ASSERT_TRUE(m->hw_in_stalled);
    mb_host_clear_halt(m, MSC_BOT_EP_IN);
    mb_deliver_in(m, true); /* CSW */
    mb_assert_clean(m);
    mb_assert_csw(m, 0x31, 1, 4096);
    TEST_ASSERT_EQUAL_UINT32(4096, m->rx_len);
    mb_assert_rx_pattern(m, 0, 0, 4096);
    MB_ASSERT_LOG(m, "exec read1 read1 in2048 read1 in2048 done stallI in13 out31");
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
}

/* 最初の READ_BLOCKS が失敗: データなし。CSW Failed、residue = H、STALL。 */
static void test_in_mid_failure_first_read(void) {
    mb_t *m = mb_init(2048, false);
    m->read_fail_nth = 0;
    mb_start(m);
    mb_clear_log(m);
    mb_send_read10(m, 0x32, 0, 4, 8192);
    mb_settle(m);
    mb_assert_clean(m);
    mb_assert_csw(m, 0x32, 1, 8192);
    TEST_ASSERT_EQUAL_UINT32(0, m->rx_len);
    MB_ASSERT_LOG(m, "exec read1 done stallI in13 out31");
}

/* 2 つ目が失敗したとき、1 つ目の転送が、まだ USB で進行中。それが終わってから STALL する。 */
static void test_in_failure_waits_for_inflight_transfer(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    m->read_fail_nth = 1;
    mb_start(m);
    mb_send_read10(m, 0x33, 0, 4, 8192);
    mb_complete_cmd(m);
    mb_complete_cmd(m); /* r1 -> in(b0) + r2 */
    TEST_ASSERT_TRUE(m->in_pending);
    mb_complete_cmd(m); /* r2 失敗。in(b0) はまだ USB で進行中 */
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    MB_ASSERT_STATE(m, MSC_BOT_DATA_IN);
    TEST_ASSERT_TRUE(m->in_pending);
    TEST_ASSERT_FALSE(m->hw_in_stalled); /* 転送中に STALL しない */
    mb_deliver_in(m, true);
    MB_ASSERT_STATE(m, MSC_BOT_STATUS_WAIT_UNSTALL);
    mb_settle(m);
    mb_assert_clean(m);
    mb_assert_csw(m, 0x33, 1, 8192 - 2048);
    TEST_ASSERT_EQUAL_UINT32(2048, m->rx_len);
}

/* ケース 7 の READ が途中で失敗しても、Phase Error（2）のまま（BOT 6.7.2: ケース 7 は status 2）。 */
static void test_in_failure_in_case7_keeps_phase_error(void) {
    mb_t *m = mb_init(2048, false);
    m->read_fail_nth = 1;
    mb_start(m);
    mb_send_read10(m, 0x34, 0, 8, 4096); /* デバイス 16384、ホスト 4096 */
    mb_settle(m);
    mb_assert_clean(m);
    mb_assert_csw(m, 0x34, 2, 4096 - 2048);
    TEST_ASSERT_EQUAL_UINT32(2048, m->rx_len);
}

/* IN の転送自体の失敗（ホストが読まない、取り消し等）は、内部エラー -> RECOVERY。command_done は 1 回。 */
static void test_in_transfer_error_goes_to_recovery(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_read10(m, 0x35, 0, 4, 8192);
    mb_complete_cmd(m);
    mb_complete_cmd(m); /* in(b0) + r2 */
    mb_deliver_in(m, false);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    TEST_ASSERT_TRUE(m->hw_in_stalled && m->hw_out_stalled);
    TEST_ASSERT_EQUAL_UINT(0, m->n_done); /* r2 がまだ実行中。完了を待つ */
    mb_complete_cmd(m);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    mb_assert_clean(m);
}

static void test_in_short_completion_is_error(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_inquiry(m, 1, 36);
    mb_complete_cmd(m);
    TEST_ASSERT_TRUE(m->in_pending);
    m->in_pending = false;
    msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_IN, 35, true); /* 36 を要求したのに 35 */
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    mb_assert_clean(m);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
}

/* ---- データ OUT ----------------------------------------------------------- */
static void test_out_discard_chunks_bounded_by_buffer(void) {
    mb_t *m = mb_init(4096, false);
    mb_start(m);
    mb_clear_log(m);
    m->host_out_left = 10240;
    mb_send_write10(m, 0x41, 10240);
    mb_settle(m);
    mb_assert_clean(m);
    MB_ASSERT_LOG(m, "exec done out4096 out4096 out2048 in13 out31");
    mb_assert_csw(m, 0x41, 1, 10240); /* WRITE は DATA PROTECT（失敗）。処理した量は 0 */
    TEST_ASSERT_EQUAL_UINT32(0, m->host_out_left);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
}

/* ケース 12 で、ホストが短いパケットで打ち切った（100 バイトだけ送った）。受け取った分が処理した量の上限。 */
static void test_out_short_transfer_ends_data_phase(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    const uint8_t cdb[6] = {0xF0, 2, 0, 1, 0, 0}; /* デバイスは 512 バイトを受け取る予定 */
    mb_send_cbw(m, 0x42, 512, false, cdb, 6);
    mb_complete_cmd(m);
    TEST_ASSERT_TRUE(m->out_pending);
    TEST_ASSERT_EQUAL_UINT32(512, m->out_len);
    mb_deliver_out(m, 100, true); /* 短い */
    mb_deliver_in(m, true);
    mb_assert_clean(m);
    mb_assert_csw(m, 0x42, 0, 512 - 100);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
}

static void test_out_transfer_error_goes_to_recovery(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    m->host_out_left = 0;
    mb_send_write10(m, 0x43, 512);
    mb_settle(m);
    TEST_ASSERT_TRUE(m->out_pending);
    mb_deliver_out(m, 0, false);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    TEST_ASSERT_TRUE(m->hw_in_stalled && m->hw_out_stalled);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    mb_assert_clean(m);
}

static void test_out_oversized_completion_is_error(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    mb_send_write10(m, 0x44, 512);
    mb_settle(m);
    TEST_ASSERT_TRUE(m->out_pending);
    mb_deliver_out(m, 513, true); /* 512 を要求したのに 513 */
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    mb_assert_clean(m);
}

/* out_stall: ケース 9 でデータを受け取らず、Bulk-OUT を STALL。CSW は先に届く。ホストが OUT をクリアするまで、次の CBW を受けない。 */
static void test_out_stall_policy_waits_for_clear_before_next_cbw(void) {
    mb_t *m = mb_init(2048, true);
    mb_start(m);
    mb_clear_log(m);
    mb_send_write10(m, 0x45, 512);
    mb_settle(m);
    mb_assert_clean(m);
    MB_ASSERT_LOG(m, "exec done stallO in13");
    mb_assert_csw(m, 0x45, 1, 512);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    TEST_ASSERT_FALSE(m->out_pending); /* OUT が STALL 中は、CBW の受信を発行しない */
    TEST_ASSERT_TRUE(msc_bot_halted(&m->bot, MSC_BOT_EP_OUT));
    mb_host_clear_halt(m, MSC_BOT_EP_OUT);
    TEST_ASSERT_TRUE(m->out_pending && m->out_ptr == m->cbw);
    TEST_ASSERT_FALSE(msc_bot_halted(&m->bot, MSC_BOT_EP_OUT));
    mb_assert_clean(m);
}

/* ---- 内部エラー（注入した関数の拒否、契約違反）--------------------------- */
static void test_xfer_out_refused_at_attach_goes_to_recovery(void) {
    mb_t *m = mb_init(2048, false);
    m->refuse_out = true;
    msc_bot_attach(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    TEST_ASSERT_TRUE(m->hw_in_stalled && m->hw_out_stalled);
    mb_assert_clean(m);
}

static void test_submit_refused_for_execute(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    m->refuse_submit = true;
    mb_send_tur(m, 1);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    TEST_ASSERT_EQUAL_UINT(0, m->n_exec);
    TEST_ASSERT_EQUAL_UINT(0, m->n_done); /* 実行を依頼していないので、command_done も呼ばない */
    TEST_ASSERT_EQUAL_UINT32(0, msc_bot_stats(&m->bot).exec_submitted);
    TEST_ASSERT_FALSE(msc_bot_cmd_live(&m->bot));
    TEST_ASSERT_FALSE(msc_bot_cmd_outstanding(&m->bot));
    mb_assert_clean(m);
}

static void test_submit_refused_for_read_blocks(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_read10(m, 1, 0, 2, 4096);
    m->refuse_submit = true; /* EXECUTE の完了の後、1 つ目の READ_BLOCKS を断る */
    mb_complete_cmd(m);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    TEST_ASSERT_EQUAL_UINT(1, m->n_exec);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done); /* 実行中の依頼がないので、すぐ command_done */
    TEST_ASSERT_TRUE(m->hw_in_stalled && m->hw_out_stalled);
    mb_assert_clean(m);
}

static void test_xfer_in_refused_for_data_waits_for_outstanding_read(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_read10(m, 1, 0, 4, 8192);
    mb_complete_cmd(m);
    m->refuse_in = true;
    mb_complete_cmd(m); /* r1 -> r2 を依頼 -> in(b0) を断られる */
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    TEST_ASSERT_EQUAL_UINT(0, m->n_done); /* r2 が実行中: 完了を待つ */
    TEST_ASSERT_TRUE(m->cmd_pending);
    mb_complete_cmd(m);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    mb_assert_clean(m);
}

/* READ_BLOCKS の完了は、EXECUTE の seq では受け付けない（seq は依頼ごとに進む）。 */
static void test_read_blocks_has_its_own_seq(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_read10(m, 1, 0, 2, 4096);
    uint32_t exec_seq = m->cmd.seq;
    mb_complete_cmd(m);
    TEST_ASSERT_TRUE(m->cmd_pending && m->cmd.kind == MSC_BOT_CMD_READ_BLOCKS);
    TEST_ASSERT_TRUE(m->cmd.seq != exec_seq);
    msc_bot_cmd_result_t r;
    memset(&r, 0, sizeof(r));
    r.seq = exec_seq; /* 古い（EXECUTE の）完了が、遅れて届く */
    r.good = true;
    msc_bot_command_complete(&m->bot, &r);
    TEST_ASSERT_EQUAL_UINT32(1, msc_bot_stats(&m->bot).stale_events);
    TEST_ASSERT_FALSE(m->in_pending); /* READ はまだ完了していない */
    m->hold_cmd = false;
    mb_settle(m);
    mb_assert_csw(m, 1, 0, 0);
    mb_assert_rx_pattern(m, 0, 0, 4096);
    mb_assert_clean(m);
}

/* CSW の転送が短く完了（12 バイト）したら、内部エラー。 */
static void test_short_csw_completion_is_error(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_tur(m, 1);
    mb_complete_cmd(m);
    TEST_ASSERT_TRUE(m->in_pending && m->in_ptr == m->csw);
    m->in_pending = false;
    msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_IN, 12, true);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    mb_assert_clean(m);
}

/* RECOVERY に入るとき、すでに STALL してある Bulk は、もう一度 STALL しない（out_stall の方針で Bulk-OUT が STALL 済み）。 */
static void test_recovery_does_not_stall_an_already_halted_pipe(void) {
    mb_t *m = mb_init(2048, true);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_write10(m, 1, 512);
    mb_complete_cmd(m); /* ケース 9、out_stall: Bulk-OUT を STALL して、CSW を発行 */
    MB_ASSERT_STATE(m, MSC_BOT_STATUS_SEND);
    TEST_ASSERT_TRUE(m->hw_out_stalled);
    mb_clear_log(m);
    mb_deliver_in(m, false); /* CSW の転送が失敗 */
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    MB_ASSERT_LOG(m, "stallI"); /* Bulk-OUT は STALL 済みなので、IN だけ */
    TEST_ASSERT_TRUE(m->hw_in_stalled && m->hw_out_stalled);
    mb_assert_clean(m);
}

static void test_xfer_in_refused_for_csw(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    m->refuse_in = true;
    mb_send_tur(m, 1);
    mb_settle(m);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    TEST_ASSERT_TRUE(m->hw_in_stalled && m->hw_out_stalled);
    mb_assert_clean(m);
}

static void test_xfer_out_refused_for_data_out(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    mb_send_write10(m, 1, 512);
    m->refuse_out = true;
    mb_settle(m);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    mb_assert_clean(m);
}

static void test_xfer_out_refused_for_next_cbw_after_csw(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_tur(m, 1);
    mb_complete_cmd(m);
    m->refuse_out = true; /* CSW の完了の後の、次の CBW の受信を断る */
    mb_deliver_in(m, true);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    mb_assert_clean(m);
}

/* READ 以外の結果の data_len が、バッファより大きい（契約違反）-> RECOVERY。command_done は 1 回。 */
static void test_result_larger_than_out_buffer_is_internal_error(void) {
    msc_bot_cmd_result_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.good = true;
    sc.dev_dir = MSC_BOT_DIR_IN;
    sc.data_len = 2049;
    mb_t *m = mb_init(2048, false);
    m->script_on = true;
    m->script = sc;
    mb_start(m);
    mb_send_inquiry(m, 1, 4096);
    mb_settle(m);
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    TEST_ASSERT_TRUE(m->hw_in_stalled && m->hw_out_stalled);
    mb_assert_clean(m);

    /* ちょうどバッファの大きさ（2048）は、契約の範囲内 */
    sc.data_len = 2048;
    m = mb_init(2048, false);
    m->script_on = true;
    m->script = sc;
    mb_start(m);
    mb_send_inquiry(m, 1, 2048);
    mb_settle(m);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    mb_assert_csw(m, 1, 0, 0);
    mb_assert_clean(m);
}

/* OUT のデータを受け取る予定の長さ（デバイスの結果）は、バッファの大きさに制限されない。契約の制限は、READ 以外の「IN」だけ。 */
static void test_device_out_length_larger_than_buffer_is_not_an_error(void) {
    msc_bot_cmd_result_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.good = true;
    sc.dev_dir = MSC_BOT_DIR_OUT;
    sc.data_len = 100000;
    mb_t *m = mb_init(2048, false);
    m->script_on = true;
    m->script = sc;
    m->host_out_left = 512;
    mb_start(m);
    const uint8_t cdb[6] = {0xF0};
    mb_send_cbw(m, 1, 512, false, cdb, 6); /* ケース 13: Ho < Do */
    mb_settle(m);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    mb_assert_csw(m, 1, 2, 0);
    mb_assert_clean(m);
    /* READ の転送長（READ(12) の blocks x 2048）も、バッファの大きさに制限されない（分割して送る）。 */
    m = mb_init(2048, false);
    mb_start(m);
    mb_send_read12(m, 2, 0, 8, 16384);
    mb_settle(m);
    mb_assert_csw(m, 2, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(16384, m->rx_len);
    mb_assert_clean(m);
}

/* ---- 契約外のイベント ----------------------------------------------------- */
static void test_stale_events_are_dropped(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    msc_bot_stats_t s0 = msc_bot_stats(&m->bot);
    msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_IN, 13, true); /* IN の依頼がない */
    TEST_ASSERT_EQUAL_UINT32(s0.stale_events + 1, msc_bot_stats(&m->bot).stale_events);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    msc_bot_cmd_result_t r;
    memset(&r, 0, sizeof(r));
    r.seq = 1;
    r.good = true;
    msc_bot_command_complete(&m->bot, &r); /* 依頼がない */
    TEST_ASSERT_EQUAL_UINT32(s0.stale_events + 2, msc_bot_stats(&m->bot).stale_events);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    mb_assert_clean(m);
}

static void test_completion_with_wrong_seq_is_dropped(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_tur(m, 1);
    MB_ASSERT_STATE(m, MSC_BOT_EXEC);
    msc_bot_cmd_result_t r;
    memset(&r, 0, sizeof(r));
    r.seq = m->cmd.seq + 1;
    r.good = true;
    msc_bot_command_complete(&m->bot, &r);
    MB_ASSERT_STATE(m, MSC_BOT_EXEC);
    TEST_ASSERT_TRUE(msc_bot_cmd_outstanding(&m->bot));
    TEST_ASSERT_EQUAL_UINT32(1, msc_bot_stats(&m->bot).stale_events);
    mb_complete_cmd(m); /* 正しい seq は有効 */
    mb_settle(m);
    mb_assert_csw(m, 1, 0, 0);
    mb_assert_clean(m);
}

static void test_cbw_completion_while_not_armed_is_dropped(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    mb_send_tur(m, 1); /* EXEC 中。Bulk-OUT の受信は発行していない */
    msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_OUT, 31, true);
    TEST_ASSERT_EQUAL_UINT32(1, msc_bot_stats(&m->bot).stale_events);
    mb_settle(m);
    mb_assert_csw(m, 1, 0, 0);
    mb_assert_clean(m);
}

/* CSW の tag と、続けて流した複数のコマンド */
static void test_back_to_back_commands_echo_tags(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    uint32_t tags[4] = {0x00000001u, 0xFFFFFFFFu, 0x80000000u, 0x12345678u};
    for (int i = 0; i < 4; i++) {
        mb_send_tur(m, tags[i]);
        mb_settle(m);
        mb_assert_csw(m, tags[i], 0, 0);
        MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    }
    mb_send_inquiry(m, 0xAAAA5555u, 36);
    mb_settle(m);
    mb_assert_csw(m, 0xAAAA5555u, 0, 0);
    TEST_ASSERT_EQUAL_UINT(5, m->n_exec);
    TEST_ASSERT_EQUAL_UINT(5, m->n_done);
    mb_assert_clean(m);
}

/* H = 0xFFFFFFFF（Dn）: 32 ビットの境界。residue = H。 */
static void test_host_length_max_u32(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    mb_send_cbw(m, 5, 0xFFFFFFFFu, true, (const uint8_t[]){0x00, 0, 0, 0, 0, 0}, 6);
    mb_settle(m);
    mb_assert_csw(m, 5, 0, 0xFFFFFFFFu);
    mb_assert_clean(m);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_double_buffer_in_first_order);
    RUN_TEST(test_double_buffer_read_first_order);
    RUN_TEST(test_transfer_sizes_are_bounded_by_buffer);
    RUN_TEST(test_tail_not_multiple_of_block);
    RUN_TEST(test_large_read_sync_and_delayed);
    RUN_TEST(test_non_read_in_data_comes_from_out_buffer);
    RUN_TEST(test_in_mid_failure_third_read);
    RUN_TEST(test_in_mid_failure_first_read);
    RUN_TEST(test_in_failure_waits_for_inflight_transfer);
    RUN_TEST(test_in_failure_in_case7_keeps_phase_error);
    RUN_TEST(test_in_transfer_error_goes_to_recovery);
    RUN_TEST(test_in_short_completion_is_error);
    RUN_TEST(test_out_discard_chunks_bounded_by_buffer);
    RUN_TEST(test_out_short_transfer_ends_data_phase);
    RUN_TEST(test_out_transfer_error_goes_to_recovery);
    RUN_TEST(test_out_oversized_completion_is_error);
    RUN_TEST(test_out_stall_policy_waits_for_clear_before_next_cbw);
    RUN_TEST(test_xfer_out_refused_at_attach_goes_to_recovery);
    RUN_TEST(test_submit_refused_for_execute);
    RUN_TEST(test_submit_refused_for_read_blocks);
    RUN_TEST(test_xfer_in_refused_for_data_waits_for_outstanding_read);
    RUN_TEST(test_read_blocks_has_its_own_seq);
    RUN_TEST(test_short_csw_completion_is_error);
    RUN_TEST(test_recovery_does_not_stall_an_already_halted_pipe);
    RUN_TEST(test_xfer_in_refused_for_csw);
    RUN_TEST(test_xfer_out_refused_for_data_out);
    RUN_TEST(test_xfer_out_refused_for_next_cbw_after_csw);
    RUN_TEST(test_result_larger_than_out_buffer_is_internal_error);
    RUN_TEST(test_device_out_length_larger_than_buffer_is_not_an_error);
    RUN_TEST(test_stale_events_are_dropped);
    RUN_TEST(test_completion_with_wrong_seq_is_dropped);
    RUN_TEST(test_cbw_completion_while_not_armed_is_dropped);
    RUN_TEST(test_back_to_back_commands_echo_tags);
    RUN_TEST(test_host_length_max_u32);
    return UNITY_END();
}

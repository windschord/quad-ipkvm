/*
 * msc_bot: Reset / 切断 / 接続、実行中のコマンドとの関係、command_done の回数、再入。
 * 出典: ISO_MOUNT_DESIGN.md §4.1 と §5.1「msc_bot のスレッドモデル」、INTERFACES.md §7.3、BOT 1.0 3.1（Reset）と 5.3.4。
 *  - 実行中のコマンド（iso_msc_task）があるときの Reset / 切断は、中止の印を立て、完了イベントを待ってから command_done。
 *  - command_done は、EXECUTE を依頼したコマンドごとに、どの終わり方でも、ちょうど 1 回。
 *  - ops の中からイベントを入れても（再入）、再帰せず、順に処理される。
 */
#include "mb_common.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- Reset と実行中のコマンド --------------------------------------------- */
static void test_reset_while_execute_outstanding_waits_then_done_once(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_tur(m, 1);
    msc_bot_reset(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_RESET_WAIT);
    TEST_ASSERT_EQUAL_UINT(0, m->n_done); /* iso_msc_task が実行中: 完了を待つ */
    TEST_ASSERT_EQUAL_UINT(0, m->n_reset_done);
    TEST_ASSERT_TRUE(msc_bot_cmd_live(&m->bot) && msc_bot_cmd_outstanding(&m->bot));
    TEST_ASSERT_FALSE(m->out_pending); /* 完了までは、次の CBW を受けない */
    mb_complete_cmd(m);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    TEST_ASSERT_EQUAL_UINT(1, m->n_reset_done);
    TEST_ASSERT_TRUE(m->out_pending);
    MB_ASSERT_LOG(m, "exec done out31 rdone");
    TEST_ASSERT_FALSE(msc_bot_cmd_live(&m->bot));
    mb_assert_clean(m);
}

static void test_reset_during_read_with_transfer_in_flight(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_read10(m, 2, 0, 4, 8192);
    mb_complete_cmd(m);
    mb_complete_cmd(m); /* in(b0) + 2 つ目の読み出し */
    TEST_ASSERT_TRUE(m->in_pending && m->cmd_pending);
    msc_bot_reset(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_RESET_WAIT);
    TEST_ASSERT_FALSE(m->in_pending); /* 転送は取り消された */
    TEST_ASSERT_EQUAL_UINT(1, m->n_cancel);
    TEST_ASSERT_EQUAL_UINT(0, m->n_done);
    mb_complete_cmd(m); /* 実行中だった読み出しの完了（中止の印: 結果は捨てる） */
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    TEST_ASSERT_EQUAL_UINT(2, m->n_read); /* 完了の後に、続きの読み出しを依頼しない */
    MB_ASSERT_LOG(m, "exec read1 read1 in2048 cancel done out31 rdone");
    mb_assert_clean(m);

    /* 復帰の後、普通に動く。取り消した in(b0) は rx に入っていない。 */
    TEST_ASSERT_EQUAL_UINT32(0, m->rx_len);
    m->hold_cmd = false;
    mb_send_read10(m, 3, 10, 2, 4096);
    mb_settle(m);
    mb_assert_csw(m, 3, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(4096, m->rx_len);
    mb_assert_rx_pattern(m, 10, 0, 4096);
    TEST_ASSERT_EQUAL_UINT(2, m->n_done);
    mb_assert_clean(m);
}

static void test_reset_during_first_read_without_transfer(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_read10(m, 2, 0, 4, 8192);
    mb_complete_cmd(m);
    msc_bot_reset(&m->bot); /* 1 つ目の読み出しが実行中。USB の転送はない */
    MB_ASSERT_STATE(m, MSC_BOT_RESET_WAIT);
    TEST_ASSERT_EQUAL_UINT(0, m->n_cancel);
    mb_complete_cmd(m);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    MB_ASSERT_LOG(m, "exec read1 done out31 rdone");
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    mb_assert_clean(m);
}

/* 実行中の依頼がない（バッファが両方埋まって、USB の完了待ち）。command_done は Reset の中で、すぐ呼ぶ。 */
static void test_reset_in_data_in_with_live_command_but_nothing_outstanding(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_read10(m, 2, 0, 4, 8192);
    mb_complete_cmd(m);
    mb_complete_cmd(m); /* in(b0) + read(b1) */
    mb_complete_cmd(m); /* b1 が埋まる。b0 はまだ送信中: 次の読み出しは出ない */
    TEST_ASSERT_FALSE(m->cmd_pending);
    TEST_ASSERT_TRUE(msc_bot_cmd_live(&m->bot));
    TEST_ASSERT_FALSE(msc_bot_cmd_outstanding(&m->bot));
    TEST_ASSERT_EQUAL_UINT(0, m->n_done);
    msc_bot_reset(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    MB_ASSERT_LOG(m, "exec read1 read1 in2048 cancel done out31 rdone");
    mb_assert_clean(m);
}

static void test_reset_in_data_out(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_write10(m, 2, 512);
    mb_complete_cmd(m);
    TEST_ASSERT_TRUE(m->out_pending && m->out_ptr != m->cbw);
    msc_bot_reset(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    MB_ASSERT_LOG(m, "exec done out512 cancel out31 rdone");
    TEST_ASSERT_EQUAL_UINT(1, m->n_done); /* DATA_OUT の前に、すでに呼んでいる。二重に呼ばない */
    TEST_ASSERT_TRUE(m->out_pending && m->out_ptr == m->cbw);
    mb_assert_clean(m);
}

static void test_reset_in_status_send(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_tur(m, 2);
    mb_complete_cmd(m);
    TEST_ASSERT_TRUE(m->in_pending); /* CSW */
    msc_bot_reset(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    MB_ASSERT_LOG(m, "exec done in13 cancel out31 rdone");
    TEST_ASSERT_FALSE(m->in_pending);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    mb_assert_clean(m);
}

static void test_double_reset_while_waiting(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_tur(m, 2);
    msc_bot_reset(&m->bot);
    msc_bot_reset(&m->bot);
    msc_bot_reset(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_RESET_WAIT);
    mb_complete_cmd(m);
    MB_ASSERT_LOG(m, "exec done out31 rdone");
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    TEST_ASSERT_EQUAL_UINT(1, m->n_reset_done);
    mb_assert_clean(m);
}

/* Reset の後、遅れて届いた、古い依頼の完了（同じ seq の二重の完了）は捨てる。新しいコマンドに影響しない。 */
static void test_duplicate_old_completion_after_reset_is_dropped(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_send_tur(m, 2);
    uint32_t old_seq = m->cmd.seq;
    msc_bot_reset(&m->bot);
    mb_complete_cmd(m);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    mb_send_tur(m, 3);
    MB_ASSERT_STATE(m, MSC_BOT_EXEC);
    msc_bot_cmd_result_t r;
    memset(&r, 0, sizeof(r));
    r.seq = old_seq;
    r.good = true;
    msc_bot_command_complete(&m->bot, &r); /* 古い seq */
    MB_ASSERT_STATE(m, MSC_BOT_EXEC);
    TEST_ASSERT_EQUAL_UINT32(1, msc_bot_stats(&m->bot).stale_events);
    m->hold_cmd = false;
    mb_settle(m);
    mb_assert_csw(m, 3, 0, 0);
    TEST_ASSERT_EQUAL_UINT(2, m->n_done);
    mb_assert_clean(m);
}

/* ---- 切断 ----------------------------------------------------------------- */
static void test_detach_while_execute_outstanding(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_tur(m, 1);
    msc_bot_detach(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_DETACH_WAIT);
    TEST_ASSERT_EQUAL_UINT(0, m->n_done);
    mb_complete_cmd(m);
    MB_ASSERT_STATE(m, MSC_BOT_DETACHED);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    MB_ASSERT_LOG(m, "exec done");
    TEST_ASSERT_FALSE(m->out_pending);
    msc_bot_attach(&m->bot); /* 再接続: CBW を受ける */
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    TEST_ASSERT_TRUE(m->out_pending);
    mb_assert_clean(m);
}

static void test_detach_during_read_with_transfer_in_flight(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_read10(m, 2, 0, 4, 8192);
    mb_complete_cmd(m);
    mb_complete_cmd(m);
    msc_bot_detach(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_DETACH_WAIT);
    TEST_ASSERT_FALSE(m->in_pending);
    TEST_ASSERT_EQUAL_UINT(0, m->n_done);
    mb_complete_cmd(m);
    MB_ASSERT_STATE(m, MSC_BOT_DETACHED);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    TEST_ASSERT_EQUAL_UINT(2, m->n_read);
    MB_ASSERT_LOG(m, "exec read1 read1 in2048 cancel done");
    mb_assert_clean(m);
}

static void test_detach_in_idle_cancels_the_cbw_receive(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    mb_clear_log(m);
    msc_bot_detach(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_DETACHED);
    MB_ASSERT_LOG(m, "cancel");
    TEST_ASSERT_FALSE(m->out_pending);
    msc_bot_attach(&m->bot);
    MB_ASSERT_LOG(m, "cancel out31");
    TEST_ASSERT_EQUAL_UINT(0, m->n_exec);
    TEST_ASSERT_EQUAL_UINT(0, m->n_done);
    mb_assert_clean(m);
}

/* 切断で、bot が STALL した halt を、bot 自身が解く（再接続で、STALL が残らない）。 */
static void test_detach_unstalls_halted_pipes(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    uint8_t raw[31] = {0};
    mb_send_raw_cbw(m, raw, 31); /* 署名が 0: 不正な CBW -> 両方を STALL */
    MB_ASSERT_STATE(m, MSC_BOT_RECOVERY);
    mb_clear_log(m);
    msc_bot_detach(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_DETACHED);
    MB_ASSERT_LOG(m, "unstallI unstallO");
    TEST_ASSERT_FALSE(m->hw_in_stalled || m->hw_out_stalled);
    TEST_ASSERT_FALSE(msc_bot_halted(&m->bot, MSC_BOT_EP_IN) || msc_bot_halted(&m->bot, MSC_BOT_EP_OUT));
    msc_bot_attach(&m->bot);
    TEST_ASSERT_TRUE(m->out_pending);
    mb_assert_clean(m);
}

static void test_detach_while_waiting_for_in_unstall(void) {
    mb_t *m = mb_init(2048, false);
    m->auto_clear = false;
    mb_start(m);
    const uint8_t cdb[6] = {0};
    mb_send_cbw(m, 1, 36, true, cdb, 6); /* ケース 4 */
    mb_settle(m);
    MB_ASSERT_STATE(m, MSC_BOT_STATUS_WAIT_UNSTALL);
    mb_clear_log(m);
    msc_bot_detach(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_DETACHED);
    MB_ASSERT_LOG(m, "unstallI");
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    mb_assert_clean(m);
}

/* DETACH_WAIT の間の ATT / DET / RST。 */
static void test_detach_wait_then_attach_reattaches_after_completion(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_tur(m, 1);
    msc_bot_detach(&m->bot);
    msc_bot_attach(&m->bot); /* 実行中のうちに再接続 */
    MB_ASSERT_STATE(m, MSC_BOT_DETACH_WAIT);
    msc_bot_reset(&m->bot); /* 無視 */
    MB_ASSERT_STATE(m, MSC_BOT_DETACH_WAIT);
    TEST_ASSERT_EQUAL_UINT(0, m->n_reset_done);
    mb_complete_cmd(m);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    MB_ASSERT_LOG(m, "exec done out31");
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    mb_assert_clean(m);
}

static void test_detach_wait_attach_detach_stays_detached(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_tur(m, 1);
    msc_bot_detach(&m->bot);
    msc_bot_attach(&m->bot);
    msc_bot_detach(&m->bot);
    mb_complete_cmd(m);
    MB_ASSERT_STATE(m, MSC_BOT_DETACHED);
    MB_ASSERT_LOG(m, "exec done");
    TEST_ASSERT_FALSE(m->out_pending);
    mb_assert_clean(m);
}

static void test_reset_wait_then_detach(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    mb_start(m);
    mb_clear_log(m);
    mb_send_tur(m, 1);
    msc_bot_reset(&m->bot);
    msc_bot_detach(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_DETACH_WAIT);
    mb_complete_cmd(m);
    MB_ASSERT_STATE(m, MSC_BOT_DETACHED);
    TEST_ASSERT_EQUAL_UINT(0, m->n_reset_done); /* 切断されたので、Reset の完了は呼ばない */
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    mb_assert_clean(m);
}

static void test_events_while_detached_are_ignored(void) {
    mb_t *m = mb_init(2048, false);
    msc_bot_reset(&m->bot);
    msc_bot_halt_cleared(&m->bot, MSC_BOT_EP_IN);
    msc_bot_halt_cleared(&m->bot, MSC_BOT_EP_OUT);
    msc_bot_detach(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_DETACHED);
    TEST_ASSERT_EQUAL_UINT(0, m->nlog);
    msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_OUT, 31, true);
    msc_bot_xfer_complete(&m->bot, MSC_BOT_EP_IN, 13, true);
    msc_bot_cmd_result_t r;
    memset(&r, 0, sizeof(r));
    r.seq = 1;
    msc_bot_command_complete(&m->bot, &r);
    TEST_ASSERT_EQUAL_UINT32(3, msc_bot_stats(&m->bot).stale_events);
    MB_ASSERT_STATE(m, MSC_BOT_DETACHED);
    TEST_ASSERT_EQUAL_UINT(0, m->nlog);
    mb_assert_clean(m);
}

static void test_attach_twice_arms_once(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    msc_bot_attach(&m->bot);
    msc_bot_attach(&m->bot);
    MB_ASSERT_LOG(m, "out31");
    mb_assert_clean(m);
}

/* ---- 再入 ----------------------------------------------------------------- */
/* USB の転送も、依頼の完了も、発行された関数の中で、すぐ返る。1.2 MB の READ が、1 回の呼び出しの中で、再帰せずに終わる。 */
static void test_fully_synchronous_environment_large_read(void) {
    mb_t *m = mb_init(8192, false);
    m->sync = true;
    m->sync_xfer = true;
    mb_start(m);
    mb_send_read12(m, 0x99, 5, 600, 600u * 2048u);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE); /* 1 回の mb_send の中で、CSW まで終わった */
    mb_assert_clean(m);
    mb_assert_csw(m, 0x99, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(600u * 2048u, m->rx_len);
    mb_assert_rx_pattern(m, 5, 0, 600u * 2048u);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    TEST_ASSERT_TRUE(m->out_pending);
}

static void test_fully_synchronous_environment_other_commands(void) {
    mb_t *m = mb_init(2048, false);
    m->sync = true;
    m->sync_xfer = true;
    m->host_out_left = 4096;
    mb_start(m);
    mb_send_write10(m, 1, 4096); /* OUT の読み捨ても、再入で進む */
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    mb_assert_csw(m, 1, 1, 4096);
    mb_send_inquiry(m, 2, 100); /* ケース 5: STALL。ホストのクリアは、手で */
    MB_ASSERT_STATE(m, MSC_BOT_STATUS_WAIT_UNSTALL);
    mb_host_clear_halt(m, MSC_BOT_EP_IN);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    mb_assert_csw(m, 2, 0, 64);
    TEST_ASSERT_EQUAL_UINT(2, m->n_done);
    mb_assert_clean(m);
}

static int hook_count;
static void hook_reset_in_cancel(mb_t *m, mb_act_t a) {
    if (a == A_CANCEL && hook_count++ == 0) msc_bot_reset(&m->bot);
}

/* Reset の処理の中（cancel_xfers の中）で、もう 1 つ Reset が来る。後から処理され、二重に command_done しない。 */
static void test_reset_inside_reset(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    hook_count = 0;
    mb_start(m);
    mb_send_read10(m, 2, 0, 4, 8192);
    mb_complete_cmd(m);
    mb_complete_cmd(m); /* in(b0) + 読み出し */
    m->hook = hook_reset_in_cancel;
    msc_bot_reset(&m->bot);
    m->hook = NULL;
    MB_ASSERT_STATE(m, MSC_BOT_RESET_WAIT);
    TEST_ASSERT_EQUAL_UINT(1, hook_count);
    TEST_ASSERT_EQUAL_UINT(1, m->n_cancel); /* 2 つ目の Reset は、RESET_WAIT で無視される */
    mb_complete_cmd(m);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    TEST_ASSERT_EQUAL_UINT(1, m->n_reset_done);
    mb_assert_clean(m);
}

static void hook_detach_in_done(mb_t *m, mb_act_t a) {
    if (a == A_DONE && hook_count++ == 0) msc_bot_detach(&m->bot);
}

/* command_done の中で、切断が来る。CSW の送信を発行した後に、切断が処理され、転送を取り消す。 */
static void test_detach_inside_command_done(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    hook_count = 0;
    mb_start(m);
    mb_clear_log(m);
    mb_send_tur(m, 1);
    m->hook = hook_detach_in_done;
    mb_complete_cmd(m);
    m->hook = NULL;
    MB_ASSERT_STATE(m, MSC_BOT_DETACHED);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    MB_ASSERT_LOG(m, "exec done in13 cancel");
    TEST_ASSERT_FALSE(m->in_pending);
    mb_assert_clean(m);
}

static void hook_detach_in_submit(mb_t *m, mb_act_t a) {
    if (a == A_SUBMIT_EXEC && hook_count++ == 0) msc_bot_detach(&m->bot);
}

/* EXECUTE を依頼する関数の中で、切断が来る。依頼は受け付けられているので、完了を待ってから command_done。 */
static void test_detach_inside_submit_command(void) {
    mb_t *m = mb_init(2048, false);
    m->hold_cmd = true;
    hook_count = 0;
    mb_start(m);
    mb_clear_log(m);
    m->hook = hook_detach_in_submit;
    mb_send_tur(m, 1);
    m->hook = NULL;
    MB_ASSERT_STATE(m, MSC_BOT_DETACH_WAIT);
    TEST_ASSERT_EQUAL_UINT(0, m->n_done);
    mb_complete_cmd(m);
    MB_ASSERT_STATE(m, MSC_BOT_DETACHED);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    mb_assert_clean(m);
}

static void hook_clear_in_stall(mb_t *m, mb_act_t a) {
    if (a == A_STALL_IN) mb_host_clear_halt(m, MSC_BOT_EP_IN);
}

/* STALL を発行する関数の中で、ホストの CLEAR_FEATURE が来る（STALL が即座にクリアされる）。デッドロックせず、CSW が出る。 */
static void test_clear_halt_inside_stall(void) {
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    mb_clear_log(m);
    m->hook = hook_clear_in_stall;
    const uint8_t cdb[6] = {0};
    mb_send_cbw(m, 1, 36, true, cdb, 6); /* ケース 4 */
    mb_settle(m);
    m->hook = NULL;
    mb_assert_clean(m);
    mb_assert_csw(m, 1, 0, 36);
    MB_ASSERT_LOG(m, "exec done stallI in13 out31");
}

static void hook_reset_in_reset_done(mb_t *m, mb_act_t a) {
    if (a == A_RESET_DONE && hook_count++ == 0) msc_bot_reset(&m->bot);
}

static void test_reset_inside_reset_done(void) {
    mb_t *m = mb_init(2048, false);
    hook_count = 0;
    mb_start(m);
    mb_clear_log(m);
    m->hook = hook_reset_in_reset_done;
    msc_bot_reset(&m->bot);
    m->hook = NULL;
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    MB_ASSERT_LOG(m, "rdone rdone");
    mb_assert_clean(m);
}

static void hook_flood(mb_t *m, mb_act_t a) {
    if (a == A_STALL_IN && hook_count++ == 0) {
        for (int i = 0; i < 12; i++) msc_bot_halt_cleared(&m->bot, MSC_BOT_EP_OUT); /* 契約違反: 1 回の呼び出しで 1 つまで */
    }
}

/* FIFO があふれたら、イベントを捨て、数える（クラッシュしない）。契約違反の検出用。 */
static void test_event_fifo_overflow_is_counted_not_fatal(void) {
    mb_t *m = mb_init(2048, false);
    hook_count = 0;
    mb_start(m);
    m->hook = hook_flood;
    const uint8_t cdb[6] = {0};
    mb_send_cbw(m, 1, 36, true, cdb, 6); /* ケース 4: stall_in の中で、12 個のイベントを入れる */
    mb_settle(m);
    m->hook = NULL;
    TEST_ASSERT_EQUAL_UINT32(12 - MSC_BOT_EVQ_LEN, msc_bot_stats(&m->bot).queue_overflow);
    TEST_ASSERT_EQUAL_UINT(1, m->n_done);
    TEST_ASSERT_EQUAL_UINT(0, m->viol);
}

/* ---- command_done の回数（どの終わり方でも、実行したコマンドごとに 1 回）-------- */
typedef void (*ending_fn)(mb_t *m);

static void end_normal_tur(mb_t *m) { mb_send_tur(m, 1); mb_settle(m); }
static void end_stall_case4(mb_t *m) {
    const uint8_t cdb[6] = {0};
    mb_send_cbw(m, 1, 36, true, cdb, 6);
    mb_settle(m);
}
static void end_read_ok(mb_t *m) { mb_send_read10(m, 1, 0, 3, 6144); mb_settle(m); }
static void end_read_failed(mb_t *m) { m->read_fail_nth = 1; mb_send_read10(m, 1, 0, 3, 6144); mb_settle(m); }
static void end_execute_check_condition(mb_t *m) { mb_send_write10(m, 1, 0); mb_settle(m); }
static void end_out_discard(mb_t *m) { m->host_out_left = 512; mb_send_write10(m, 1, 512); mb_settle(m); }
static void end_out_stall(mb_t *m) { mb_send_write10(m, 1, 512); mb_settle(m); }
static void end_invalid_cbw(mb_t *m) { uint8_t raw[31] = {0}; mb_send_raw_cbw(m, raw, 31); }
static void end_reset_mid_exec(mb_t *m) { m->hold_cmd = true; mb_send_tur(m, 1); msc_bot_reset(&m->bot); mb_complete_cmd(m); }
static void end_reset_mid_read(mb_t *m) {
    m->hold_cmd = true;
    mb_send_read10(m, 1, 0, 4, 8192);
    mb_complete_cmd(m);
    mb_complete_cmd(m);
    msc_bot_reset(&m->bot);
    mb_complete_cmd(m);
}
static void end_detach_mid_exec(mb_t *m) { m->hold_cmd = true; mb_send_tur(m, 1); msc_bot_detach(&m->bot); mb_complete_cmd(m); }
static void end_detach_mid_read(mb_t *m) {
    m->hold_cmd = true;
    mb_send_read10(m, 1, 0, 4, 8192);
    mb_complete_cmd(m);
    mb_complete_cmd(m);
    msc_bot_detach(&m->bot);
    mb_complete_cmd(m);
}
static void end_inquiry_ok(mb_t *m) {
    mb_send_inquiry(m, 1, 36);
    mb_settle(m);
}
static void end_submit_refused_read(mb_t *m) {
    m->hold_cmd = true;
    mb_send_read10(m, 1, 0, 2, 4096);
    m->refuse_submit = true;
    mb_complete_cmd(m);
}
static void end_csw_xfer_refused(mb_t *m) { m->refuse_in = true; mb_send_tur(m, 1); mb_settle(m); }

static void test_command_done_exactly_once_for_every_ending(void) {
    struct { const char *name; ending_fn fn; bool out_stall; unsigned execs; } rows[] = {
        {"normal", end_normal_tur, false, 1},
        {"STALL (case 4)", end_stall_case4, false, 1},
        {"READ ok", end_read_ok, false, 1},
        {"READ failed mid-way", end_read_failed, false, 1},
        {"EXECUTE CHECK CONDITION", end_execute_check_condition, false, 1},
        {"OUT discard", end_out_discard, false, 1},
        {"OUT stall", end_out_stall, true, 1},
        {"invalid CBW (no command executed)", end_invalid_cbw, false, 0},
        {"reset mid EXECUTE", end_reset_mid_exec, false, 1},
        {"reset mid READ", end_reset_mid_read, false, 1},
        {"detach mid EXECUTE", end_detach_mid_exec, false, 1},
        {"detach mid READ", end_detach_mid_read, false, 1},
        {"INQUIRY ok", end_inquiry_ok, false, 1},
        {"READ_BLOCKS refused", end_submit_refused_read, false, 1},
        {"CSW transfer refused", end_csw_xfer_refused, false, 1},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        mb_t *m = mb_init(2048, rows[i].out_stall);
        mb_start(m);
        rows[i].fn(m);
        char msg[120];
        snprintf(msg, sizeof(msg), "ending: %s", rows[i].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(rows[i].execs, m->n_exec, msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(rows[i].execs, m->n_done, msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, m->live, msg);
        TEST_ASSERT_FALSE_MESSAGE(msc_bot_cmd_live(&m->bot), msg);
        TEST_ASSERT_FALSE_MESSAGE(msc_bot_cmd_outstanding(&m->bot), msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows[i].execs, msc_bot_stats(&m->bot).exec_submitted, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows[i].execs, msc_bot_stats(&m->bot).cmd_done_called, msg);
        mb_assert_clean(m);
    }
}

/* ---- 初期化の検査 --------------------------------------------------------- */
static void test_init_validates_config(void) {
    mb_t *m = mb_init(2048, false);
    msc_bot_cfg_t c = m->cfg;
    msc_bot_t b;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_OK, msc_bot_init(&b, &c));
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DETACHED, msc_bot_state(&b));
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(NULL, &c));
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, NULL));

    msc_bot_cfg_t d = c;
    d.buf_size = 1024; /* 2048 未満 */
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c;
    d.buf_size = 3000; /* 2048 の倍数でない */
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c;
    d.buf_size = 0;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c;
    d.buf_size = 4096;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_OK, msc_bot_init(&b, &d));
    d = c;
    d.buf[0] = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c;
    d.buf[1] = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c;
    d.buf[1] = d.buf[0]; /* 同じバッファ */
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c;
    d.cbw_buf = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c;
    d.csw_buf = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    /* 必須の関数 */
    d = c; d.ops.xfer_in = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c; d.ops.xfer_out = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c; d.ops.stall_in = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c; d.ops.stall_out = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c; d.ops.cancel_xfers = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c; d.ops.submit_command = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    d = c; d.ops.command_done = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_ERR_ARG, msc_bot_init(&b, &d));
    /* 省略してよい関数 */
    d = c; d.ops.unstall = NULL; d.ops.reset_done = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_OK, msc_bot_init(&b, &d));
}

/* unstall / reset_done を省略しても動く。 */
static void test_optional_ops_may_be_null(void) {
    mb_t *m = mb_init(2048, false);
    m->cfg.ops.unstall = NULL;
    m->cfg.ops.reset_done = NULL;
    TEST_ASSERT_EQUAL_INT(MSC_BOT_OK, msc_bot_init(&m->bot, &m->cfg));
    mb_start(m);
    uint8_t raw[31] = {0};
    mb_send_raw_cbw(m, raw, 31);
    msc_bot_reset(&m->bot);
    msc_bot_detach(&m->bot);
    m->hw_in_stalled = m->hw_out_stalled = false; /* unstall を省略したので、スタックが解く役 */
    msc_bot_attach(&m->bot);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    mb_send_tur(m, 1);
    msc_bot_reset(&m->bot);
    mb_settle(m);
    mb_assert_clean(m);
}

/* 初期化していない（ゼロの）msc_bot に、イベントを与えても、何も起きない（ops が NULL でも落ちない）。 */
static void test_events_before_init_are_ignored(void) {
    static msc_bot_t z;
    memset(&z, 0, sizeof(z));
    msc_bot_attach(&z);
    msc_bot_detach(&z);
    msc_bot_reset(&z);
    msc_bot_halt_cleared(&z, MSC_BOT_EP_IN);
    msc_bot_xfer_complete(&z, MSC_BOT_EP_OUT, 31, true);
    msc_bot_cmd_result_t r;
    memset(&r, 0, sizeof(r));
    msc_bot_command_complete(&z, &r);
    TEST_ASSERT_EQUAL_INT(MSC_BOT_DETACHED, msc_bot_state(&z));
    /* NULL の bot と、NULL の結果 */
    msc_bot_attach(NULL);
    msc_bot_command_complete(NULL, &r);
    mb_t *m = mb_init(2048, false);
    mb_start(m);
    msc_bot_command_complete(&m->bot, NULL);
    MB_ASSERT_STATE(m, MSC_BOT_IDLE);
    TEST_ASSERT_EQUAL_UINT32(0, msc_bot_stats(&m->bot).stale_events);
    mb_assert_clean(m);
}

/* 状態の名前は、ログに使う。固定の文字列。 */
static void test_state_names_exact(void) {
    TEST_ASSERT_EQUAL_STRING("DETACHED", msc_bot_state_name(MSC_BOT_DETACHED));
    TEST_ASSERT_EQUAL_STRING("IDLE", msc_bot_state_name(MSC_BOT_IDLE));
    TEST_ASSERT_EQUAL_STRING("EXEC", msc_bot_state_name(MSC_BOT_EXEC));
    TEST_ASSERT_EQUAL_STRING("DATA_IN", msc_bot_state_name(MSC_BOT_DATA_IN));
    TEST_ASSERT_EQUAL_STRING("DATA_OUT", msc_bot_state_name(MSC_BOT_DATA_OUT));
    TEST_ASSERT_EQUAL_STRING("STATUS_WAIT_UNSTALL", msc_bot_state_name(MSC_BOT_STATUS_WAIT_UNSTALL));
    TEST_ASSERT_EQUAL_STRING("STATUS_SEND", msc_bot_state_name(MSC_BOT_STATUS_SEND));
    TEST_ASSERT_EQUAL_STRING("RECOVERY", msc_bot_state_name(MSC_BOT_RECOVERY));
    TEST_ASSERT_EQUAL_STRING("RESET_WAIT", msc_bot_state_name(MSC_BOT_RESET_WAIT));
    TEST_ASSERT_EQUAL_STRING("DETACH_WAIT", msc_bot_state_name(MSC_BOT_DETACH_WAIT));
    TEST_ASSERT_EQUAL_STRING("?", msc_bot_state_name(MSC_BOT_STATE_COUNT));
}

static void test_state_names_are_distinct(void) {
    for (int a = 0; a < MSC_BOT_STATE_COUNT; a++) {
        for (int b = a + 1; b < MSC_BOT_STATE_COUNT; b++) {
            TEST_ASSERT_TRUE(strcmp(msc_bot_state_name((msc_bot_state_t)a), msc_bot_state_name((msc_bot_state_t)b)) != 0);
        }
        TEST_ASSERT_TRUE(strcmp(msc_bot_state_name((msc_bot_state_t)a), "?") != 0);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reset_while_execute_outstanding_waits_then_done_once);
    RUN_TEST(test_reset_during_read_with_transfer_in_flight);
    RUN_TEST(test_reset_during_first_read_without_transfer);
    RUN_TEST(test_reset_in_data_in_with_live_command_but_nothing_outstanding);
    RUN_TEST(test_reset_in_data_out);
    RUN_TEST(test_reset_in_status_send);
    RUN_TEST(test_double_reset_while_waiting);
    RUN_TEST(test_duplicate_old_completion_after_reset_is_dropped);
    RUN_TEST(test_detach_while_execute_outstanding);
    RUN_TEST(test_detach_during_read_with_transfer_in_flight);
    RUN_TEST(test_detach_in_idle_cancels_the_cbw_receive);
    RUN_TEST(test_detach_unstalls_halted_pipes);
    RUN_TEST(test_detach_while_waiting_for_in_unstall);
    RUN_TEST(test_detach_wait_then_attach_reattaches_after_completion);
    RUN_TEST(test_detach_wait_attach_detach_stays_detached);
    RUN_TEST(test_reset_wait_then_detach);
    RUN_TEST(test_events_while_detached_are_ignored);
    RUN_TEST(test_attach_twice_arms_once);
    RUN_TEST(test_fully_synchronous_environment_large_read);
    RUN_TEST(test_fully_synchronous_environment_other_commands);
    RUN_TEST(test_reset_inside_reset);
    RUN_TEST(test_detach_inside_command_done);
    RUN_TEST(test_detach_inside_submit_command);
    RUN_TEST(test_clear_halt_inside_stall);
    RUN_TEST(test_reset_inside_reset_done);
    RUN_TEST(test_event_fifo_overflow_is_counted_not_fatal);
    RUN_TEST(test_command_done_exactly_once_for_every_ending);
    RUN_TEST(test_init_validates_config);
    RUN_TEST(test_optional_ops_may_be_null);
    RUN_TEST(test_state_names_are_distinct);
    RUN_TEST(test_state_names_exact);
    RUN_TEST(test_events_before_init_are_ignored);
    return UNITY_END();
}

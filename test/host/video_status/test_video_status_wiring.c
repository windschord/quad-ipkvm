/*
 * 結線（統合）: 実際の kvm_ctrl（-> kvm_logic）+ モック HAL + 偽の映像状態（video_status）。
 * kvm_service と同じ形: HAL の video_sync を video_status_hal_sync に差し替えてから kvm_ctrl_init に渡す。
 * （モック HAL 自身の映像状態 mock_set_sync は、この結線では使われない。それも固定する。）
 *
 * 期待値は、kvm_logic.c の実際の挙動を読んで決めた（docs/KVM_LOGIC_SCENARIOS.md 1 章、docs/VIDEO_STATUS_TESTS.md）。要点:
 *   - 切替の映像待ち（SWITCH_WAIT_VIDEO）は、LOCKED を見たら（LOCKED の立ち上がりを待たずに）その tick で完了する。
 *   - NO_SIGNAL は、映像待ちに入った時刻（か NO_SIGNAL の開始の遅いほう）から no_signal_settle_ms 続けば完了する。
 *   - UNKNOWN は完了条件ではなく、switch_video_sync_timeout_ms 後に完了する。
 *   - ATX のロックは idle のときに解ける。映像が NO_SIGNAL / UNKNOWN でも、idle なら ATX は受理される（電源の入っていない PC を起動するため）。
 */
#include "../kvm_ctrl/kc_common.h"
#include "video_status.h"

static video_status_fake_t V;

void setUp(void) {
    fx_reset(1);
    video_status_fake_init(&V);
    video_status_set_provider(video_status_fake_provider, &V);
}
void tearDown(void) { video_status_set_provider(NULL, NULL); }

/* kvm_service と同じ形の結線 */
static kvm_ctrl_result_t wired_init(void) {
    kvm_board_hal_t hal = mock_hal(&M);
    hal.video_sync = video_status_hal_sync;
    return kvm_ctrl_init(&C, &CFG, &hal);
}

/* LED が ch を示し、映像が (s, w, h) の状態で起動して、idle まで進める */
static void boot_wired(uint8_t ch, kvm_video_sync_state_t s, uint32_t w, uint32_t h) {
    fx_reset(1);
    video_status_fake_set(&V, s, w, h);
    mock_set_led_channel(&M, ch);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, wired_init());
    fx_tick(0);
    for (int i = 0; i < 6; i++) fx_step(); /* 60 ms。LED の安定 50 ms を超える */
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT8(ch, st.active_channel);
    TEST_ASSERT_FALSE(st.atx_locked);
    mock_reset_calls(&M);
}

static void expect_clean_board(void) {
    TEST_ASSERT_FALSE_MESSAGE(mock_violated(&M), "HAL contract violated by kvm_ctrl");
    TEST_ASSERT_EQUAL(0, mock_active_count(&M));
}

/* 切替を要求して、KVM のパルスが始まったら映像を (s, w, h) にし、パルスが終わったら LED を target にする。
 * 映像待ちに入った時刻と、idle に戻った時刻を返す */
typedef struct {
    uint64_t t_wait_video;
    uint64_t t_idle;
} sw_times_t;

static sw_times_t run_switch(uint8_t target, kvm_video_sync_state_t s_at_pulse, uint32_t w, uint32_t h) {
    sw_times_t t = {0, 0};
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, target, NULL));
    bool video_applied = false;
    for (int i = 0; i < 2000; i++) {
        if (!video_applied && C.logic.state == KVM_STATE_SWITCH_PULSE) {
            video_status_fake_set(&V, s_at_pulse, w, h);
            video_applied = true;
        }
        if (C.logic.state == KVM_STATE_SWITCH_WAIT_LED) mock_set_led_channel(&M, target);
        fx_step();
        if (t.t_wait_video == 0 && C.logic.state == KVM_STATE_SWITCH_WAIT_VIDEO) t.t_wait_video = T;
        if (fx_status().state == KVM_CTRL_STATE_IDLE) {
            t.t_idle = T;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(t.t_wait_video != 0, "never reached the video wait");
    TEST_ASSERT_TRUE_MESSAGE(t.t_idle != 0, "switch never finished");
    return t;
}

/* ① 起動直後（映像は UNKNOWN）-> LED 確定 -> idle */
void test_boot_with_unknown_video_reaches_idle_and_reports_unknown(void) {
    fx_reset(1);
    mock_set_led_channel(&M, 2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, wired_init());
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_UNKNOWN, fx_status().video_sync); /* tick の前 */
    fx_tick(0);
    /* LED が安定するまでは unknown */
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_UNKNOWN, fx_status().state);
    for (int i = 0; i < 6; i++) fx_step();
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT8(2, st.active_channel);
    TEST_ASSERT_FALSE_MESSAGE(st.atx_locked, "an UNKNOWN video must not keep the ATX locked in idle");
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_UNKNOWN, st.video_sync);
    expect_clean_board();
}

/* ② 切替: 映像が NO_SIGNAL -> LOCKED に変わって、初めて（NO_SIGNAL の settle 時間より前に）完了する */
void test_switch_completes_when_video_goes_no_signal_then_locked(void) {
    boot_wired(2, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    uint32_t op = 0;
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, &op));
    TEST_ASSERT_EQUAL_UINT32(1, op);

    /* KVM のパルスが始まったら、映像が切れる（HDMI の切替） */
    for (int i = 0; i < 100 && C.logic.state != KVM_STATE_SWITCH_PULSE; i++) fx_step();
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_PULSE, C.logic.state);
    video_status_fake_set(&V, KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    for (int i = 0; i < 100 && C.logic.state != KVM_STATE_SWITCH_WAIT_LED; i++) fx_step();
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_LED, C.logic.state);
    mock_set_led_channel(&M, 1);
    for (int i = 0; i < 100 && C.logic.state != KVM_STATE_SWITCH_WAIT_VIDEO; i++) fx_step();
    TEST_ASSERT_EQUAL(KVM_STATE_SWITCH_WAIT_VIDEO, C.logic.state);
    uint64_t t_wv = T;

    /* NO_SIGNAL のあいだは切替中。settle 時間の手前まで、完了しない。ATX は拒否される（busy）。ロックされている */
    while (T + 10 < t_wv + CFG.logic.no_signal_settle_ms - 100) fx_step();
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_SWITCHING, st.state);
    TEST_ASSERT_TRUE(st.atx_locked);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_NO_SIGNAL, st.video_sync);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_PENDING, st.last_op.result);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_power(&C, 1, KVM_CTRL_POWER_PRESS, NULL));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));

    /* 映像が LOCKED になった tick の次の tick で完了する（settle 時間は待たない） */
    video_status_fake_set(&V, KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    fx_step();
    st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT8(1, st.active_channel);
    TEST_ASSERT_FALSE(st.atx_locked);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_LOCKED, st.video_sync);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, st.last_op.result);
    TEST_ASSERT_TRUE_MESSAGE(T < t_wv + CFG.logic.no_signal_settle_ms, "completed only after the settle time: LOCKED not wired");
    TEST_ASSERT_EQUAL_UINT8(1, M.addr);
    /* 完了したので、ATX が受理される */
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 1, KVM_CTRL_POWER_PRESS, NULL));
    expect_clean_board();
}

/* ② 映像が NO_SIGNAL のまま戻らない: settle 時間（映像待ちに入ってから）で完了する。映像は NO_SIGNAL と報告 */
void test_switch_with_persistent_no_signal_completes_after_settle(void) {
    boot_wired(2, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    sw_times_t t = run_switch(1, KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    TEST_ASSERT_EQUAL_UINT64(t.t_wait_video + CFG.logic.no_signal_settle_ms, t.t_idle);
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_NO_SIGNAL, st.video_sync);
    TEST_ASSERT_EQUAL_UINT8(1, st.active_channel);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, st.last_op.result);
    TEST_ASSERT_FALSE(st.atx_locked);
    expect_clean_board();
}

/* ③ 映像が LOCKED のまま変わらない: 既存の kvm_logic は、LOCKED を見たらすぐ完了する（タイムアウトを待たない） */
void test_switch_with_video_locked_throughout_completes_at_once(void) {
    boot_wired(2, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    sw_times_t t = run_switch(1, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(t.t_wait_video + 10, t.t_idle, "LOCKED must complete on the first tick of the video wait");
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, fx_status().last_op.result);
    expect_clean_board();
}

/* ③ 映像が UNKNOWN のまま（提供元が状態を出せない）: switch_video_sync_timeout_ms 後に完了する */
void test_switch_with_video_unknown_completes_at_the_timeout(void) {
    boot_wired(2, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    sw_times_t t = run_switch(1, KVM_VIDEO_SYNC_UNKNOWN, 0, 0);
    TEST_ASSERT_EQUAL_UINT64(t.t_wait_video + CFG.logic.switch_video_sync_timeout_ms, t.t_idle);
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_UNKNOWN, st.video_sync);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, st.last_op.result);
    TEST_ASSERT_FALSE(st.atx_locked);
    expect_clean_board();
}

/* 3 つの状態で完了までの時間が違う（映像が結線されている）。LOCKED < NO_SIGNAL(settle) < UNKNOWN(timeout) */
void test_completion_time_orders_locked_no_signal_unknown(void) {
    boot_wired(2, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    sw_times_t l = run_switch(1, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    boot_wired(2, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    sw_times_t n = run_switch(1, KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    boot_wired(2, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    sw_times_t u = run_switch(1, KVM_VIDEO_SYNC_UNKNOWN, 0, 0);
    TEST_ASSERT_TRUE(l.t_idle - l.t_wait_video < n.t_idle - n.t_wait_video);
    TEST_ASSERT_TRUE(n.t_idle - n.t_wait_video < u.t_idle - u.t_wait_video);
}

/* ④ idle では、映像が NO_SIGNAL でも ATX はロックされない（kvm_logic は idle の ATX のロックに映像を使わない。
 *    電源の入っていない PC を起動するのに必要）。LOCKED -> NO_SIGNAL の変化でもロックされない */
void test_atx_is_not_locked_in_idle_while_video_is_no_signal(void) {
    boot_wired(3, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    video_status_fake_set(&V, KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    fx_steps(100); /* 1 秒。settle 時間(500 ms)を十分に超える */
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_NO_SIGNAL, st.video_sync);
    TEST_ASSERT_FALSE(st.atx_locked);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", fx_trace(), "a video change in idle must not move any output");

    uint32_t op = 0;
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 3, KVM_CTRL_POWER_PRESS, &op));
    /* パルスを出し終える */
    for (int i = 0; i < 100 && fx_status().state != KVM_CTRL_STATE_IDLE; i++) fx_step();
    for (int i = 0; i < 100 && fx_status().last_op.result == KVM_CTRL_OP_PENDING; i++) fx_step();
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, fx_status().last_op.result);
    TEST_ASSERT_TRUE(fx_find(MOCK_FN_ASSERT_LINE, 0) >= 0);
    expect_clean_board();
}

/* ④ 手動の切替（KVM のボタンを人が押した）の追従中は ATX がロックされ、映像が NO_SIGNAL -> LOCKED で idle に戻る */
void test_manual_switch_tracking_waits_for_video_and_keeps_atx_locked(void) {
    boot_wired(1, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    video_status_fake_set(&V, KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    mock_set_led_channel(&M, 3);
    kvm_ctrl_status_t st;
    int guard = 0;
    while (C.logic.state != KVM_STATE_MANUAL_TRACK_WAIT_VIDEO && guard++ < 100) fx_step();
    TEST_ASSERT_EQUAL(KVM_STATE_MANUAL_TRACK_WAIT_VIDEO, C.logic.state);
    fx_steps(5);
    st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_TRACKING, st.state);
    TEST_ASSERT_TRUE(st.atx_locked);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_power(&C, 3, KVM_CTRL_POWER_PRESS, NULL));

    video_status_fake_set(&V, KVM_VIDEO_SYNC_LOCKED, 1024, 768);
    fx_steps(2);
    st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT8(3, st.active_channel);
    TEST_ASSERT_FALSE(st.atx_locked);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_LOCKED, st.video_sync);
    expect_clean_board();
}

/* ⑤ status の video_sync は、直近の tick で読んだ偽の状態に追従する（tick の間は変わらない） */
void test_status_video_sync_follows_the_fake_per_tick(void) {
    boot_wired(2, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_LOCKED, fx_status().video_sync);

    video_status_fake_set(&V, KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    TEST_ASSERT_EQUAL_MESSAGE(KVM_VIDEO_SYNC_LOCKED, fx_status().video_sync, "status is the value read at the last tick");
    fx_step();
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_NO_SIGNAL, fx_status().video_sync);

    video_status_fake_set(&V, KVM_VIDEO_SYNC_UNKNOWN, 0, 0);
    fx_step();
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_UNKNOWN, fx_status().video_sync);

    video_status_fake_set(&V, KVM_VIDEO_SYNC_LOCKED, 640, 480);
    fx_step();
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_LOCKED, fx_status().video_sync);
    /* 解像度の変化（LOCKED のまま）は、状態機械に影響しない */
    video_status_fake_set(&V, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    fx_steps(3);
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_LOCKED, st.video_sync);
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_FALSE(st.atx_locked);
    expect_clean_board();
}

/* ⑤ 不整合な値（LOCKED で解像度 0、範囲外の state）は、video_status が UNKNOWN に直すので、kvm_ctrl には UNKNOWN が届く */
void test_inconsistent_provider_values_reach_kvm_as_unknown(void) {
    boot_wired(2, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    video_status_fake_set(&V, KVM_VIDEO_SYNC_LOCKED, 0, 0);
    fx_step();
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_UNKNOWN, fx_status().video_sync);

    video_status_fake_set(&V, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    fx_step();
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_LOCKED, fx_status().video_sync);

    video_status_fake_set(&V, (kvm_video_sync_state_t)9, 1920, 1080);
    fx_step();
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_UNKNOWN, fx_status().video_sync);

    /* UNKNOWN は完了条件でないので、LOCKED と偽って 0x0 を出す提供元では、切替はタイムアウトまで待つ */
    sw_times_t t = run_switch(1, KVM_VIDEO_SYNC_LOCKED, 0, 1080);
    TEST_ASSERT_EQUAL_UINT64(t.t_wait_video + CFG.logic.switch_video_sync_timeout_ms, t.t_idle);
}

/* ⑥ 提供元がない（既定）: video は UNKNOWN のまま。idle には入れて、ATX は受理される。切替はタイムアウトで完了する */
void test_without_provider_video_stays_unknown_and_kvm_still_works_safely(void) {
    video_status_set_provider(NULL, NULL);
    fx_reset(1);
    mock_set_led_channel(&M, 2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, wired_init());
    fx_tick(0);
    for (int i = 0; i < 6; i++) fx_step();
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_UNKNOWN, st.video_sync);
    TEST_ASSERT_FALSE(st.atx_locked);

    /* ATX は受理される（idle の ATX のロックは映像に依存しない） */
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_RESET, NULL));
    for (int i = 0; i < 100 && fx_status().last_op.result == KVM_CTRL_OP_PENDING; i++) fx_step();
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, fx_status().last_op.result);
    /* チャンネルの不一致は、映像と無関係に拒否される */
    fx_steps(250); /* クールダウン(2000 ms)を超える */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_CHANNEL_MISMATCH, kvm_ctrl_power(&C, 1, KVM_CTRL_POWER_RESET, NULL));

    /* 切替は、映像が UNKNOWN のままなので、タイムアウトまで待ってから完了する（待ち続けて固まることはない） */
    sw_times_t t = run_switch(4, KVM_VIDEO_SYNC_UNKNOWN, 0, 0);
    TEST_ASSERT_EQUAL_UINT64(t.t_wait_video + CFG.logic.switch_video_sync_timeout_ms, t.t_idle);
    st = fx_status();
    TEST_ASSERT_EQUAL_UINT8(4, st.active_channel);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, st.last_op.result);
    expect_clean_board();
}

/* 提供元を実行中に外しても（テストの都合）、次の tick から UNKNOWN になる */
void test_removing_the_provider_reports_unknown_from_the_next_tick(void) {
    boot_wired(2, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    video_status_set_provider(NULL, NULL);
    fx_step();
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_UNKNOWN, fx_status().video_sync);
}

/* 結線の形: tick ごとに提供元を 1 回だけ読む。モック HAL 自身の video_sync は呼ばれず、mock_set_sync は効かない */
typedef struct {
    unsigned calls;
} count_t;
static void counting_provider(void *ctx, video_status_t *out) {
    count_t *c = (count_t *)ctx;
    c->calls++;
    out->state = KVM_VIDEO_SYNC_NO_SIGNAL;
    out->width = 0;
    out->height = 0;
}

void test_provider_is_read_once_per_tick_and_the_boards_own_video_sync_is_not_used(void) {
    count_t cnt = {0};
    video_status_set_provider(counting_provider, &cnt);
    fx_reset(1);
    mock_set_led_channel(&M, 2);
    mock_set_sync(&M, KVM_VIDEO_SYNC_LOCKED); /* モックの値は、この結線では使われない */
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, wired_init());
    TEST_ASSERT_EQUAL_UINT(0, cnt.calls); /* init では読まない */
    fx_tick(0);
    TEST_ASSERT_EQUAL_UINT(1, cnt.calls);
    fx_steps(9);
    TEST_ASSERT_EQUAL_UINT(10, cnt.calls);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, M.calls[MOCK_FN_VIDEO_SYNC], "the board's own video_sync must be replaced");
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_NO_SIGNAL, fx_status().video_sync);

    /* ステータスの取得（API）は、提供元を読まない */
    (void)fx_status();
    (void)fx_status();
    TEST_ASSERT_EQUAL_UINT(10, cnt.calls);
    /* 要求も読まない */
    (void)kvm_ctrl_select(&C, 1, NULL);
    TEST_ASSERT_EQUAL_UINT(10, cnt.calls);
}

/* フォールトのラッチ後は、kvm_ctrl_tick が映像を読まなくてよい（HAL を呼ばない）。ここでは、ラッチ後も安全に動くことだけを確かめる */
void test_after_a_fault_the_video_wiring_does_not_break_the_latch(void) {
    boot_wired(2, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    mock_fail_all(&M, MOCK_FN_READ_LED, true);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    mock_fail_all(&M, MOCK_FN_READ_LED, false);
    video_status_fake_set(&V, KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    TEST_ASSERT_TRUE(fx_status().fault);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    TEST_ASSERT_EQUAL(0, mock_active_count(&M));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_boot_with_unknown_video_reaches_idle_and_reports_unknown);
    RUN_TEST(test_switch_completes_when_video_goes_no_signal_then_locked);
    RUN_TEST(test_switch_with_persistent_no_signal_completes_after_settle);
    RUN_TEST(test_switch_with_video_locked_throughout_completes_at_once);
    RUN_TEST(test_switch_with_video_unknown_completes_at_the_timeout);
    RUN_TEST(test_completion_time_orders_locked_no_signal_unknown);
    RUN_TEST(test_atx_is_not_locked_in_idle_while_video_is_no_signal);
    RUN_TEST(test_manual_switch_tracking_waits_for_video_and_keeps_atx_locked);
    RUN_TEST(test_status_video_sync_follows_the_fake_per_tick);
    RUN_TEST(test_inconsistent_provider_values_reach_kvm_as_unknown);
    RUN_TEST(test_without_provider_video_stays_unknown_and_kvm_still_works_safely);
    RUN_TEST(test_removing_the_provider_reports_unknown_from_the_next_tick);
    RUN_TEST(test_provider_is_read_once_per_tick_and_the_boards_own_video_sync_is_not_used);
    RUN_TEST(test_after_a_fault_the_video_wiring_does_not_break_the_latch);
    return UNITY_END();
}

/* kvm_ctrl: 切替・ATX の要求と、HAL への出力の順序、判定の順序、クールダウン、操作の記録、状態の報告 */
#include "kc_common.h"

void setUp(void) { fx_reset(1); }
void tearDown(void) {}

/* kvm_logic の内部状態が s になるまで fx_step する（max_steps で打ち切り） */
static bool run_until_logic_state(kvm_logic_state_t s, int max_steps) {
    for (int i = 0; i < max_steps; i++) {
        if (C.logic.state == s) return true;
        fx_step();
    }
    return C.logic.state == s;
}

static void expect_clean_board(void) {
    TEST_ASSERT_FALSE_MESSAGE(mock_violated(&M), "HAL contract violated by kvm_ctrl");
    TEST_ASSERT_EQUAL(0, mock_active_count(&M));
}

/* ====================================================================================================
 * 切替(select)
 * ================================================================================================== */

void test_select_full_sequence_and_trace(void) {
    fx_boot(2);
    uint32_t op = 0;
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, &op));
    TEST_ASSERT_EQUAL_UINT32(1, op);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, M.log_len, "request must not touch the HAL");

    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_TRUE(st.has_last_op);
    TEST_ASSERT_EQUAL_UINT32(1, st.last_op.op_id);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_SELECT, st.last_op.kind);
    TEST_ASSERT_EQUAL_UINT8(1, st.last_op.channel);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_PENDING, st.last_op.result);

    fx_step(); /* 70 ms: A0/A1 を変える。全線 High */
    TEST_ASSERT_EQUAL_STRING("a1@70", fx_trace());
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_SWITCHING, fx_status().state);
    fx_step(); /* 80 ms: セットアップ時間が経過 -> KVM を有効にする */
    TEST_ASSERT_EQUAL_STRING("a1@70 S3:250@80", fx_trace());
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_KVM));
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_SWITCHING, fx_status().state);

    fx_complete_switch(1);
    TEST_ASSERT_EQUAL_STRING("a1@70 S3:250@80 r3@230", fx_trace());
    st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT8(1, st.active_channel);
    TEST_ASSERT_FALSE(st.atx_locked);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, st.last_op.result);
    TEST_ASSERT_EQUAL_UINT32(1, st.last_op.op_id);
    TEST_ASSERT_EQUAL_UINT8(1, M.addr);
    expect_clean_board();
}

void test_select_rejects_invalid_channels(void) {
    fx_boot(2);
    uint8_t bad[] = {0, 5, 6, 255};
    for (size_t i = 0; i < sizeof(bad); i++) {
        uint32_t op = 77;
        TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_select(&C, bad[i], &op));
        TEST_ASSERT_EQUAL_UINT32(0, op);
    }
    TEST_ASSERT_FALSE(fx_status().has_last_op);
    TEST_ASSERT_EQUAL_STRING("", fx_trace());
    /* 境界の 1 と 4 は有効 */
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 4, NULL));
}

void test_select_accepts_null_op_id(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 3, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, fx_status().last_op.op_id);
}

void test_select_already_selected_does_nothing(void) {
    fx_boot(2);
    uint32_t op = 77;
    TEST_ASSERT_EQUAL(KVM_CTRL_ALREADY_SELECTED, kvm_ctrl_select(&C, 2, &op));
    TEST_ASSERT_EQUAL_UINT32(0, op);
    TEST_ASSERT_FALSE(fx_status().has_last_op);
    fx_steps(20);
    TEST_ASSERT_EQUAL_STRING("", fx_trace()); /* 何も押さない */
    /* op_id は進んでいない */
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 3, &op));
    TEST_ASSERT_EQUAL_UINT32(1, op);
}

void test_select_unknown_state_is_distinguished(void) {
    /* 起動直後(tick 前) */
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    uint32_t op = 77;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_UNKNOWN_STATE, kvm_ctrl_select(&C, 1, &op));
    TEST_ASSERT_EQUAL_UINT32(0, op);

    /* idle だったあとで LED が消えた: 現在のチャンネルへの select でも unknown_state(already_selected ではない) */
    fx_boot(2);
    mock_set_led_off(&M);
    fx_steps(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_UNKNOWN, fx_status().state);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_UNKNOWN_STATE, kvm_ctrl_select(&C, 2, &op));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_UNKNOWN_STATE, kvm_ctrl_select(&C, 3, &op));
    TEST_ASSERT_FALSE(fx_status().has_last_op);
}

void test_select_busy_while_switching_or_tracking(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    fx_steps(2);
    uint32_t op = 77;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_select(&C, 3, &op));
    TEST_ASSERT_EQUAL_UINT32(0, op);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_select(&C, 1, &op)); /* 同じ目標でも busy(idle ではない) */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_select(&C, 2, &op)); /* 現在のチャンネルでも、idle でなければ already_selected ではない */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, &op));
    TEST_ASSERT_EQUAL_UINT32(1, fx_status().last_op.op_id); /* 後続の拒否は記録を変えない */

    /* 手動切替に追従中 */
    fx_boot(2);
    mock_set_led_channel(&M, 4);
    fx_step();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_TRACKING, fx_status().state);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_select(&C, 1, &op));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_RESET, &op));
}

void test_select_timeout_reverts_a0a1_and_aborts(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    /* LED が変わらない */
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
    TEST_ASSERT_EQUAL_STRING("a1@70 S3:250@80 r3@230 a2@2230", fx_trace());
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT8(2, st.active_channel);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, st.last_op.result);
    TEST_ASSERT_EQUAL_UINT8(2, M.addr);
    expect_clean_board();
}

void test_select_overtaken_by_manual_switch_is_aborted(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    TEST_ASSERT_TRUE(run_until_logic_state(KVM_STATE_SWITCH_WAIT_LED, 100));
    mock_set_led_channel(&M, 3); /* 目標(1)でも現在(2)でもない */
    fx_step();
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_TRACKING, st.state);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_PENDING, st.last_op.result); /* 追従中はまだ決まらない */
    fx_steps(20);
    st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT8(3, st.active_channel);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, st.last_op.result);
    TEST_ASSERT_EQUAL_UINT8(3, M.addr);
    expect_clean_board();
}

void test_select_completes_when_idle_matches_target_after_tracking(void) {
    /* 追従を経ても、idle に戻ったとき active == 目標なら completed(結果は状態遷移だけで決まる) */
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 3, NULL));
    TEST_ASSERT_TRUE(run_until_logic_state(KVM_STATE_SWITCH_WAIT_LED, 100));
    mock_set_led_channel(&M, 4); /* 先に ch4 へ追い越される */
    fx_step();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_TRACKING, fx_status().state);
    mock_set_led_channel(&M, 3); /* その後 ch3 になる(手動切替) */
    fx_steps(30);
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT8(3, st.active_channel);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, st.last_op.result);
}

static uint64_t switch_duration_with_sync(kvm_video_sync_state_t sync) {
    fx_boot(2);
    mock_set_sync(&M, sync);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    fx_complete_switch(1);
    return T;
}

void test_video_sync_is_wired_into_switch_completion(void) {
    uint64_t t_locked = switch_duration_with_sync(KVM_VIDEO_SYNC_LOCKED);
    uint64_t t_nosig = switch_duration_with_sync(KVM_VIDEO_SYNC_NO_SIGNAL);
    uint64_t t_unknown = switch_duration_with_sync(KVM_VIDEO_SYNC_UNKNOWN);
    /* NO_SIGNAL は settle(500 ms)後、UNKNOWN はタイムアウト(3000 ms)後に idle へ戻る */
    TEST_ASSERT_TRUE_MESSAGE(t_nosig >= t_locked + 400 && t_nosig <= t_locked + 700, "NO_SIGNAL settle not wired");
    TEST_ASSERT_TRUE_MESSAGE(t_unknown >= t_locked + 2800, "UNKNOWN timeout not wired");
}

void test_manual_switch_is_tracked_with_no_output_lines(void) {
    fx_boot(1);
    mock_set_led_channel(&M, 3);
    fx_step();
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_TRACKING, st.state);
    TEST_ASSERT_EQUAL_UINT8(1, st.active_channel); /* 追従中は元のまま */
    TEST_ASSERT_TRUE(st.atx_locked);
    fx_steps(10);
    st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT8(3, st.active_channel);
    TEST_ASSERT_FALSE(st.has_last_op); /* 受理した操作ではない */
    TEST_ASSERT_EQUAL_STRING("a3@120", fx_trace()); /* A0/A1 だけが全線 High で追従する(LED が安定した 50 ms 後) */
    TEST_ASSERT_EQUAL_UINT8(3, M.addr);
    expect_clean_board();
}

/* ====================================================================================================
 * ATX
 * ================================================================================================== */

typedef struct {
    kvm_ctrl_power_action_t action;
    kvm_ctrl_op_kind_t kind;
    kvm_board_line_t line;
    const char *trace;
} atx_case_t;

void test_power_actions_use_right_line_max_ms_and_order(void) {
    const atx_case_t cases[] = {
        { KVM_CTRL_POWER_PRESS, KVM_CTRL_OP_POWER_PRESS, KVM_BOARD_LINE_POWER,       "S0:300@80 r0@280" },
        { KVM_CTRL_POWER_RESET, KVM_CTRL_OP_POWER_RESET, KVM_BOARD_LINE_RESET,       "S2:200@80 r2@180" },
        { KVM_CTRL_POWER_FORCE, KVM_CTRL_OP_POWER_FORCE, KVM_BOARD_LINE_POWER_FORCE, "S1:4600@80 r1@4580" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fx_boot(2);
        uint32_t op = 0;
        TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, cases[i].action, &op));
        TEST_ASSERT_EQUAL_UINT32(1, op);
        TEST_ASSERT_EQUAL_size_t(0, M.log_len);
        kvm_ctrl_status_t st = fx_status();
        TEST_ASSERT_EQUAL(cases[i].kind, st.last_op.kind);
        TEST_ASSERT_EQUAL_UINT8(2, st.last_op.channel);
        TEST_ASSERT_EQUAL(KVM_CTRL_OP_PENDING, st.last_op.result);

        fx_step(); /* 70 ms: 準備。まだ何も出さない */
        TEST_ASSERT_EQUAL_STRING("", fx_trace());
        TEST_ASSERT_EQUAL(KVM_CTRL_STATE_ATX, fx_status().state);
        fx_step(); /* 80 ms: パルス開始 */
        TEST_ASSERT_TRUE(mock_line_active(&M, cases[i].line));
        TEST_ASSERT_EQUAL(1, mock_active_count(&M));
        TEST_ASSERT_EQUAL(KVM_CTRL_STATE_ATX, fx_status().state);
        TEST_ASSERT_EQUAL(KVM_CTRL_OP_PENDING, fx_status().last_op.result);
        TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
        TEST_ASSERT_EQUAL_STRING(cases[i].trace, fx_trace());
        st = fx_status();
        TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, st.last_op.result);
        TEST_ASSERT_FALSE(st.atx_anomaly);
        TEST_ASSERT_EQUAL_UINT8(2, st.active_channel);
        expect_clean_board();
        TEST_ASSERT_EQUAL_UINT8(2, M.addr); /* ATX では A0/A1 は変わらない */
    }
}

void test_each_action_uses_its_own_configured_pulse_length(void) {
    /* 既定値では偶然同じになる組があるので、互いに違う値にして、取り違えを見つける */
    fx_reset(1);
    CFG.press_ms = 123;
    CFG.reset_ms = 45;
    CFG.force_ms = 3210;
    CFG.atx_min_interval_ms = 0;
    mock_set_led_channel(&M, 2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    fx_tick(0);
    fx_steps(6);
    const kvm_ctrl_power_action_t acts[] = { KVM_CTRL_POWER_PRESS, KVM_CTRL_POWER_RESET, KVM_CTRL_POWER_FORCE };
    const uint32_t pulse[] = { 123, 45, 3210 };
    const int lines[] = { KVM_BOARD_LINE_POWER, KVM_BOARD_LINE_RESET, KVM_BOARD_LINE_POWER_FORCE };
    for (int i = 0; i < 3; i++) {
        mock_reset_calls(&M);
        TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, acts[i], NULL));
        TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 3000));
        int a = fx_find(MOCK_FN_ASSERT_LINE, 0);
        int r = fx_find(MOCK_FN_RELEASE_LINE, 0);
        TEST_ASSERT_TRUE(a >= 0 && r >= 0);
        TEST_ASSERT_EQUAL_INT(lines[i], M.log[a].arg);
        TEST_ASSERT_EQUAL_UINT32(pulse[i] + 100, M.log[a].max_ms);
        /* ソフトの解除は、パルス長以上、パルス長 + tick 1 回分未満 */
        uint64_t held = M.log[r].at_ms - M.log[a].at_ms;
        TEST_ASSERT_TRUE_MESSAGE(held >= pulse[i] && held < pulse[i] + 10, "pulse length");
    }
}

void test_power_does_not_pulse_longer_than_configured(void) {
    /* ソフトの解除はパルス長ちょうど(tick 刻みの範囲)。ボードのタイマーに頼らない */
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    fx_steps(2);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, M.auto_releases, "pulse started");
    fx_steps(19);
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_POWER)); /* 80 + 190 = 270 ms: まだ有効 */
    fx_step();
    TEST_ASSERT_FALSE(mock_line_active(&M, KVM_BOARD_LINE_POWER)); /* 280 ms: 解除 */
    TEST_ASSERT_EQUAL_UINT(0, M.auto_releases); /* ボードのタイマーではなく、ソフトが解除した */
}

void test_assert_max_ms_is_capped_at_hard_max(void) {
    /* パルス長が上限ちょうどのとき、余裕を足しても kvm_logic のハード上限を超えない */
    fx_reset(1);
    CFG.press_ms = KVM_ATX_HARD_MAX_POWER_MS;
    CFG.reset_ms = KVM_ATX_HARD_MAX_RESET_MS;
    CFG.force_ms = KVM_ATX_HARD_MAX_FORCE_MS;
    CFG.atx_min_interval_ms = 0;
    mock_set_led_channel(&M, 2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    fx_tick(0);
    fx_steps(6);
    const kvm_ctrl_power_action_t acts[] = { KVM_CTRL_POWER_PRESS, KVM_CTRL_POWER_RESET, KVM_CTRL_POWER_FORCE };
    const uint32_t expect[] = { KVM_ATX_HARD_MAX_POWER_MS, KVM_ATX_HARD_MAX_RESET_MS, KVM_ATX_HARD_MAX_FORCE_MS };
    for (int i = 0; i < 3; i++) {
        mock_reset_calls(&M);
        TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, acts[i], NULL));
        TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 3000));
        int idx = fx_find(MOCK_FN_ASSERT_LINE, 0);
        TEST_ASSERT_TRUE(idx >= 0);
        TEST_ASSERT_EQUAL_UINT32(expect[i], M.log[idx].max_ms);
        TEST_ASSERT_EQUAL_UINT(1, mock_calls(&M, MOCK_FN_ASSERT_LINE));
    }
    expect_clean_board();
}

void test_assert_max_ms_does_not_overflow_with_huge_margin(void) {
    fx_reset(1);
    CFG.assert_margin_ms = UINT32_MAX;
    mock_set_led_channel(&M, 2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    fx_tick(0);
    fx_steps(6);
    mock_reset_calls(&M);
    /* 切替(KVM)は上限の定数が無いので、飽和する(0 や小さい値に回り込まない) */
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    fx_steps(3);
    int idx = fx_find(MOCK_FN_ASSERT_LINE, 0);
    TEST_ASSERT_TRUE(idx >= 0);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, M.log[idx].max_ms);
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_KVM)); /* モックのクランプ(上限)の範囲で有効 */
}

void test_assert_max_ms_for_kvm_is_pulse_plus_margin(void) {
    fx_reset(1);
    CFG.assert_margin_ms = 1;
    mock_set_led_channel(&M, 2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    fx_tick(0);
    fx_steps(6);
    mock_reset_calls(&M);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    fx_steps(3);
    TEST_ASSERT_EQUAL_STRING("a1@70 S3:151@80", fx_trace());
}

void test_power_anomaly_during_pulse_releases_immediately(void) {
    const uint8_t leds[] = {3, 0, 0xFF}; /* 別のチャンネル / 無点灯 / 複数点灯(0x03 を使う) */
    for (size_t i = 0; i < sizeof(leds); i++) {
        fx_boot(2);
        TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_FORCE, NULL));
        fx_steps(2); /* 80 ms: パルス中 */
        TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_POWER_FORCE));
        fx_step();   /* 90 ms */
        if (leds[i] == 0xFF) mock_set_led_multi(&M); else mock_set_led_channel(&M, leds[i]);
        fx_step();   /* 100 ms: 変化を検出した tick で、同じ tick の中で release_line */
        TEST_ASSERT_FALSE(mock_line_active(&M, KVM_BOARD_LINE_POWER_FORCE));
        TEST_ASSERT_EQUAL_STRING("S1:4600@80 r1@100", fx_trace());
        kvm_ctrl_status_t st = fx_status();
        TEST_ASSERT_TRUE(st.atx_anomaly);
        TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, st.last_op.result);
        TEST_ASSERT_EQUAL(leds[i] == 3 ? KVM_CTRL_STATE_TRACKING : KVM_CTRL_STATE_UNKNOWN, st.state);
        TEST_ASSERT_TRUE(st.atx_locked);
        /* LED が元に戻っても、同じ操作は再開しない */
        mock_set_led_channel(&M, 2);
        fx_steps(300);
        TEST_ASSERT_EQUAL_STRING("S1:4600@80 r1@100", fx_trace());
        expect_clean_board();
    }
}

void test_power_anomaly_before_assert_never_asserts(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    fx_step(); /* 70 ms: 準備 */
    mock_set_led_channel(&M, 3);
    fx_steps(40);
    TEST_ASSERT_EQUAL_STRING("a3@130", fx_trace()); /* 追従のための A0/A1 だけ。ATX の線は一度も出ない */
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_ASSERT_LINE));
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, fx_status().last_op.result);
    TEST_ASSERT_TRUE(fx_status().atx_anomaly);
}

void test_anomaly_flag_clears_when_next_pulse_starts(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_RESET, NULL));
    fx_steps(3);
    mock_set_led_channel(&M, 3);
    fx_step();
    TEST_ASSERT_TRUE(fx_status().atx_anomaly);
    mock_set_led_channel(&M, 2);
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 300));
    TEST_ASSERT_TRUE(fx_status().atx_anomaly); /* 次の ATX パルスの開始まで残る */
    fx_tick(T + 5000); /* クールダウンを越える */
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_RESET, NULL));
    fx_step();
    TEST_ASSERT_TRUE(fx_status().atx_anomaly); /* 準備中はまだ残っている */
    fx_step();
    TEST_ASSERT_FALSE(fx_status().atx_anomaly); /* パルス開始で消える */
}

void test_power_with_anomaly_flag_at_the_end_is_not_completed(void) {
    /* 「atx_anomaly なしでパルスが終われば completed」: フラグが立ったまま終わったら completed にしない */
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    fx_steps(3);
    C.logic.current_outputs.atx_anomaly_detected = true;
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, fx_status().last_op.result);
    TEST_ASSERT_EQUAL_STRING("S0:300@80 r0@280", fx_trace()); /* 線の出力には影響しない */
}

void test_power_channel_mismatch_outputs_nothing_and_does_not_consume_cooldown(void) {
    fx_boot(2);
    uint32_t op = 77;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_CHANNEL_MISMATCH, kvm_ctrl_power(&C, 1, KVM_CTRL_POWER_PRESS, &op));
    TEST_ASSERT_EQUAL_UINT32(0, op);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_CHANNEL_MISMATCH, kvm_ctrl_power(&C, 4, KVM_CTRL_POWER_FORCE, &op));
    fx_steps(40);
    TEST_ASSERT_EQUAL_STRING("", fx_trace());
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_FALSE(st.has_last_op);
    TEST_ASSERT_EQUAL_UINT32(0, st.cooldown_ms);
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, st.state);
    /* 拒否は何も消費しない: 正しい channel は直ちに受理され、op_id は 1 */
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, &op));
    TEST_ASSERT_EQUAL_UINT32(1, op);
}

void test_power_rejects_invalid_parameters(void) {
    fx_boot(2);
    uint32_t op = 77;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_power(&C, 0, KVM_CTRL_POWER_PRESS, &op));
    TEST_ASSERT_EQUAL_UINT32(0, op);
    op = 77;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_power(&C, 5, KVM_CTRL_POWER_PRESS, &op));
    TEST_ASSERT_EQUAL_UINT32(0, op);
    op = 77;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_power(&C, 2, (kvm_ctrl_power_action_t)3, &op));
    TEST_ASSERT_EQUAL_UINT32(0, op);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_power(&C, 2, (kvm_ctrl_power_action_t)-1, &op));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_power(&C, 2, (kvm_ctrl_power_action_t)255, &op));
    TEST_ASSERT_FALSE(fx_status().has_last_op);
    TEST_ASSERT_EQUAL_UINT32(0, fx_status().cooldown_ms);
}

void test_atx_op_is_aborted_if_logic_leaves_atx_states_without_anomaly(void) {
    /* 通常は anomaly が立つが、立たないまま ATX の状態を出たら、completed にしない(中断) */
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    C.logic.state = KVM_STATE_SWITCH_WAIT_LED;
    C.logic.state_entered = false;
    fx_step();
    TEST_ASSERT_FALSE(fx_status().atx_anomaly);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, fx_status().last_op.result);
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_ASSERT_LINE));
}

void test_logic_invalid_param_maps_to_invalid_param(void) {
    fx_boot(2);
    C.config.press_ms = 0; /* 検証をすり抜けた不正な値: kvm_logic が拒否する */
    uint32_t op = 77;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, &op));
    TEST_ASSERT_EQUAL_UINT32(0, op);
    TEST_ASSERT_FALSE(fx_status().has_last_op);
    TEST_ASSERT_EQUAL_UINT32(0, fx_status().cooldown_ms);
}

void test_power_is_rejected_in_unknown_state(void) {
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    uint32_t op = 77;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_UNKNOWN_STATE, kvm_ctrl_power(&C, 1, KVM_CTRL_POWER_PRESS, &op));
    TEST_ASSERT_EQUAL_UINT32(0, op);
}

/* ---- 判定の順序: unknown -> busy -> channel_mismatch -> atx_locked -> cooldown -> 受理 ---- */

/* 電源 press を 1 回完了させた(クールダウン中の)idle ch2 の状態にする。受理は T=60 */
static void boot_with_cooldown_active(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
    fx_steps(1);
    TEST_ASSERT_TRUE(fx_status().cooldown_ms > 0);
}

void test_order_unknown_beats_mismatch_and_cooldown(void) {
    boot_with_cooldown_active();
    mock_set_led_off(&M);
    fx_steps(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_UNKNOWN, fx_status().state);
    TEST_ASSERT_TRUE(fx_status().cooldown_ms > 0);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_UNKNOWN_STATE, kvm_ctrl_power(&C, 3, KVM_CTRL_POWER_PRESS, NULL)); /* mismatch でもある */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_UNKNOWN_STATE, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL)); /* cooldown でもある */
}

void test_order_busy_beats_mismatch_and_cooldown(void) {
    boot_with_cooldown_active();
    /* select はクールダウンの影響を受けない。切替中は busy で、クールダウンも残っている */
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    fx_steps(3);
    TEST_ASSERT_TRUE(fx_status().cooldown_ms > 0);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_power(&C, 3, KVM_CTRL_POWER_PRESS, NULL)); /* mismatch でもある */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL)); /* cooldown でもある */
}

void test_order_mismatch_beats_locked_and_cooldown(void) {
    boot_with_cooldown_active();
    C.logic.current_outputs.atx_locked = true; /* idle のまま ATX ロックされた状態を作る */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_CHANNEL_MISMATCH, kvm_ctrl_power(&C, 3, KVM_CTRL_POWER_PRESS, NULL));
}

void test_order_locked_beats_cooldown(void) {
    boot_with_cooldown_active();
    C.logic.current_outputs.atx_locked = true;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_ATX_LOCKED, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL)); /* cooldown でもある */
    C.logic.current_outputs.atx_locked = false;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_COOLDOWN, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
}

void test_order_mismatch_beats_cooldown_without_tick_in_between(void) {
    boot_with_cooldown_active();
    /* 別の PC への操作を、時間の問題として隠さない */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_CHANNEL_MISMATCH, kvm_ctrl_power(&C, 1, KVM_CTRL_POWER_RESET, NULL));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_COOLDOWN, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_RESET, NULL));
}

void test_order_invalid_params_beat_everything(void) {
    boot_with_cooldown_active();
    mock_set_led_off(&M);
    fx_steps(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_UNKNOWN, fx_status().state);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_power(&C, 9, KVM_CTRL_POWER_PRESS, NULL));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_power(&C, 2, (kvm_ctrl_power_action_t)7, NULL));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_select(&C, 0, NULL));
}

/* ---- クールダウン ---- */

void test_cooldown_boundary_is_exact(void) {
    boot_with_cooldown_active(); /* press を T=60 に受理。パルスは 80..280 ms で、280 ms の tick で終わる。いまは T=290 */
    const uint64_t ended_at = 280;
    TEST_ASSERT_EQUAL_UINT64(ended_at, C.atx_last_ms); /* 起点は受理(60)ではなく、終了の tick */
    TEST_ASSERT_EQUAL_UINT32(2000 - (uint32_t)(T - ended_at), fx_status().cooldown_ms);

    fx_tick(ended_at + 1999);
    TEST_ASSERT_EQUAL_UINT32(1, fx_status().cooldown_ms);
    uint32_t op = 77;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_COOLDOWN, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, &op));
    TEST_ASSERT_EQUAL_UINT32(0, op);

    fx_tick(ended_at + 2000);
    TEST_ASSERT_EQUAL_UINT32(0, fx_status().cooldown_ms);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, &op));
    TEST_ASSERT_EQUAL_UINT32(2, op);

    /* 実行中は数えない: cooldown_ms は 0 で、要求は busy */
    fx_step();
    TEST_ASSERT_EQUAL_UINT32(0, fx_status().cooldown_ms);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    /* 終わった tick から、また 2000 ms */
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 100));
    TEST_ASSERT_EQUAL_UINT32(2000, fx_status().cooldown_ms);
}

void test_cooldown_counts_from_the_end_of_a_force_pulse(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_FORCE, NULL)); /* T=60 に受理。パルスは 80..4580 ms */
    fx_steps(3);
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_ATX, fx_status().state);
    TEST_ASSERT_EQUAL_UINT32(0, fx_status().cooldown_ms); /* パルス中は、クールダウンの値を見せない */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
    TEST_ASSERT_EQUAL_UINT64(4580, T);
    TEST_ASSERT_EQUAL_UINT64(4580, C.atx_last_ms);
    TEST_ASSERT_EQUAL_UINT32(2000, fx_status().cooldown_ms);
    /* 受理(60 ms)から数えれば 2000 ms はとうに過ぎているが、終了(4580 ms)から数える */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_COOLDOWN, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    fx_tick(4580 + 1999);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_COOLDOWN, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    fx_tick(4580 + 2000);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
}

void test_cooldown_counts_from_an_aborted_op(void) {
    for (int variant = 0; variant < 2; variant++) {
        fx_boot(2);
        TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_RESET, NULL));
        if (variant == 0) fx_steps(3); /* パルス中(T=90) */
        else fx_step();                /* 準備中(T=70) */
        TEST_ASSERT_EQUAL_UINT32(0, fx_status().cooldown_ms);
        mock_set_led_channel(&M, 3);
        fx_step();                     /* 中断した tick */
        uint64_t aborted_at = T;
        TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, fx_status().last_op.result);
        TEST_ASSERT_EQUAL_UINT64(aborted_at, C.atx_last_ms);
        TEST_ASSERT_EQUAL_UINT32(2000, fx_status().cooldown_ms);
        mock_set_led_channel(&M, 2);
        TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 300));
        fx_tick(aborted_at + 1999);
        TEST_ASSERT_EQUAL(KVM_CTRL_ERR_COOLDOWN, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_RESET, NULL));
        fx_tick(aborted_at + 2000);
        TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_RESET, NULL));
    }
}

void test_cooldown_does_not_count_while_a_select_runs_or_after_it(void) {
    boot_with_cooldown_active();
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    uint64_t before = C.atx_last_ms;
    fx_complete_switch(1);
    TEST_ASSERT_EQUAL_UINT64(before, C.atx_last_ms); /* select の完了は、起点を動かさない */
}

void test_cooldown_rejection_leaves_logic_untouched(void) {
    boot_with_cooldown_active();
    kvm_logic_t before = C.logic;
    uint32_t next_op_before = C.next_op_id;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_COOLDOWN, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    TEST_ASSERT_EQUAL_MEMORY(&before, &C.logic, sizeof(before)); /* 試行が反映されていない */
    TEST_ASSERT_EQUAL_UINT32(next_op_before, C.next_op_id);
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, fx_status().state);
    fx_steps(30);
    TEST_ASSERT_EQUAL_UINT(1, mock_calls(&M, MOCK_FN_ASSERT_LINE)); /* 最初の press の 1 回だけ */
}

void test_cooldown_disabled_when_interval_is_zero(void) {
    fx_reset(1);
    CFG.atx_min_interval_ms = 0;
    mock_set_led_channel(&M, 2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    fx_tick(0);
    fx_steps(6);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_RESET, NULL));
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
    fx_step();
    TEST_ASSERT_EQUAL_UINT32(0, fx_status().cooldown_ms);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_RESET, NULL)); /* 直ちに再受理 */
}

void test_cooldown_does_not_apply_to_select_and_select_does_not_start_it(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    fx_complete_switch(1);
    TEST_ASSERT_EQUAL_UINT32(0, fx_status().cooldown_ms);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 1, KVM_CTRL_POWER_PRESS, NULL)); /* select のあとでも ATX は直ちに可能 */
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
    fx_step();
    TEST_ASSERT_TRUE(fx_status().cooldown_ms > 0);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 3, NULL)); /* ATX のクールダウン中でも select は可能 */
}

void test_cooldown_uses_time_of_last_tick(void) {
    /* 要求は now_ms を取らない。クールダウンは直近の tick の時刻で数える */
    boot_with_cooldown_active();
    fx_tick(280 + 1999);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_COOLDOWN, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_COOLDOWN, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL)); /* tick が無ければ変わらない */
    fx_tick(280 + 2000);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
}

/* ---- 操作の記録 ---- */

void test_op_ids_increment_and_only_last_op_is_kept(void) {
    fx_boot(2);
    uint32_t op = 0;
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 3, &op));
    TEST_ASSERT_EQUAL_UINT32(1, op);
    fx_complete_switch(3);
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL_UINT32(1, st.last_op.op_id);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_SELECT, st.last_op.kind);
    TEST_ASSERT_EQUAL_UINT8(3, st.last_op.channel);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, st.last_op.result);

    TEST_ASSERT_EQUAL(KVM_CTRL_ALREADY_SELECTED, kvm_ctrl_select(&C, 3, &op)); /* op_id は進まない */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_CHANNEL_MISMATCH, kvm_ctrl_power(&C, 1, KVM_CTRL_POWER_PRESS, &op));
    TEST_ASSERT_EQUAL_UINT32(1, fx_status().last_op.op_id);

    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 3, KVM_CTRL_POWER_RESET, &op));
    TEST_ASSERT_EQUAL_UINT32(2, op);
    st = fx_status(); /* 前の操作の結果は消える */
    TEST_ASSERT_EQUAL_UINT32(2, st.last_op.op_id);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_POWER_RESET, st.last_op.kind);
    TEST_ASSERT_EQUAL_UINT8(3, st.last_op.channel);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_PENDING, st.last_op.result);
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, fx_status().last_op.result);

    T += 3000;
    fx_tick(T);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 3, KVM_CTRL_POWER_FORCE, &op));
    TEST_ASSERT_EQUAL_UINT32(3, op);
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_POWER_FORCE, fx_status().last_op.kind);
}

void test_pending_op_is_always_resolved_before_next_accept(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
    TEST_ASSERT_NOT_EQUAL(KVM_CTRL_OP_PENDING, fx_status().last_op.result);
}

/* ---- 状態の報告 ---- */

void test_status_state_mapping_covers_every_logic_state(void) {
    fx_boot(2);
    const struct { kvm_logic_state_t s; kvm_ctrl_state_t expect; } map[] = {
        { KVM_STATE_UNKNOWN, KVM_CTRL_STATE_UNKNOWN },
        { KVM_STATE_IDLE, KVM_CTRL_STATE_IDLE },
        { KVM_STATE_SWITCH_WAIT_SETUP, KVM_CTRL_STATE_SWITCHING },
        { KVM_STATE_SWITCH_PULSE, KVM_CTRL_STATE_SWITCHING },
        { KVM_STATE_SWITCH_WAIT_LED, KVM_CTRL_STATE_SWITCHING },
        { KVM_STATE_SWITCH_WAIT_VIDEO, KVM_CTRL_STATE_SWITCHING },
        { KVM_STATE_SWITCH_REVERT_SETUP, KVM_CTRL_STATE_SWITCHING },
        { KVM_STATE_MANUAL_TRACK_WAIT_LED, KVM_CTRL_STATE_TRACKING },
        { KVM_STATE_MANUAL_TRACK_WAIT_VIDEO, KVM_CTRL_STATE_TRACKING },
        { KVM_STATE_ATX_WAIT_SETUP, KVM_CTRL_STATE_ATX },
        { KVM_STATE_ATX_PULSE, KVM_CTRL_STATE_ATX },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        C.logic.state = map[i].s;
        kvm_ctrl_status_t st = fx_status();
        TEST_ASSERT_EQUAL_MESSAGE(map[i].expect, st.state, "state mapping");
        /* active_channel は、unknown のときだけ 0 */
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(map[i].s == KVM_STATE_UNKNOWN ? 0 : 2, st.active_channel, "active_channel");
    }
}

void test_status_reports_lock_anomaly_cooldown_and_sync(void) {
    fx_boot(4);
    mock_set_sync(&M, KVM_VIDEO_SYNC_NO_SIGNAL);
    fx_step();
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_NO_SIGNAL, st.video_sync);
    TEST_ASSERT_FALSE(st.atx_locked);
    TEST_ASSERT_FALSE(st.atx_anomaly);
    TEST_ASSERT_EQUAL_UINT8(4, st.active_channel);

    mock_set_led_channel(&M, 1);
    fx_step();
    st = fx_status();
    TEST_ASSERT_TRUE(st.atx_locked);
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_TRACKING, st.state);
    TEST_ASSERT_EQUAL_UINT8(4, st.active_channel);

    C.logic.current_outputs.atx_anomaly_detected = true; /* 値がそのまま報告される */
    TEST_ASSERT_TRUE(fx_status().atx_anomaly);
}

void test_led_chatter_during_idle_never_outputs_and_blocks_requests(void) {
    fx_boot(2);
    mock_set_led_chatter(&M, 0x02, 0x04);
    fx_steps(50);
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_TRACKING, fx_status().state);
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_ASSERT_LINE));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_BUSY, kvm_ctrl_select(&C, 1, NULL));
    expect_clean_board();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_select_full_sequence_and_trace);
    RUN_TEST(test_select_rejects_invalid_channels);
    RUN_TEST(test_select_accepts_null_op_id);
    RUN_TEST(test_select_already_selected_does_nothing);
    RUN_TEST(test_select_unknown_state_is_distinguished);
    RUN_TEST(test_select_busy_while_switching_or_tracking);
    RUN_TEST(test_select_timeout_reverts_a0a1_and_aborts);
    RUN_TEST(test_select_overtaken_by_manual_switch_is_aborted);
    RUN_TEST(test_select_completes_when_idle_matches_target_after_tracking);
    RUN_TEST(test_video_sync_is_wired_into_switch_completion);
    RUN_TEST(test_manual_switch_is_tracked_with_no_output_lines);
    RUN_TEST(test_power_actions_use_right_line_max_ms_and_order);
    RUN_TEST(test_each_action_uses_its_own_configured_pulse_length);
    RUN_TEST(test_power_does_not_pulse_longer_than_configured);
    RUN_TEST(test_assert_max_ms_is_capped_at_hard_max);
    RUN_TEST(test_assert_max_ms_does_not_overflow_with_huge_margin);
    RUN_TEST(test_assert_max_ms_for_kvm_is_pulse_plus_margin);
    RUN_TEST(test_power_anomaly_during_pulse_releases_immediately);
    RUN_TEST(test_power_anomaly_before_assert_never_asserts);
    RUN_TEST(test_anomaly_flag_clears_when_next_pulse_starts);
    RUN_TEST(test_power_with_anomaly_flag_at_the_end_is_not_completed);
    RUN_TEST(test_power_channel_mismatch_outputs_nothing_and_does_not_consume_cooldown);
    RUN_TEST(test_power_rejects_invalid_parameters);
    RUN_TEST(test_atx_op_is_aborted_if_logic_leaves_atx_states_without_anomaly);
    RUN_TEST(test_logic_invalid_param_maps_to_invalid_param);
    RUN_TEST(test_power_is_rejected_in_unknown_state);
    RUN_TEST(test_order_unknown_beats_mismatch_and_cooldown);
    RUN_TEST(test_order_busy_beats_mismatch_and_cooldown);
    RUN_TEST(test_order_mismatch_beats_locked_and_cooldown);
    RUN_TEST(test_order_locked_beats_cooldown);
    RUN_TEST(test_order_mismatch_beats_cooldown_without_tick_in_between);
    RUN_TEST(test_order_invalid_params_beat_everything);
    RUN_TEST(test_cooldown_boundary_is_exact);
    RUN_TEST(test_cooldown_counts_from_the_end_of_a_force_pulse);
    RUN_TEST(test_cooldown_counts_from_an_aborted_op);
    RUN_TEST(test_cooldown_does_not_count_while_a_select_runs_or_after_it);
    RUN_TEST(test_cooldown_rejection_leaves_logic_untouched);
    RUN_TEST(test_cooldown_disabled_when_interval_is_zero);
    RUN_TEST(test_cooldown_does_not_apply_to_select_and_select_does_not_start_it);
    RUN_TEST(test_cooldown_uses_time_of_last_tick);
    RUN_TEST(test_op_ids_increment_and_only_last_op_is_kept);
    RUN_TEST(test_pending_op_is_always_resolved_before_next_accept);
    RUN_TEST(test_status_state_mapping_covers_every_logic_state);
    RUN_TEST(test_status_reports_lock_anomaly_cooldown_and_sync);
    RUN_TEST(test_led_chatter_during_idle_never_outputs_and_blocks_requests);
    return UNITY_END();
}

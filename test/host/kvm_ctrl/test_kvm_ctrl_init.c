/* kvm_ctrl: 初期化、設定の検証、LED の変換、起動直後の状態（INTERFACES.md 6.3 の「初期化」） */
#include "kc_common.h"

void setUp(void) { fx_reset(1); }
void tearDown(void) {}

/* ---- 初期化の手順 ---- */

void test_init_releases_all_then_sets_addr_1_even_when_board_already_at_1(void) {
    fx_reset(1);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    /* ボードの実際のアドレスが論理の初期出力と同じでも、set_addr は必ず呼ぶ */
    TEST_ASSERT_EQUAL_STRING("R@0 a1@0", fx_trace());
    TEST_ASSERT_EQUAL_size_t(2, M.log_len);
    TEST_ASSERT_EQUAL_UINT8(1, M.addr);
    TEST_ASSERT_EQUAL(0, mock_active_count(&M));
    TEST_ASSERT_FALSE(mock_violated(&M));
    TEST_ASSERT_FALSE(fx_status().fault);
}

void test_init_forces_addr_1_when_board_was_elsewhere(void) {
    fx_reset(3);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    TEST_ASSERT_EQUAL_UINT8(1, M.addr);
    TEST_ASSERT_EQUAL_STRING("R@0 a1@0", fx_trace());
}

void test_init_makes_no_other_hal_calls(void) {
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_ASSERT_LINE));
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_RELEASE_LINE));
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_READ_LED));
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_VIDEO_SYNC));
    TEST_ASSERT_EQUAL_UINT(1, mock_calls(&M, MOCK_FN_RELEASE_ALL));
    TEST_ASSERT_EQUAL_UINT(1, mock_calls(&M, MOCK_FN_SET_ADDR));
}

void test_init_release_all_failure_latches_fault_and_skips_set_addr(void) {
    mock_fail_nth(&M, MOCK_FN_RELEASE_ALL, 1);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_init());
    /* 全線 High を確認できないので、set_addr は呼ばない */
    TEST_ASSERT_EQUAL_STRING("R@0!", fx_trace());
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_TRUE(st.fault);
    TEST_ASSERT_EQUAL(KVM_CTRL_FAULT_INIT, st.fault_reason);
    /* ラッチして始まる: tick は release_all の再試行だけ */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_tick(0));
    TEST_ASSERT_EQUAL_STRING("R@0! R@0", fx_trace());
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_READ_LED));
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_SET_ADDR));
}

void test_init_set_addr_failure_latches_fault_and_releases_all(void) {
    mock_fail_nth(&M, MOCK_FN_SET_ADDR, 1);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_init());
    TEST_ASSERT_EQUAL_STRING("R@0 a1@0! R@0", fx_trace());
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_TRUE(st.fault);
    TEST_ASSERT_EQUAL(KVM_CTRL_FAULT_INIT, st.fault_reason);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_tick(0));
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_READ_LED));
}

void test_init_can_be_repeated_and_clears_previous_state(void) {
    mock_fail_nth(&M, MOCK_FN_SET_ADDR, 1);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_init());
    mock_clear_failures(&M);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init()); /* 再起動の模擬 */
    TEST_ASSERT_FALSE(fx_status().fault);
    TEST_ASSERT_EQUAL(KVM_CTRL_FAULT_NONE, fx_status().fault_reason);
}

/* ---- 引数・HAL・設定の検証 ---- */

void test_init_rejects_null_arguments(void) {
    kvm_board_hal_t hal = mock_hal(&M);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_init(NULL, &CFG, &hal));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_init(&C, NULL, &hal));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_init(&C, &CFG, NULL));
    TEST_ASSERT_EQUAL_size_t(0, M.log_len);
}

typedef enum { H_SET_ADDR, H_ASSERT, H_RELEASE_LINE, H_RELEASE_ALL, H_READ_LED, H_VIDEO_SYNC, H_N } hal_slot_t;

void test_init_rejects_incomplete_hal_without_touching_hardware(void) {
    for (int slot = 0; slot < H_N; slot++) {
        fx_reset(1);
        kvm_board_hal_t hal = mock_hal(&M);
        switch (slot) {
            case H_SET_ADDR:     hal.set_addr = NULL; break;
            case H_ASSERT:       hal.assert_line = NULL; break;
            case H_RELEASE_LINE: hal.release_line = NULL; break;
            case H_RELEASE_ALL:  hal.release_all = NULL; break;
            case H_READ_LED:     hal.read_led_mask = NULL; break;
            default:             hal.video_sync = NULL; break;
        }
        TEST_ASSERT_EQUAL_MESSAGE(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_init(&C, &CFG, &hal), "incomplete HAL accepted");
        TEST_ASSERT_EQUAL_size_t(0, M.log_len); /* HAL は一切呼ばない */
        /* 使えない状態: すべての API が NOT_READY */
        kvm_ctrl_status_t st;
        uint32_t op = 99;
        TEST_ASSERT_EQUAL(KVM_CTRL_ERR_NOT_READY, kvm_ctrl_tick(&C, 0));
        TEST_ASSERT_EQUAL(KVM_CTRL_ERR_NOT_READY, kvm_ctrl_select(&C, 1, &op));
        TEST_ASSERT_EQUAL_UINT32(0, op);
        TEST_ASSERT_EQUAL(KVM_CTRL_ERR_NOT_READY, kvm_ctrl_power(&C, 1, KVM_CTRL_POWER_PRESS, &op));
        TEST_ASSERT_EQUAL(KVM_CTRL_ERR_NOT_READY, kvm_ctrl_get_status(&C, &st));
    }
}


#define EXPECT_CFG_REJECTED(mutation_stmt, label)                                  \
    do {                                                                           \
        fx_reset(1);                                                                \
        mutation_stmt;                                                             \
        TEST_ASSERT_EQUAL_MESSAGE(KVM_CTRL_ERR_INVALID_PARAM, fx_init(), label);   \
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, M.log_len, label);                     \
        TEST_ASSERT_EQUAL_MESSAGE(KVM_CTRL_ERR_NOT_READY, kvm_ctrl_tick(&C, 0), label); \
    } while (0)

#define EXPECT_CFG_ACCEPTED(mutation_stmt, label)                                  \
    do {                                                                           \
        fx_reset(1);                                                                \
        mutation_stmt;                                                             \
        TEST_ASSERT_EQUAL_MESSAGE(KVM_CTRL_OK, fx_init(), label);                  \
    } while (0)

void test_init_validates_pulse_lengths_and_margin(void) {
    EXPECT_CFG_REJECTED(CFG.press_ms = 0, "press 0");
    EXPECT_CFG_REJECTED(CFG.reset_ms = 0, "reset 0");
    EXPECT_CFG_REJECTED(CFG.force_ms = 0, "force 0");
    EXPECT_CFG_REJECTED(CFG.assert_margin_ms = 0, "margin 0");

    /* 上限(ハード上限でクランプ済みの logic.max_*)ちょうどは受理、1 超は拒否 */
    EXPECT_CFG_ACCEPTED(CFG.press_ms = KVM_ATX_HARD_MAX_POWER_MS, "press at hard max");
    EXPECT_CFG_REJECTED(CFG.press_ms = KVM_ATX_HARD_MAX_POWER_MS + 1, "press over hard max");
    EXPECT_CFG_ACCEPTED(CFG.reset_ms = KVM_ATX_HARD_MAX_RESET_MS, "reset at hard max");
    EXPECT_CFG_REJECTED(CFG.reset_ms = KVM_ATX_HARD_MAX_RESET_MS + 1, "reset over hard max");
    EXPECT_CFG_ACCEPTED(CFG.force_ms = KVM_ATX_HARD_MAX_FORCE_MS, "force at hard max");
    EXPECT_CFG_REJECTED(CFG.force_ms = KVM_ATX_HARD_MAX_FORCE_MS + 1, "force over hard max");

    /* logic 側の上限がハード上限より大きくても、ハード上限にクランプされる */
    EXPECT_CFG_REJECTED(do { CFG.logic.max_power_pulse_ms = 9999; CFG.press_ms = 501; } while (0), "press over clamped max");
    EXPECT_CFG_ACCEPTED(do { CFG.logic.max_power_pulse_ms = 9999; CFG.press_ms = 500; } while (0), "press at clamped max");
    EXPECT_CFG_REJECTED(do { CFG.logic.max_reset_pulse_ms = 9999; CFG.reset_ms = 201; } while (0), "reset over clamped max");
    EXPECT_CFG_ACCEPTED(do { CFG.logic.max_reset_pulse_ms = 9999; CFG.reset_ms = 200; } while (0), "reset at clamped max");
    EXPECT_CFG_REJECTED(do { CFG.logic.max_force_power_pulse_ms = 99999; CFG.force_ms = 5001; } while (0), "force over clamped max");
    EXPECT_CFG_ACCEPTED(do { CFG.logic.max_force_power_pulse_ms = 99999; CFG.force_ms = 5000; } while (0), "force at clamped max");
    /* logic 側の上限がハード上限より小さければ、それが上限 */
    EXPECT_CFG_ACCEPTED(do { CFG.logic.max_power_pulse_ms = 300; CFG.press_ms = 300; } while (0), "press at configured max");
    EXPECT_CFG_REJECTED(do { CFG.logic.max_power_pulse_ms = 300; CFG.press_ms = 301; } while (0), "press over configured max");
    EXPECT_CFG_ACCEPTED(do { CFG.logic.max_reset_pulse_ms = 80; CFG.reset_ms = 80; } while (0), "reset at configured max");
    EXPECT_CFG_REJECTED(do { CFG.logic.max_reset_pulse_ms = 80; CFG.reset_ms = 81; } while (0), "reset over configured max");
    EXPECT_CFG_ACCEPTED(do { CFG.logic.max_force_power_pulse_ms = 3000; CFG.force_ms = 3000; } while (0), "force at configured max");
    EXPECT_CFG_REJECTED(do { CFG.logic.max_force_power_pulse_ms = 3000; CFG.force_ms = 3001; } while (0), "force over configured max");

    /* クールダウン 0 は「なし」として受理、最大値も受理 */
    EXPECT_CFG_ACCEPTED(CFG.atx_min_interval_ms = 0, "cooldown disabled");
    EXPECT_CFG_ACCEPTED(CFG.atx_min_interval_ms = UINT32_MAX, "cooldown max");
    EXPECT_CFG_ACCEPTED(CFG.assert_margin_ms = UINT32_MAX, "margin max");
}

void test_init_validates_embedded_logic_config(void) {
    EXPECT_CFG_REJECTED(CFG.logic.led_stable_time_ms = 0, "led_stable 0");
    EXPECT_CFG_REJECTED(CFG.logic.setup_time_ms = 0, "setup 0");
    EXPECT_CFG_REJECTED(CFG.logic.switch_kvm_pulse_time_ms = 0, "kvm pulse 0");
    EXPECT_CFG_REJECTED(CFG.logic.switch_led_wait_timeout_ms = 0, "led timeout 0");
}

void test_config_default_is_valid_and_within_limits(void) {
    kvm_ctrl_config_t d;
    memset(&d, 0xFF, sizeof(d));
    kvm_ctrl_config_default(&d);
    TEST_ASSERT_TRUE(d.press_ms >= 1 && d.press_ms <= d.logic.max_power_pulse_ms);
    TEST_ASSERT_TRUE(d.reset_ms >= 1 && d.reset_ms <= d.logic.max_reset_pulse_ms);
    TEST_ASSERT_TRUE(d.force_ms >= 1 && d.force_ms <= d.logic.max_force_power_pulse_ms);
    TEST_ASSERT_TRUE(d.press_ms < d.force_ms); /* 長押しは短押しより長い */
    TEST_ASSERT_TRUE(d.assert_margin_ms >= 1);
    TEST_ASSERT_TRUE(d.atx_min_interval_ms > 0); /* 既定はクールダウンあり（安全側） */
    kvm_ctrl_config_default(NULL); /* NULL は何もしない（落ちない） */
    fx_reset(1);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
}

/* ---- LED のビットマスク -> led_raw_state ---- */

void test_led_mask_to_raw(void) {
    TEST_ASSERT_EQUAL_UINT8(0, kvm_ctrl_led_mask_to_raw(0x00));
    TEST_ASSERT_EQUAL_UINT8(1, kvm_ctrl_led_mask_to_raw(0x01));
    TEST_ASSERT_EQUAL_UINT8(2, kvm_ctrl_led_mask_to_raw(0x02));
    TEST_ASSERT_EQUAL_UINT8(3, kvm_ctrl_led_mask_to_raw(0x04));
    TEST_ASSERT_EQUAL_UINT8(4, kvm_ctrl_led_mask_to_raw(0x08));
    /* 複数点灯 */
    for (int m = 0; m < 16; m++) {
        int bits = __builtin_popcount((unsigned)m);
        if (bits >= 2) TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xFF, kvm_ctrl_led_mask_to_raw((uint8_t)m), "multi");
    }
    /* ビット 4 以上は無視する */
    TEST_ASSERT_EQUAL_UINT8(0, kvm_ctrl_led_mask_to_raw(0x10));
    TEST_ASSERT_EQUAL_UINT8(0, kvm_ctrl_led_mask_to_raw(0xF0));
    TEST_ASSERT_EQUAL_UINT8(1, kvm_ctrl_led_mask_to_raw(0x11));
    TEST_ASSERT_EQUAL_UINT8(4, kvm_ctrl_led_mask_to_raw(0x88));
    TEST_ASSERT_EQUAL_UINT8(0xFF, kvm_ctrl_led_mask_to_raw(0xFF));
    TEST_ASSERT_EQUAL_UINT8(0xFF, kvm_ctrl_led_mask_to_raw(0x33));
}

/* ---- 起動直後の状態 ---- */

void test_status_right_after_init(void) {
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_UNKNOWN, st.state);
    TEST_ASSERT_EQUAL_UINT8(0, st.active_channel);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_UNKNOWN, st.video_sync);
    TEST_ASSERT_TRUE(st.atx_locked);
    TEST_ASSERT_FALSE(st.atx_anomaly);
    TEST_ASSERT_FALSE(st.fault);
    TEST_ASSERT_EQUAL(KVM_CTRL_FAULT_NONE, st.fault_reason);
    TEST_ASSERT_EQUAL_UINT32(0, st.cooldown_ms);
    TEST_ASSERT_FALSE(st.has_last_op);
    TEST_ASSERT_EQUAL_UINT32(0, st.last_op.op_id);
}

void test_get_status_rejects_bad_arguments(void) {
    kvm_ctrl_status_t st;
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_NOT_READY, kvm_ctrl_get_status(NULL, &st));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_get_status(&C, NULL));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_NOT_READY, kvm_ctrl_tick(NULL, 0));
    uint32_t op;
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_select(NULL, 1, &op));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_INVALID_PARAM, kvm_ctrl_power(NULL, 1, KVM_CTRL_POWER_PRESS, &op));
}

/* ---- LED の確定と A0/A1 ---- */

void test_boot_with_led_ch3_sets_addr_3_with_all_lines_high(void) {
    fx_reset(1);
    mock_set_led_channel(&M, 3);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    mock_reset_calls(&M);
    fx_tick(0);
    /* LED が安定するまでは unknown で、出力を動かさない */
    for (int i = 0; i < 4; i++) {
        fx_step();
        TEST_ASSERT_EQUAL(KVM_CTRL_STATE_UNKNOWN, fx_status().state);
    }
    TEST_ASSERT_EQUAL_STRING("", fx_trace());
    fx_step(); /* 50 ms */
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, fx_status().state);
    TEST_ASSERT_EQUAL_UINT8(3, fx_status().active_channel);
    TEST_ASSERT_EQUAL_STRING("a3@50", fx_trace());
    TEST_ASSERT_EQUAL_UINT8(3, M.addr);
    TEST_ASSERT_FALSE(mock_violated(&M));
}

void test_boot_with_led_ch1_does_not_set_addr_again(void) {
    fx_boot(1);
    TEST_ASSERT_EQUAL_UINT8(1, M.addr);
    TEST_ASSERT_EQUAL_STRING("", fx_trace()); /* fx_boot は init 後の記録を消している。差分適用で何も出ない */
}

void test_dark_led_stays_unknown_and_outputs_nothing(void) {
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    mock_reset_calls(&M);
    for (int i = 0; i < 30; i++) fx_tick((uint64_t)i * 10);
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_UNKNOWN, st.state);
    TEST_ASSERT_EQUAL_UINT8(0, st.active_channel);
    TEST_ASSERT_TRUE(st.atx_locked);
    TEST_ASSERT_EQUAL_STRING("", fx_trace());
}

void test_multi_lit_led_is_unknown(void) {
    fx_reset(1);
    mock_set_led_multi(&M);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    for (int i = 0; i < 30; i++) fx_tick((uint64_t)i * 10);
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_UNKNOWN, fx_status().state);
}

void test_chattering_led_never_settles(void) {
    fx_reset(1);
    mock_set_led_chatter(&M, 0x01, 0x02);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    mock_reset_calls(&M);
    for (int i = 0; i < 100; i++) fx_tick((uint64_t)i * 10);
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_UNKNOWN, fx_status().state);
    TEST_ASSERT_EQUAL_STRING("", fx_trace());
    TEST_ASSERT_FALSE(mock_violated(&M));
}

void test_led_mask_upper_bits_are_ignored_by_tick(void) {
    fx_reset(1);
    mock_set_led_mask(&M, 0x11); /* ch1 + ビット 4 */
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    fx_tick(0);
    fx_steps(6);
    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, fx_status().state);
    TEST_ASSERT_EQUAL_UINT8(1, fx_status().active_channel);
}

/* ---- 映像の同期状態 ---- */

void test_video_sync_is_read_every_tick_and_reported(void) {
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    mock_reset_calls(&M);
    mock_set_sync(&M, KVM_VIDEO_SYNC_LOCKED);
    fx_tick(0);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_LOCKED, fx_status().video_sync);
    mock_set_sync(&M, KVM_VIDEO_SYNC_NO_SIGNAL);
    fx_tick(10);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_NO_SIGNAL, fx_status().video_sync);
    mock_set_sync(&M, KVM_VIDEO_SYNC_UNKNOWN);
    fx_tick(20);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_UNKNOWN, fx_status().video_sync);
    /* 範囲外の値は UNKNOWN として扱う */
    mock_set_sync(&M, (kvm_video_sync_state_t)99);
    fx_tick(30);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_UNKNOWN, fx_status().video_sync);
    TEST_ASSERT_EQUAL_UINT(4, mock_calls(&M, MOCK_FN_VIDEO_SYNC));
    TEST_ASSERT_EQUAL_UINT(4, mock_calls(&M, MOCK_FN_READ_LED));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_releases_all_then_sets_addr_1_even_when_board_already_at_1);
    RUN_TEST(test_init_forces_addr_1_when_board_was_elsewhere);
    RUN_TEST(test_init_makes_no_other_hal_calls);
    RUN_TEST(test_init_release_all_failure_latches_fault_and_skips_set_addr);
    RUN_TEST(test_init_set_addr_failure_latches_fault_and_releases_all);
    RUN_TEST(test_init_can_be_repeated_and_clears_previous_state);
    RUN_TEST(test_init_rejects_null_arguments);
    RUN_TEST(test_init_rejects_incomplete_hal_without_touching_hardware);
    RUN_TEST(test_init_validates_pulse_lengths_and_margin);
    RUN_TEST(test_init_validates_embedded_logic_config);
    RUN_TEST(test_config_default_is_valid_and_within_limits);
    RUN_TEST(test_led_mask_to_raw);
    RUN_TEST(test_status_right_after_init);
    RUN_TEST(test_get_status_rejects_bad_arguments);
    RUN_TEST(test_boot_with_led_ch3_sets_addr_3_with_all_lines_high);
    RUN_TEST(test_boot_with_led_ch1_does_not_set_addr_again);
    RUN_TEST(test_dark_led_stays_unknown_and_outputs_nothing);
    RUN_TEST(test_multi_lit_led_is_unknown);
    RUN_TEST(test_chattering_led_never_settles);
    RUN_TEST(test_led_mask_upper_bits_are_ignored_by_tick);
    RUN_TEST(test_video_sync_is_read_every_tick_and_reported);
    return UNITY_END();
}

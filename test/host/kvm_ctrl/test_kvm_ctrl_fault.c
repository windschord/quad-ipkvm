/*
 * kvm_ctrl: フォールト(HAL の失敗、kvm_logic の出力の不整合、時刻の巻き戻り)。
 * 契約(INTERFACES.md 6.3): 即座に release_all -> フォールトをラッチ。ラッチ後の tick は kvm_logic_step の出力を
 * HAL に適用しない(set_addr も assert_line も呼ばない)。release_all の再試行だけを続ける。解除は再起動のみ。
 */
#include "kc_common.h"

void setUp(void) { fx_reset(1); }
void tearDown(void) {}

#define NO_LINE_VALUE KVM_BOARD_LINE_COUNT

/*
 * フォールトがラッチされ、以後は何も出力しないことを確認する。
 * LED を変化させながら ticks 回 tick して、assert_line / set_addr / release_line が 0 回、
 * release_all が tick ごとに 1 回であること。
 */
static void expect_latched_and_quiet(kvm_ctrl_fault_t reason, int ticks) {
    kvm_ctrl_status_t st = fx_status();
    TEST_ASSERT_TRUE(st.fault);
    TEST_ASSERT_EQUAL(reason, st.fault_reason);
    TEST_ASSERT_EQUAL(0, mock_active_count(&M));

    uint32_t op = 77;
    for (uint8_t ch = 1; ch <= 4; ch++) {
        TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, kvm_ctrl_select(&C, ch, &op));
        TEST_ASSERT_EQUAL_UINT32(0, op);
        TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, kvm_ctrl_power(&C, ch, KVM_CTRL_POWER_PRESS, &op));
        TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, kvm_ctrl_power(&C, ch, KVM_CTRL_POWER_RESET, &op));
        TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, kvm_ctrl_power(&C, ch, KVM_CTRL_POWER_FORCE, &op));
    }

    mock_reset_calls(&M);
    for (int i = 0; i < ticks; i++) {
        /* LED が回復しても(目標のチャンネルに落ち着いても)、出力は一切再開しない */
        mock_set_led_channel(&M, (uint8_t)(1 + (i / 25) % 4));
        TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mock_calls(&M, MOCK_FN_ASSERT_LINE), "assert_line after fault");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mock_calls(&M, MOCK_FN_SET_ADDR), "set_addr after fault");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mock_calls(&M, MOCK_FN_RELEASE_LINE), "release_line after fault");
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)ticks, mock_calls(&M, MOCK_FN_RELEASE_ALL), "release_all retry per tick");
    TEST_ASSERT_EQUAL(0, mock_active_count(&M));
    TEST_ASSERT_FALSE(mock_violated(&M));
    TEST_ASSERT_TRUE(fx_status().fault);
    TEST_ASSERT_EQUAL(reason, fx_status().fault_reason);
}

/* ---- set_addr の失敗 ---- */

void test_fault_set_addr_during_select(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    mock_fail_nth(&M, MOCK_FN_SET_ADDR, 1);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step()); /* 70 ms */
    TEST_ASSERT_EQUAL_STRING("a1@70! R@70", fx_trace()); /* 失敗の直後に release_all */
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, fx_status().last_op.result);
    /* 切替が進んでいたはずの状況(LED を目標に合わせる)でも、再開しない */
    mock_set_led_channel(&M, 1);
    expect_latched_and_quiet(KVM_CTRL_FAULT_SET_ADDR, 300);
}

void test_fault_set_addr_during_manual_tracking(void) {
    fx_boot(2);
    mock_set_led_channel(&M, 3);
    mock_fail_nth(&M, MOCK_FN_SET_ADDR, 1);
    int r = KVM_CTRL_OK;
    for (int i = 0; i < 20 && r == KVM_CTRL_OK; i++) r = fx_step();
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, r);
    TEST_ASSERT_EQUAL_STRING("a3@120! R@120", fx_trace());
    expect_latched_and_quiet(KVM_CTRL_FAULT_SET_ADDR, 100);
}

/* ---- assert_line の失敗 ---- */

void test_fault_assert_line_kvm(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    mock_fail_nth(&M, MOCK_FN_ASSERT_LINE, 1);
    fx_step();
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step()); /* 80 ms */
    TEST_ASSERT_EQUAL_STRING("a1@70 S3:250@80! R@80", fx_trace());
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, fx_status().last_op.result);
    expect_latched_and_quiet(KVM_CTRL_FAULT_ASSERT_LINE, 300);
}

void test_fault_assert_line_atx(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    mock_fail_nth(&M, MOCK_FN_ASSERT_LINE, 1);
    fx_step();
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    TEST_ASSERT_EQUAL_STRING("S0:300@80! R@80", fx_trace());
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, fx_status().last_op.result);
    expect_latched_and_quiet(KVM_CTRL_FAULT_ASSERT_LINE, 300);
}

/* ---- release_line の失敗 ---- */

void test_fault_release_line_at_pulse_end(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    fx_steps(2);
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_POWER));
    mock_fail_nth(&M, MOCK_FN_RELEASE_LINE, 1);
    kvm_ctrl_result_t r = KVM_CTRL_OK;
    for (int i = 0; i < 30 && r == KVM_CTRL_OK; i++) r = fx_step();
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, r);
    TEST_ASSERT_EQUAL_STRING("S0:300@80 r0@280! R@280", fx_trace());
    TEST_ASSERT_FALSE(mock_line_active(&M, KVM_BOARD_LINE_POWER)); /* release_all が確実に下げる */
    /* 出し終えたパルスでも、解除を確認できなかったので completed にはしない */
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, fx_status().last_op.result);
    expect_latched_and_quiet(KVM_CTRL_FAULT_RELEASE_LINE, 300);
}

void test_fault_release_line_at_anomaly(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_FORCE, NULL));
    fx_steps(3);
    mock_fail_nth(&M, MOCK_FN_RELEASE_LINE, 1);
    mock_set_led_channel(&M, 3);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    TEST_ASSERT_EQUAL_STRING("S1:4600@80 r1@100! R@100", fx_trace());
    TEST_ASSERT_FALSE(mock_line_active(&M, KVM_BOARD_LINE_POWER_FORCE));
    expect_latched_and_quiet(KVM_CTRL_FAULT_RELEASE_LINE, 300);
}

/* ---- read_led_mask の失敗 ---- */

void test_fault_read_led_while_idle(void) {
    fx_boot(2);
    mock_fail_nth(&M, MOCK_FN_READ_LED, 1);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    TEST_ASSERT_EQUAL_STRING("R@70", fx_trace());
    TEST_ASSERT_FALSE(fx_status().has_last_op);
    expect_latched_and_quiet(KVM_CTRL_FAULT_READ_LED, 100);
}

void test_fault_read_led_while_line_is_asserted(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    fx_steps(2);
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_KVM));
    mock_fail_nth(&M, MOCK_FN_READ_LED, 1);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    TEST_ASSERT_EQUAL_STRING("a1@70 S3:250@80 R@90", fx_trace()); /* LED を読めない tick で、有効な線を直ちに下げる */
    TEST_ASSERT_FALSE(mock_line_active(&M, KVM_BOARD_LINE_KVM));
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, fx_status().last_op.result);
    expect_latched_and_quiet(KVM_CTRL_FAULT_READ_LED, 300);
}

void test_fault_read_led_persistent(void) {
    fx_boot(2);
    mock_fail_all(&M, MOCK_FN_READ_LED, true);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    mock_fail_all(&M, MOCK_FN_READ_LED, false); /* 回復しても、ラッチは変わらない */
    expect_latched_and_quiet(KVM_CTRL_FAULT_READ_LED, 200);
}

/* ---- release_all の失敗と再試行 ---- */

void test_fault_release_all_keeps_being_retried_and_failure_does_not_unlatch(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    fx_steps(2);
    mock_reset_calls(&M);
    mock_fail_nth(&M, MOCK_FN_READ_LED, 1);
    mock_fail_all(&M, MOCK_FN_RELEASE_ALL, true); /* 緊急の release_all も失敗する */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    TEST_ASSERT_EQUAL_STRING("R@90!", fx_trace());
    TEST_ASSERT_TRUE(fx_status().fault);
    /* 失敗し続けても、毎 tick 再試行し、ラッチは変わらない */
    for (int i = 0; i < 10; i++) TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    TEST_ASSERT_EQUAL_UINT(11, mock_calls(&M, MOCK_FN_RELEASE_ALL));
    TEST_ASSERT_TRUE(fx_status().fault);
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_POWER)); /* 下げられなかった(ボードのタイマーが最後の砦) */
    mock_fail_all(&M, MOCK_FN_RELEASE_ALL, false);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step()); /* 再試行が成功して、線が下がる */
    TEST_ASSERT_FALSE(mock_line_active(&M, KVM_BOARD_LINE_POWER));
    expect_latched_and_quiet(KVM_CTRL_FAULT_READ_LED, 50);
}

void test_fault_keeps_first_reason(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 1, NULL));
    mock_fail_nth(&M, MOCK_FN_SET_ADDR, 1);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    mock_fail_all(&M, MOCK_FN_RELEASE_ALL, true);
    mock_fail_all(&M, MOCK_FN_READ_LED, true);
    fx_steps(5);
    TEST_ASSERT_EQUAL(KVM_CTRL_FAULT_SET_ADDR, fx_status().fault_reason);
}

void test_fault_does_not_change_completed_op(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_RESET, NULL));
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, fx_status().last_op.result);
    mock_fail_nth(&M, MOCK_FN_READ_LED, 1);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_COMPLETED, fx_status().last_op.result); /* 終わった操作は変えない */
}

/* ---- 時刻の巻き戻り ---- */

void test_time_rewind_latches_fault(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    fx_steps(3); /* T = 90, パルス中 */
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_POWER));
    mock_reset_calls(&M);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_tick(&C, 90)); /* 同じ時刻の繰り返しは許す */
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, kvm_ctrl_tick(&C, 89)); /* 1 ms の巻き戻りでもフォールト */
    TEST_ASSERT_FALSE(mock_line_active(&M, KVM_BOARD_LINE_POWER));
    TEST_ASSERT_EQUAL(KVM_CTRL_OP_ABORTED, fx_status().last_op.result);
    /* 巻き戻りの tick では LED も読まず、kvm_logic も進めない: 出力は release_all だけ */
    TEST_ASSERT_EQUAL_STRING("R@90", fx_trace());
    expect_latched_and_quiet(KVM_CTRL_FAULT_TIME_REWIND, 100);
}

void test_time_does_not_rewind_with_equal_or_large_jumps(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_tick(&C, 60));
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_tick(&C, 60));
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_tick(&C, 0x100000000ULL)); /* 32 bit を越える時刻 */
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_tick(&C, 0x100000001ULL));
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, kvm_ctrl_tick(&C, 0xFFFFFFFFULL)); /* 32 bit への切り詰めのような巻き戻り */
    TEST_ASSERT_EQUAL(KVM_CTRL_FAULT_TIME_REWIND, fx_status().fault_reason);
}

void test_first_tick_may_start_at_any_time(void) {
    fx_reset(1);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_tick(&C, 123456789ULL)); /* 初回は比較対象がない */
    TEST_ASSERT_FALSE(fx_status().fault);
}

/* ---- kvm_logic の出力の不整合(通常は起きない。起きたら安全側に倒す) ---- */

static void run_to_fault(void) {
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
}

void test_fault_when_logic_enables_two_lines(void) {
    fx_boot(2);
    C.logic.current_outputs.e_power_low = true;
    C.logic.current_outputs.e_reset_low = true;
    run_to_fault();
    TEST_ASSERT_EQUAL_STRING("R@70", fx_trace());
    expect_latched_and_quiet(KVM_CTRL_FAULT_LOGIC_OUTPUT, 50);

    fx_boot(2);
    C.logic.current_outputs.e_power_low = true;
    C.logic.current_outputs.e_kvm_low = true;
    run_to_fault();
    expect_latched_and_quiet(KVM_CTRL_FAULT_LOGIC_OUTPUT, 10);

    fx_boot(2);
    C.logic.current_outputs.e_reset_low = true;
    C.logic.current_outputs.e_kvm_low = true;
    run_to_fault();
    expect_latched_and_quiet(KVM_CTRL_FAULT_LOGIC_OUTPUT, 10);

    fx_boot(2);
    C.logic.current_outputs.e_reset_low = true;
    C.logic.current_outputs.e_kvm_low = true;
    C.logic.current_outputs.e_power_low = true;
    run_to_fault();
    expect_latched_and_quiet(KVM_CTRL_FAULT_LOGIC_OUTPUT, 10);
}

/* 受理中の操作に対応する線を含む 2 本以上でも、フォールト(対応する線だけを有効にして済ませない) */
void test_fault_when_logic_enables_two_lines_including_the_matching_one(void) {
    const kvm_ctrl_op_kind_t kinds[] = { KVM_CTRL_OP_SELECT, KVM_CTRL_OP_POWER_PRESS, KVM_CTRL_OP_POWER_RESET, KVM_CTRL_OP_POWER_FORCE };
    for (size_t k = 0; k < 4; k++) {
        for (int mask = 1; mask < 8; mask++) {
            if (__builtin_popcount((unsigned)mask) < 2) continue;
            fx_boot(2);
            C.has_last_op = true;
            C.last_op.op_id = 1;
            C.last_op.kind = kinds[k];
            C.last_op.channel = 2;
            C.last_op.result = KVM_CTRL_OP_PENDING;
            C.logic.current_outputs.e_kvm_low = (mask & 1) != 0;
            C.logic.current_outputs.e_reset_low = (mask & 2) != 0;
            C.logic.current_outputs.e_power_low = (mask & 4) != 0;
            TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
            TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mock_calls(&M, MOCK_FN_ASSERT_LINE), "line enabled although two were requested");
            TEST_ASSERT_EQUAL(KVM_CTRL_FAULT_LOGIC_OUTPUT, fx_status().fault_reason);
        }
    }
}

void test_fault_when_logic_a0a1_out_of_range(void) {
    const uint8_t bad[] = {0, 5, 255};
    for (size_t i = 0; i < sizeof(bad); i++) {
        fx_boot(2);
        C.logic.current_outputs.a0a1_channel = bad[i];
        mock_set_led_channel(&M, 3); /* kvm_logic が同じ tick で A0/A1 を直さないよう、LED を不安定にしておく */
        run_to_fault();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mock_calls(&M, MOCK_FN_SET_ADDR), "set_addr with invalid channel");
        expect_latched_and_quiet(KVM_CTRL_FAULT_LOGIC_OUTPUT, 10);
    }
}

/* 受理中の操作と e_*_low の組み合わせ。対応する操作があるときだけ、その線を有効にする */
void test_line_is_enabled_only_for_matching_pending_op(void) {
    typedef struct { int pending; /* -1 = なし */ int e; /* 0 kvm, 1 reset, 2 power */ int expect_line; /* -1 = フォールト */ } c_t;
    const c_t cases[] = {
        /* pending: 0 select, 1 press, 2 reset, 3 force */
        { -1, 0, -1 }, { -1, 1, -1 }, { -1, 2, -1 },
        {  0, 0, KVM_BOARD_LINE_KVM }, { 0, 1, -1 }, { 0, 2, -1 },
        {  1, 0, -1 }, { 1, 1, -1 }, { 1, 2, KVM_BOARD_LINE_POWER },
        {  2, 0, -1 }, { 2, 1, KVM_BOARD_LINE_RESET }, { 2, 2, -1 },
        {  3, 0, -1 }, { 3, 1, -1 }, { 3, 2, KVM_BOARD_LINE_POWER_FORCE },
    };
    const kvm_ctrl_op_kind_t kinds[] = { KVM_CTRL_OP_SELECT, KVM_CTRL_OP_POWER_PRESS, KVM_CTRL_OP_POWER_RESET, KVM_CTRL_OP_POWER_FORCE };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fx_boot(2);
        if (cases[i].pending >= 0) {
            C.has_last_op = true;
            C.last_op.op_id = 1;
            C.last_op.kind = kinds[cases[i].pending];
            C.last_op.channel = 2;
            C.last_op.result = KVM_CTRL_OP_PENDING;
        }
        C.logic.current_outputs.e_kvm_low = (cases[i].e == 0);
        C.logic.current_outputs.e_reset_low = (cases[i].e == 1);
        C.logic.current_outputs.e_power_low = (cases[i].e == 2);
        kvm_ctrl_result_t r = fx_step();
        if (cases[i].expect_line < 0) {
            TEST_ASSERT_EQUAL_MESSAGE(KVM_CTRL_ERR_FAULT, r, "inconsistent output must fault");
            TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mock_calls(&M, MOCK_FN_ASSERT_LINE), "no line for an inconsistent output");
            TEST_ASSERT_EQUAL(KVM_CTRL_FAULT_LOGIC_OUTPUT, fx_status().fault_reason);
        } else {
            TEST_ASSERT_EQUAL_MESSAGE(KVM_CTRL_OK, r, "consistent output must be applied");
            const mock_call_t *e = NULL;
            for (size_t k = 0; k < M.log_len; k++) if (M.log[k].fn == MOCK_FN_ASSERT_LINE) e = &M.log[k];
            TEST_ASSERT_NOT_NULL(e);
            TEST_ASSERT_EQUAL_INT(cases[i].expect_line, e->arg);
            TEST_ASSERT_EQUAL_UINT(1, mock_calls(&M, MOCK_FN_ASSERT_LINE));
        }
        TEST_ASSERT_FALSE(mock_violated(&M));
    }
}

void test_finished_op_does_not_justify_a_line(void) {
    /* 完了済み(pending でない)の操作では、線を有効にしない */
    fx_boot(2);
    C.has_last_op = true;
    C.last_op.op_id = 1;
    C.last_op.kind = KVM_CTRL_OP_POWER_PRESS;
    C.last_op.channel = 2;
    C.last_op.result = KVM_CTRL_OP_COMPLETED;
    C.logic.current_outputs.e_power_low = true;
    run_to_fault();
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_ASSERT_LINE));
}

void test_fault_when_addr_changes_while_a_line_is_asserted(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    fx_steps(2);
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_POWER));
    mock_reset_calls(&M);
    C.board_addr = 3; /* ボードのアドレスと論理の出力が食い違う(= 変更が必要になる)状況を作る */
    run_to_fault();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mock_calls(&M, MOCK_FN_SET_ADDR), "set_addr while a line is asserted");
    TEST_ASSERT_EQUAL_STRING("R@90", fx_trace());
    TEST_ASSERT_FALSE(mock_violated(&M));
    expect_latched_and_quiet(KVM_CTRL_FAULT_LOGIC_OUTPUT, 20);
}

void test_fault_when_addr_changes_in_the_same_tick_as_a_new_line(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_power(&C, 2, KVM_CTRL_POWER_PRESS, NULL));
    fx_step(); /* 70: 準備。次の tick(80)でパルスが始まる */
    C.board_addr = 3; /* 80 ms の tick で A0/A1 の変更と線の有効化が重なる */
    run_to_fault();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mock_calls(&M, MOCK_FN_SET_ADDR), "set_addr before enabling a line");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mock_calls(&M, MOCK_FN_ASSERT_LINE), "line enabled in the same tick as set_addr");
    expect_latched_and_quiet(KVM_CTRL_FAULT_LOGIC_OUTPUT, 20);
}

void test_board_addr_is_tracked_after_set_addr_success(void) {
    fx_boot(2);
    TEST_ASSERT_EQUAL_UINT8(2, C.board_addr);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 4, NULL));
    fx_step();
    TEST_ASSERT_EQUAL_UINT8(4, C.board_addr);
    /* 失敗したときは、ボードに設定済みとして扱わない */
    fx_boot(2);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, kvm_ctrl_select(&C, 4, NULL));
    mock_fail_nth(&M, MOCK_FN_SET_ADDR, 1);
    fx_step();
    TEST_ASSERT_EQUAL_UINT8(2, C.board_addr);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fault_set_addr_during_select);
    RUN_TEST(test_fault_set_addr_during_manual_tracking);
    RUN_TEST(test_fault_assert_line_kvm);
    RUN_TEST(test_fault_assert_line_atx);
    RUN_TEST(test_fault_release_line_at_pulse_end);
    RUN_TEST(test_fault_release_line_at_anomaly);
    RUN_TEST(test_fault_read_led_while_idle);
    RUN_TEST(test_fault_read_led_while_line_is_asserted);
    RUN_TEST(test_fault_read_led_persistent);
    RUN_TEST(test_fault_release_all_keeps_being_retried_and_failure_does_not_unlatch);
    RUN_TEST(test_fault_keeps_first_reason);
    RUN_TEST(test_fault_does_not_change_completed_op);
    RUN_TEST(test_time_rewind_latches_fault);
    RUN_TEST(test_time_does_not_rewind_with_equal_or_large_jumps);
    RUN_TEST(test_first_tick_may_start_at_any_time);
    RUN_TEST(test_fault_when_logic_enables_two_lines);
    RUN_TEST(test_fault_when_logic_enables_two_lines_including_the_matching_one);
    RUN_TEST(test_fault_when_logic_a0a1_out_of_range);
    RUN_TEST(test_line_is_enabled_only_for_matching_pending_op);
    RUN_TEST(test_finished_op_does_not_justify_a_line);
    RUN_TEST(test_fault_when_addr_changes_while_a_line_is_asserted);
    RUN_TEST(test_fault_when_addr_changes_in_the_same_tick_as_a_new_line);
    RUN_TEST(test_board_addr_is_tracked_after_set_addr_success);
    return UNITY_END();
}

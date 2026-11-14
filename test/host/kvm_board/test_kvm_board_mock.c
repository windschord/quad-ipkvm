/* モック HAL 自身のテスト。モックが「ボード側の義務」(INTERFACES.md 6.2)を再現していないと、
 * kvm_ctrl のテストが何も保証しなくなるので、モックの振る舞いを固定する。 */
#include "unity.h"
#include "kvm_board_mock.h"
#include <string.h>

static kvm_board_mock_t M;
static kvm_board_hal_t H;

void setUp(void) {
    mock_init(&M, 1);
    H = mock_hal(&M);
}
void tearDown(void) {}

void test_power_up_state_has_all_lines_high(void) {
    TEST_ASSERT_EQUAL(0, mock_active_count(&M));
    for (int l = 0; l < KVM_BOARD_LINE_COUNT; l++) TEST_ASSERT_FALSE(mock_line_active(&M, (kvm_board_line_t)l));
    TEST_ASSERT_EQUAL_UINT8(1, M.addr);
    TEST_ASSERT_FALSE(mock_violated(&M));
    TEST_ASSERT_EQUAL_size_t(0, M.log_len);
    mock_init(&M, 4);
    TEST_ASSERT_EQUAL_UINT8(4, M.addr);
}

void test_hal_has_all_functions(void) {
    TEST_ASSERT_NOT_NULL(H.set_addr);
    TEST_ASSERT_NOT_NULL(H.assert_line);
    TEST_ASSERT_NOT_NULL(H.release_line);
    TEST_ASSERT_NOT_NULL(H.release_all);
    TEST_ASSERT_NOT_NULL(H.read_led_mask);
    TEST_ASSERT_NOT_NULL(H.video_sync);
    TEST_ASSERT_TRUE(H.ctx == &M);
}

/* ---- 義務 2: 同時に 1 本まで(POWER と POWER_FORCE を含めて、どの組でも 2 本目は失敗) ---- */
void test_only_one_line_at_a_time_for_every_pair(void) {
    for (int a = 0; a < KVM_BOARD_LINE_COUNT; a++) {
        for (int b = 0; b < KVM_BOARD_LINE_COUNT; b++) {
            mock_init(&M, 1);
            TEST_ASSERT_EQUAL(0, H.assert_line(&M, (kvm_board_line_t)a, 100));
            TEST_ASSERT_FALSE(M.viol_second_assert);
            TEST_ASSERT_TRUE(H.assert_line(&M, (kvm_board_line_t)b, 100) < 0); /* a == b の再 assert も失敗 */
            TEST_ASSERT_TRUE(M.viol_second_assert);
            TEST_ASSERT_TRUE(mock_violated(&M));
            TEST_ASSERT_EQUAL(1, mock_active_count(&M));
            TEST_ASSERT_TRUE(mock_line_active(&M, (kvm_board_line_t)a));
            if (a != b) TEST_ASSERT_FALSE(mock_line_active(&M, (kvm_board_line_t)b));
        }
    }
}

void test_assert_is_allowed_again_after_release(void) {
    TEST_ASSERT_EQUAL(0, H.assert_line(&M, KVM_BOARD_LINE_RESET, 100));
    TEST_ASSERT_EQUAL(0, H.release_line(&M, KVM_BOARD_LINE_RESET));
    TEST_ASSERT_EQUAL(0, H.assert_line(&M, KVM_BOARD_LINE_POWER, 100));
    TEST_ASSERT_FALSE(mock_violated(&M));
    TEST_ASSERT_EQUAL(0, H.release_all(&M));
    TEST_ASSERT_EQUAL(0, H.assert_line(&M, KVM_BOARD_LINE_KVM, 100));
    TEST_ASSERT_FALSE(mock_violated(&M));
}

/* ---- 義務 4: set_addr は、どれかの線が有効なら失敗 ---- */
void test_set_addr_fails_when_any_line_is_active(void) {
    for (int l = 0; l < KVM_BOARD_LINE_COUNT; l++) {
        mock_init(&M, 1);
        TEST_ASSERT_EQUAL(0, H.assert_line(&M, (kvm_board_line_t)l, 100));
        TEST_ASSERT_TRUE(H.set_addr(&M, 3) < 0);
        TEST_ASSERT_TRUE(M.viol_set_addr_while_active);
        TEST_ASSERT_EQUAL_UINT8(1, M.addr); /* 変わらない */
        TEST_ASSERT_EQUAL(0, H.release_line(&M, (kvm_board_line_t)l));
        TEST_ASSERT_EQUAL(0, H.set_addr(&M, 3));
        TEST_ASSERT_EQUAL_UINT8(3, M.addr);
    }
}

void test_set_addr_rejects_out_of_range_channels(void) {
    TEST_ASSERT_TRUE(H.set_addr(&M, 0) < 0);
    TEST_ASSERT_TRUE(M.viol_bad_arg);
    mock_init(&M, 1);
    TEST_ASSERT_TRUE(H.set_addr(&M, 5) < 0);
    TEST_ASSERT_TRUE(M.viol_bad_arg);
    TEST_ASSERT_EQUAL_UINT8(1, M.addr);
    mock_init(&M, 1);
    for (uint8_t ch = 1; ch <= 4; ch++) {
        TEST_ASSERT_EQUAL(0, H.set_addr(&M, ch));
        TEST_ASSERT_EQUAL_UINT8(ch, M.addr);
    }
    TEST_ASSERT_FALSE(mock_violated(&M));
}

/* ---- 義務 3: 種別ごとの上限でクランプし、タイマーで自動解除 ---- */
void test_caps_are_per_kind_and_independent(void) {
    TEST_ASSERT_EQUAL_UINT32(MOCK_MAX_POWER_MS, mock_line_cap_ms(KVM_BOARD_LINE_POWER));
    TEST_ASSERT_EQUAL_UINT32(MOCK_MAX_POWER_FORCE_MS, mock_line_cap_ms(KVM_BOARD_LINE_POWER_FORCE));
    TEST_ASSERT_EQUAL_UINT32(MOCK_MAX_RESET_MS, mock_line_cap_ms(KVM_BOARD_LINE_RESET));
    TEST_ASSERT_EQUAL_UINT32(MOCK_MAX_KVM_MS, mock_line_cap_ms(KVM_BOARD_LINE_KVM));
    TEST_ASSERT_EQUAL_UINT32(0, mock_line_cap_ms(KVM_BOARD_LINE_COUNT));
    /* POWER と POWER_FORCE は別の上限 */
    TEST_ASSERT_TRUE(MOCK_MAX_POWER_FORCE_MS > MOCK_MAX_POWER_MS);
}

void test_assert_clamps_and_auto_releases_at_deadline(void) {
    const struct { kvm_board_line_t line; uint32_t cap; } k[] = {
        { KVM_BOARD_LINE_POWER, MOCK_MAX_POWER_MS }, { KVM_BOARD_LINE_POWER_FORCE, MOCK_MAX_POWER_FORCE_MS },
        { KVM_BOARD_LINE_RESET, MOCK_MAX_RESET_MS }, { KVM_BOARD_LINE_KVM, MOCK_MAX_KVM_MS },
    };
    const uint32_t requests[] = { 0, 1, 50, 0xFFFFFFFFu };
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < 4; j++) {
            mock_init(&M, 1);
            mock_advance(&M, 1000);
            uint32_t req = requests[j];
            uint32_t eff = (req == 0 || req > k[i].cap) ? k[i].cap : req;
            TEST_ASSERT_EQUAL(0, H.assert_line(&M, k[i].line, req));
            TEST_ASSERT_EQUAL_UINT32(eff, M.log[0].effective_ms);
            TEST_ASSERT_EQUAL_UINT32(req, M.log[0].max_ms);
            TEST_ASSERT_EQUAL_UINT64(1000 + eff, M.deadline_ms[k[i].line]);
            mock_advance(&M, 1000 + eff - 1);
            TEST_ASSERT_TRUE(mock_line_active(&M, k[i].line));
            TEST_ASSERT_EQUAL_UINT(0, M.auto_releases);
            mock_advance(&M, 1000 + eff);
            TEST_ASSERT_FALSE(mock_line_active(&M, k[i].line));
            TEST_ASSERT_EQUAL_UINT(1, M.auto_releases);
            TEST_ASSERT_EQUAL(0, H.assert_line(&M, k[i].line, 10)); /* 解除後は再び有効にできる */
        }
    }
}

void test_zero_max_ms_is_flagged_but_clamped(void) {
    TEST_ASSERT_EQUAL(0, H.assert_line(&M, KVM_BOARD_LINE_POWER, 0));
    TEST_ASSERT_TRUE(M.viol_zero_max_ms);
    TEST_ASSERT_TRUE(mock_violated(&M));
}

void test_advance_never_goes_backwards(void) {
    mock_advance(&M, 500);
    mock_advance(&M, 100);
    TEST_ASSERT_EQUAL_UINT64(500, M.now_ms);
    TEST_ASSERT_EQUAL(0, H.assert_line(&M, KVM_BOARD_LINE_RESET, 100));
    TEST_ASSERT_EQUAL_UINT64(600, M.deadline_ms[KVM_BOARD_LINE_RESET]);
}

/* ---- 解除 ---- */
void test_release_is_idempotent_and_selective(void) {
    TEST_ASSERT_EQUAL(0, H.release_line(&M, KVM_BOARD_LINE_RESET)); /* 有効でない線の解除も成功 */
    TEST_ASSERT_EQUAL(0, H.assert_line(&M, KVM_BOARD_LINE_POWER, 100));
    TEST_ASSERT_EQUAL(0, H.release_line(&M, KVM_BOARD_LINE_RESET)); /* 別の線の解除は影響しない */
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_POWER));
    TEST_ASSERT_EQUAL(0, H.release_line(&M, KVM_BOARD_LINE_POWER));
    TEST_ASSERT_EQUAL(0, H.release_line(&M, KVM_BOARD_LINE_POWER));
    TEST_ASSERT_EQUAL(0, mock_active_count(&M));
    TEST_ASSERT_FALSE(mock_violated(&M));
}

void test_release_all_clears_everything(void) {
    TEST_ASSERT_EQUAL(0, H.assert_line(&M, KVM_BOARD_LINE_KVM, 100));
    TEST_ASSERT_EQUAL(0, H.release_all(&M));
    TEST_ASSERT_EQUAL(0, mock_active_count(&M));
}

void test_out_of_range_line_is_a_violation(void) {
    TEST_ASSERT_TRUE(H.assert_line(&M, KVM_BOARD_LINE_COUNT, 100) < 0);
    TEST_ASSERT_TRUE(M.viol_bad_arg);
    mock_init(&M, 1);
    TEST_ASSERT_TRUE(H.assert_line(&M, (kvm_board_line_t)-1, 100) < 0);
    TEST_ASSERT_TRUE(M.viol_bad_arg);
    mock_init(&M, 1);
    TEST_ASSERT_TRUE(H.release_line(&M, KVM_BOARD_LINE_COUNT) < 0);
    TEST_ASSERT_TRUE(M.viol_bad_arg);
    TEST_ASSERT_FALSE(mock_line_active(&M, KVM_BOARD_LINE_COUNT));
    TEST_ASSERT_FALSE(mock_line_active(&M, (kvm_board_line_t)-1));
}

/* ---- 失敗の注入 ---- */
void test_fail_nth_fails_exactly_that_call_and_has_no_effect(void) {
    mock_fail_nth(&M, MOCK_FN_ASSERT_LINE, 2);
    TEST_ASSERT_EQUAL(0, H.assert_line(&M, KVM_BOARD_LINE_POWER, 100)); /* 1 回目は成功 */
    TEST_ASSERT_EQUAL(0, H.release_line(&M, KVM_BOARD_LINE_POWER));
    TEST_ASSERT_EQUAL(-1, H.assert_line(&M, KVM_BOARD_LINE_RESET, 100)); /* 2 回目が失敗 */
    TEST_ASSERT_FALSE(mock_line_active(&M, KVM_BOARD_LINE_RESET));      /* 副作用なし */
    TEST_ASSERT_FALSE(M.viol_second_assert);                             /* 注入された失敗は違反ではない */
    TEST_ASSERT_EQUAL(0, H.assert_line(&M, KVM_BOARD_LINE_RESET, 100)); /* 3 回目は成功(1 回だけ) */
    TEST_ASSERT_EQUAL_INT(-1, M.log[2].ret);
}

void test_fail_nth_one_means_next_call_and_is_per_function(void) {
    mock_fail_nth(&M, MOCK_FN_SET_ADDR, 1);
    TEST_ASSERT_EQUAL(0, H.release_all(&M)); /* 別の関数は影響を受けない */
    TEST_ASSERT_EQUAL(-1, H.set_addr(&M, 2));
    TEST_ASSERT_EQUAL_UINT8(1, M.addr);
    TEST_ASSERT_EQUAL(0, H.set_addr(&M, 2));
    TEST_ASSERT_EQUAL_UINT8(2, M.addr);
}

void test_fail_all_and_clear(void) {
    mock_fail_all(&M, MOCK_FN_RELEASE_LINE, true);
    TEST_ASSERT_EQUAL(0, H.assert_line(&M, KVM_BOARD_LINE_POWER, 100));
    TEST_ASSERT_EQUAL(-1, H.release_line(&M, KVM_BOARD_LINE_POWER));
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_POWER)); /* 解除に失敗したので有効のまま */
    TEST_ASSERT_EQUAL(-1, H.release_line(&M, KVM_BOARD_LINE_POWER));
    mock_fail_all(&M, MOCK_FN_RELEASE_LINE, false);
    TEST_ASSERT_EQUAL(0, H.release_line(&M, KVM_BOARD_LINE_POWER));
    mock_fail_nth(&M, MOCK_FN_RELEASE_ALL, 1);
    mock_fail_all(&M, MOCK_FN_READ_LED, true);
    mock_clear_failures(&M);
    uint8_t mask;
    TEST_ASSERT_EQUAL(0, H.release_all(&M));
    TEST_ASSERT_EQUAL(0, H.read_led_mask(&M, &mask));
}

void test_release_all_failure_leaves_lines(void) {
    TEST_ASSERT_EQUAL(0, H.assert_line(&M, KVM_BOARD_LINE_KVM, 100));
    mock_fail_nth(&M, MOCK_FN_RELEASE_ALL, 1);
    TEST_ASSERT_EQUAL(-1, H.release_all(&M));
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_KVM));
}

/* ---- LED と映像 ---- */
void test_led_simulation(void) {
    uint8_t mask = 0xAA;
    TEST_ASSERT_EQUAL(0, H.read_led_mask(&M, &mask));
    TEST_ASSERT_EQUAL_UINT8(0, mask);
    for (uint8_t ch = 1; ch <= 4; ch++) {
        mock_set_led_channel(&M, ch);
        TEST_ASSERT_EQUAL(0, H.read_led_mask(&M, &mask));
        TEST_ASSERT_EQUAL_UINT8(1u << (ch - 1), mask);
    }
    mock_set_led_channel(&M, 0);
    H.read_led_mask(&M, &mask);
    TEST_ASSERT_EQUAL_UINT8(0, mask);
    mock_set_led_channel(&M, 5);
    H.read_led_mask(&M, &mask);
    TEST_ASSERT_EQUAL_UINT8(0, mask);
    mock_set_led_multi(&M);
    H.read_led_mask(&M, &mask);
    TEST_ASSERT_EQUAL_UINT8(0x03, mask);
    mock_set_led_mask(&M, 0x0C);
    H.read_led_mask(&M, &mask);
    TEST_ASSERT_EQUAL_UINT8(0x0C, mask);
    mock_set_led_off(&M);
    H.read_led_mask(&M, &mask);
    TEST_ASSERT_EQUAL_UINT8(0, mask);
}

void test_led_chatter_alternates_every_read(void) {
    uint8_t mask;
    mock_set_led_chatter(&M, 0x01, 0x02);
    uint8_t expected[] = {1, 2, 1, 2, 1};
    for (size_t i = 0; i < sizeof(expected); i++) {
        H.read_led_mask(&M, &mask);
        TEST_ASSERT_EQUAL_UINT8(expected[i], mask);
    }
    mock_clear_led_chatter(&M);
    mock_set_led_channel(&M, 3);
    H.read_led_mask(&M, &mask);
    TEST_ASSERT_EQUAL_UINT8(4, mask);
}

void test_read_led_failure_and_null_output(void) {
    uint8_t mask = 0x55;
    mock_fail_nth(&M, MOCK_FN_READ_LED, 1);
    TEST_ASSERT_EQUAL(-1, H.read_led_mask(&M, &mask));
    TEST_ASSERT_EQUAL_UINT8(0x55, mask); /* 失敗時は出力を書かない */
    TEST_ASSERT_EQUAL(0, H.read_led_mask(&M, &mask));
    TEST_ASSERT_EQUAL(-1, H.read_led_mask(&M, NULL));
    TEST_ASSERT_TRUE(M.viol_bad_arg);
}

void test_video_sync_is_returned_and_logged(void) {
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_UNKNOWN, H.video_sync(&M));
    mock_set_sync(&M, KVM_VIDEO_SYNC_LOCKED);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_LOCKED, H.video_sync(&M));
    mock_set_sync(&M, KVM_VIDEO_SYNC_NO_SIGNAL);
    TEST_ASSERT_EQUAL(KVM_VIDEO_SYNC_NO_SIGNAL, H.video_sync(&M));
    TEST_ASSERT_EQUAL_UINT(3, mock_calls(&M, MOCK_FN_VIDEO_SYNC));
}

/* ---- 記録 ---- */
void test_log_records_order_arguments_results_and_time(void) {
    mock_advance(&M, 10);
    H.set_addr(&M, 2);
    mock_advance(&M, 20);
    H.assert_line(&M, KVM_BOARD_LINE_RESET, 150);
    H.assert_line(&M, KVM_BOARD_LINE_POWER, 150); /* 失敗(違反) */
    H.release_line(&M, KVM_BOARD_LINE_RESET);
    H.release_all(&M);
    TEST_ASSERT_EQUAL_size_t(5, M.log_len);
    TEST_ASSERT_EQUAL_INT(MOCK_FN_SET_ADDR, M.log[0].fn);
    TEST_ASSERT_EQUAL_INT(2, M.log[0].arg);
    TEST_ASSERT_EQUAL_UINT64(10, M.log[0].at_ms);
    TEST_ASSERT_EQUAL_INT(0, M.log[0].ret);
    TEST_ASSERT_EQUAL_INT(MOCK_FN_ASSERT_LINE, M.log[1].fn);
    TEST_ASSERT_EQUAL_INT(KVM_BOARD_LINE_RESET, M.log[1].arg);
    TEST_ASSERT_EQUAL_UINT32(150, M.log[1].max_ms);
    TEST_ASSERT_EQUAL_UINT32(150, M.log[1].effective_ms);
    TEST_ASSERT_EQUAL_UINT64(20, M.log[1].at_ms);
    TEST_ASSERT_EQUAL_INT(-1, M.log[2].ret);
    TEST_ASSERT_EQUAL_INT(MOCK_FN_RELEASE_LINE, M.log[3].fn);
    TEST_ASSERT_EQUAL_INT(MOCK_FN_RELEASE_ALL, M.log[4].fn);
    TEST_ASSERT_EQUAL_UINT(1, mock_calls(&M, MOCK_FN_SET_ADDR));
    TEST_ASSERT_EQUAL_UINT(2, mock_calls(&M, MOCK_FN_ASSERT_LINE));
    TEST_ASSERT_EQUAL_UINT(1, mock_count_since(&M, 2, MOCK_FN_ASSERT_LINE));
    TEST_ASSERT_EQUAL_UINT(0, mock_count_since(&M, 3, MOCK_FN_ASSERT_LINE));
    TEST_ASSERT_NOT_NULL(mock_log_at(&M, 4));
    TEST_ASSERT_NULL(mock_log_at(&M, 5));
}

void test_reset_calls_keeps_state_and_violations(void) {
    H.assert_line(&M, KVM_BOARD_LINE_POWER, 100);
    H.assert_line(&M, KVM_BOARD_LINE_RESET, 100); /* 違反 */
    mock_fail_all(&M, MOCK_FN_SET_ADDR, true);
    mock_reset_calls(&M);
    TEST_ASSERT_EQUAL_size_t(0, M.log_len);
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_ASSERT_LINE));
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_POWER));
    TEST_ASSERT_TRUE(M.viol_second_assert);
    TEST_ASSERT_TRUE(M.fail_all[MOCK_FN_SET_ADDR]);
}

void test_log_overflow_is_counted_not_crashing(void) {
    for (int i = 0; i < MOCK_LOG_MAX + 10; i++) H.release_all(&M);
    TEST_ASSERT_EQUAL_size_t(MOCK_LOG_MAX, M.log_len);
    TEST_ASSERT_EQUAL_size_t(10, M.log_dropped);
    TEST_ASSERT_EQUAL_UINT(MOCK_LOG_MAX + 10, mock_calls(&M, MOCK_FN_RELEASE_ALL));
    mock_fail_nth(&M, MOCK_FN_RELEASE_ALL, 1);
    TEST_ASSERT_EQUAL(-1, H.release_all(&M)); /* 記録があふれていても、注入は働く */
}

void test_init_clears_violations_failures_and_log(void) {
    H.assert_line(&M, KVM_BOARD_LINE_POWER, 100);
    H.assert_line(&M, KVM_BOARD_LINE_POWER, 100);
    mock_fail_all(&M, MOCK_FN_READ_LED, true);
    mock_fail_nth(&M, MOCK_FN_SET_ADDR, 3);
    mock_set_led_chatter(&M, 1, 2);
    mock_init(&M, 2);
    TEST_ASSERT_FALSE(mock_violated(&M));
    TEST_ASSERT_EQUAL_size_t(0, M.log_len);
    TEST_ASSERT_EQUAL(0, mock_active_count(&M));
    uint8_t mask;
    TEST_ASSERT_EQUAL(0, H.read_led_mask(&M, &mask));
    TEST_ASSERT_EQUAL_UINT8(0, mask);
    TEST_ASSERT_EQUAL(0, H.set_addr(&M, 1));
    TEST_ASSERT_EQUAL(0, H.set_addr(&M, 1));
    TEST_ASSERT_EQUAL(0, H.set_addr(&M, 1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_power_up_state_has_all_lines_high);
    RUN_TEST(test_hal_has_all_functions);
    RUN_TEST(test_only_one_line_at_a_time_for_every_pair);
    RUN_TEST(test_assert_is_allowed_again_after_release);
    RUN_TEST(test_set_addr_fails_when_any_line_is_active);
    RUN_TEST(test_set_addr_rejects_out_of_range_channels);
    RUN_TEST(test_caps_are_per_kind_and_independent);
    RUN_TEST(test_assert_clamps_and_auto_releases_at_deadline);
    RUN_TEST(test_zero_max_ms_is_flagged_but_clamped);
    RUN_TEST(test_advance_never_goes_backwards);
    RUN_TEST(test_release_is_idempotent_and_selective);
    RUN_TEST(test_release_all_clears_everything);
    RUN_TEST(test_out_of_range_line_is_a_violation);
    RUN_TEST(test_fail_nth_fails_exactly_that_call_and_has_no_effect);
    RUN_TEST(test_fail_nth_one_means_next_call_and_is_per_function);
    RUN_TEST(test_fail_all_and_clear);
    RUN_TEST(test_release_all_failure_leaves_lines);
    RUN_TEST(test_led_simulation);
    RUN_TEST(test_led_chatter_alternates_every_read);
    RUN_TEST(test_read_led_failure_and_null_output);
    RUN_TEST(test_video_sync_is_returned_and_logged);
    RUN_TEST(test_log_records_order_arguments_results_and_time);
    RUN_TEST(test_reset_calls_keeps_state_and_violations);
    RUN_TEST(test_log_overflow_is_counted_not_crashing);
    RUN_TEST(test_init_clears_violations_failures_and_log);
    return UNITY_END();
}

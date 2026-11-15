#include "../kvm_ctrl/kc_common.h"
#include "kvm_api.h"

/*
 * 実際の kvm_ctrl + モック HAL（test/host/kvm_board/）と kvm_api を結線した統合テスト。
 * backend は kvm_ctrl に直接つなぐ（ロックは回数だけ数える。単一スレッド）。
 * 確かめること: select -> 受理 -> tick で切替が完了 -> status に反映、power の mismatch は 409 で何も出ない、
 * フォールト後は 503、判定の順序（WEB_API.md 7.3）。
 */

static int g_locks, g_unlocks;
static bool g_held;
static bool g_ready = true;

static bool ib_ready(void *ctx) { (void)ctx; return g_ready; }
static bool ib_lock(void *ctx) { (void)ctx; g_locks++; TEST_ASSERT_FALSE(g_held); g_held = true; return true; }
static void ib_unlock(void *ctx) { (void)ctx; g_unlocks++; TEST_ASSERT_TRUE(g_held); g_held = false; }
static kvm_ctrl_result_t ib_status(void *ctx, kvm_ctrl_status_t *s) { (void)ctx; TEST_ASSERT_TRUE(g_held); return kvm_ctrl_get_status(&C, s); }
static kvm_ctrl_result_t ib_select(void *ctx, uint8_t ch, uint32_t *op) { (void)ctx; TEST_ASSERT_TRUE(g_held); return kvm_ctrl_select(&C, ch, op); }
static kvm_ctrl_result_t ib_power(void *ctx, uint8_t ch, kvm_ctrl_power_action_t a, uint32_t *op) {
    (void)ctx;
    TEST_ASSERT_TRUE(g_held);
    return kvm_ctrl_power(&C, ch, a, op);
}

static kvm_api_backend_t backend(void) {
    kvm_api_backend_t t;
    t.ctx = NULL;
    t.lock = ib_lock;
    t.unlock = ib_unlock;
    t.ready = ib_ready;
    t.get_status = ib_status;
    t.select = ib_select;
    t.power = ib_power;
    return t;
}

static kvm_api_response_t R;

static void api(kvm_api_endpoint_t ep, const char *body) {
    kvm_api_backend_t t = backend();
    kvm_api_handle(&t, ep, body, body ? strlen(body) : 0, &R);
    TEST_ASSERT_FALSE(g_held);
    TEST_ASSERT_EQUAL_INT(g_locks, g_unlocks);
}

static void rsp(int status, const char *body) {
    TEST_ASSERT_EQUAL_INT(status, R.status);
    TEST_ASSERT_EQUAL_STRING(body, R.body);
}

static void api_boot(uint8_t ch) {
    g_locks = g_unlocks = 0;
    g_held = false;
    g_ready = true;
    fx_boot(ch);
}

void setUp(void) { api_boot(1); }
void tearDown(void) {}

#define SEL(n) "{\"channel\":" #n "}"

/* LED をボタンのパルスの後で target に変えながら、idle まで進める */
static void finish_switch(uint8_t target) { fx_complete_switch(target); }

void test_status_after_boot(void) {
    api(KVM_API_EP_STATUS, NULL);
    rsp(200,
        "{\"state\":\"idle\",\"active_channel\":1,\"video_sync\":\"locked\",\"atx_locked\":false,\"atx_anomaly\":false,"
        "\"fault\":false,\"cooldown_ms\":0,\"last_op\":null}");
    TEST_ASSERT_FALSE(R.log);
}

void test_select_flow_to_completion_is_reflected_in_status(void) {
    api(KVM_API_EP_SELECT, SEL(3));
    rsp(202, "{\"result\":\"accepted\",\"channel\":3,\"op_id\":1}");
    TEST_ASSERT_EQUAL_STRING("accepted", R.log_reason);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, M.log_len, "the request itself must not touch the board");

    api(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"last_op\":{\"op_id\":1,\"kind\":\"select\",\"channel\":3,\"result\":\"pending\"}"));

    fx_step();
    api(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"state\":\"switching\""));
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"active_channel\":1,")); /* 切替中は元のまま */

    /* 切替中の再要求は busy。別の操作も busy */
    api(KVM_API_EP_SELECT, SEL(4));
    rsp(409, "{\"error\":\"busy\"}");
    api(KVM_API_EP_POWER_PRESS, SEL(1));
    rsp(409, "{\"error\":\"busy\"}");

    finish_switch(3);
    api(KVM_API_EP_STATUS, NULL);
    rsp(200,
        "{\"state\":\"idle\",\"active_channel\":3,\"video_sync\":\"locked\",\"atx_locked\":false,\"atx_anomaly\":false,"
        "\"fault\":false,\"cooldown_ms\":0,\"last_op\":{\"op_id\":1,\"kind\":\"select\",\"channel\":3,\"result\":\"completed\"}}");

    /* すでにそのチャンネル: 200、何も押さない、op_id は進まない */
    mock_reset_calls(&M);
    api(KVM_API_EP_SELECT, SEL(3));
    rsp(200, "{\"result\":\"already_selected\",\"channel\":3}");
    fx_steps(3);
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_ASSERT_LINE));
    api(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"op_id\":1,"));
}

void test_power_mismatch_is_409_and_outputs_nothing(void) {
    mock_reset_calls(&M);
    api(KVM_API_EP_POWER_PRESS, SEL(3));
    rsp(409, "{\"error\":\"channel_mismatch\",\"active_channel\":1}");
    TEST_ASSERT_EQUAL_STRING("channel_mismatch", R.log_reason);
    TEST_ASSERT_EQUAL_UINT8(3, R.log_channel);
    TEST_ASSERT_FALSE(R.has_retry_after);

    static const kvm_api_endpoint_t EPS[] = {KVM_API_EP_POWER_PRESS, KVM_API_EP_POWER_RESET, KVM_API_EP_POWER_FORCE};
    for (int e = 0; e < 3; e++) {
        for (int ch = 2; ch <= 4; ch++) {
            char body[24];
            snprintf(body, sizeof(body), "{\"channel\":%d}", ch);
            api(EPS[e], body);
            TEST_ASSERT_EQUAL_INT(409, R.status);
        }
    }
    fx_steps(40);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mock_calls(&M, MOCK_FN_ASSERT_LINE), "a mismatching power request must never pulse a line");
    TEST_ASSERT_FALSE(mock_violated(&M));
    api(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"last_op\":null"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, C.next_op_id, "no op was accepted");
}

void test_power_flow_busy_cooldown_and_retry_after(void) {
    api(KVM_API_EP_POWER_PRESS, SEL(1));
    rsp(202, "{\"result\":\"accepted\",\"channel\":1,\"action\":\"press\",\"op_id\":1}");
    api(KVM_API_EP_POWER_RESET, SEL(1));
    rsp(409, "{\"error\":\"busy\"}");

    api(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"state\":\"atx\""));
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"cooldown_ms\":0,")); /* 実行中は 0 */
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"result\":\"pending\""));

    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
    api(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"cooldown_ms\":2000,")); /* 終了の tick から数える */
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"kind\":\"power_press\",\"channel\":1,\"result\":\"completed\""));

    /* クールダウン中: 429 + Retry-After（残りを切り上げた秒）。判定の順序では mismatch が先 */
    api(KVM_API_EP_POWER_FORCE, SEL(1));
    rsp(429, "{\"error\":\"cooldown\"}");
    TEST_ASSERT_TRUE(R.has_retry_after);
    TEST_ASSERT_EQUAL_UINT32(2, R.retry_after_s);
    api(KVM_API_EP_POWER_FORCE, SEL(2));
    rsp(409, "{\"error\":\"channel_mismatch\",\"active_channel\":1}");

    fx_tick(T + 1001); /* 残り 999 ms */
    api(KVM_API_EP_POWER_RESET, SEL(1));
    TEST_ASSERT_EQUAL_INT(429, R.status);
    TEST_ASSERT_EQUAL_UINT32(1, R.retry_after_s);

    fx_tick(T + 999); /* 残り 0 */
    api(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"cooldown_ms\":0,"));
    api(KVM_API_EP_POWER_RESET, SEL(1));
    rsp(202, "{\"result\":\"accepted\",\"channel\":1,\"action\":\"reset\",\"op_id\":2}");
    TEST_ASSERT_FALSE(mock_violated(&M));
}

void test_each_power_action_pulses_its_own_line_only_for_the_active_channel(void) {
    static const struct { kvm_api_endpoint_t ep; const char *act; kvm_board_line_t line; } T3[] = {
        {KVM_API_EP_POWER_PRESS, "press", KVM_BOARD_LINE_POWER},
        {KVM_API_EP_POWER_RESET, "reset", KVM_BOARD_LINE_RESET},
        {KVM_API_EP_POWER_FORCE, "force", KVM_BOARD_LINE_POWER_FORCE},
    };
    for (int i = 0; i < 3; i++) {
        api_boot(3);
        char want[96];
        api(T3[i].ep, SEL(3));
        snprintf(want, sizeof(want), "{\"result\":\"accepted\",\"channel\":3,\"action\":\"%s\",\"op_id\":1}", T3[i].act);
        rsp(202, want);
        fx_steps(2);
        TEST_ASSERT_TRUE(mock_line_active(&M, T3[i].line));
        TEST_ASSERT_EQUAL_INT(1, mock_active_count(&M));
        TEST_ASSERT_EQUAL_UINT8(3, M.addr);
        TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
        TEST_ASSERT_EQUAL_INT(0, mock_active_count(&M));
        TEST_ASSERT_FALSE(mock_violated(&M));
    }
}

void test_unknown_state_is_409_for_select_and_power(void) {
    mock_set_led_off(&M);
    fx_steps(3);
    api(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"state\":\"unknown\",\"active_channel\":null,"));
    api(KVM_API_EP_SELECT, SEL(2));
    rsp(409, "{\"error\":\"unknown_state\"}");
    api(KVM_API_EP_POWER_PRESS, SEL(1));
    rsp(409, "{\"error\":\"unknown_state\"}");
    api(KVM_API_EP_POWER_PRESS, SEL(4));
    rsp(409, "{\"error\":\"unknown_state\"}");
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_ASSERT_LINE));

    /* LED が確定すれば受け付ける */
    mock_set_led_channel(&M, 2);
    fx_steps(10);
    api(KVM_API_EP_POWER_PRESS, SEL(2));
    TEST_ASSERT_EQUAL_INT(202, R.status);
}

void test_atx_locked_is_409(void) {
    C.logic.current_outputs.atx_locked = true; /* idle のまま ATX ロックされた状態（kvm_ctrl のテストと同じ作り方） */
    api(KVM_API_EP_POWER_RESET, SEL(1));
    rsp(409, "{\"error\":\"atx_locked\"}");
    api(KVM_API_EP_POWER_RESET, SEL(2));
    rsp(409, "{\"error\":\"channel_mismatch\",\"active_channel\":1}"); /* mismatch が locked より先 */
}

void test_tracking_manual_switch_is_busy(void) {
    mock_set_led_channel(&M, 3);
    fx_step();
    api(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"state\":\"tracking\""));
    api(KVM_API_EP_SELECT, SEL(2));
    rsp(409, "{\"error\":\"busy\"}");
    api(KVM_API_EP_POWER_PRESS, SEL(3));
    rsp(409, "{\"error\":\"busy\"}");
    fx_steps(10);
    api(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"state\":\"idle\",\"active_channel\":3,"));
    api(KVM_API_EP_POWER_PRESS, SEL(3));
    TEST_ASSERT_EQUAL_INT(202, R.status);
}

void test_order_unknown_beats_mismatch_and_cooldown(void) {
    api(KVM_API_EP_POWER_PRESS, SEL(1));
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
    mock_set_led_off(&M);
    fx_steps(3);
    api(KVM_API_EP_POWER_PRESS, SEL(2)); /* mismatch でも cooldown でもある */
    rsp(409, "{\"error\":\"unknown_state\"}");
}

void test_fault_gives_503_hw_fault_and_nothing_is_driven(void) {
    mock_fail_all(&M, MOCK_FN_READ_LED, true);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    mock_reset_calls(&M);

    api(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_EQUAL_INT(200, R.status);
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"fault\":true"));

    api(KVM_API_EP_SELECT, SEL(2));
    rsp(503, "{\"error\":\"hw_fault\"}");
    api(KVM_API_EP_POWER_PRESS, SEL(1));
    rsp(503, "{\"error\":\"hw_fault\"}");
    api(KVM_API_EP_POWER_FORCE, SEL(3)); /* mismatch でも fault が先 */
    rsp(503, "{\"error\":\"hw_fault\"}");
    mock_fail_all(&M, MOCK_FN_READ_LED, false);
    fx_steps(50);
    api(KVM_API_EP_POWER_PRESS, SEL(1));
    rsp(503, "{\"error\":\"hw_fault\"}"); /* ラッチは再起動まで解除しない */
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_ASSERT_LINE));
    TEST_ASSERT_EQUAL_UINT(0, mock_calls(&M, MOCK_FN_SET_ADDR));
    TEST_ASSERT_FALSE(mock_violated(&M));
}

void test_fault_during_a_pulse_aborts_the_op_and_is_reported(void) {
    api(KVM_API_EP_POWER_FORCE, SEL(1));
    TEST_ASSERT_EQUAL_INT(202, R.status);
    fx_steps(3);
    TEST_ASSERT_TRUE(mock_line_active(&M, KVM_BOARD_LINE_POWER_FORCE));
    mock_fail_all(&M, MOCK_FN_READ_LED, true);
    TEST_ASSERT_EQUAL(KVM_CTRL_ERR_FAULT, fx_step());
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_active_count(&M), "the fault must release the line immediately");
    api(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"fault\":true"));
    TEST_ASSERT_NOT_NULL(strstr(R.body, "\"result\":\"aborted\""));
}

void test_service_not_started_is_503_not_ready(void) {
    g_ready = false;
    api(KVM_API_EP_STATUS, NULL);
    rsp(503, "{\"error\":\"not_ready\"}");
    api(KVM_API_EP_SELECT, SEL(2));
    rsp(503, "{\"error\":\"not_ready\"}");
    api(KVM_API_EP_POWER_PRESS, SEL(1));
    rsp(503, "{\"error\":\"not_ready\"}");
    TEST_ASSERT_EQUAL_INT(0, g_locks);
    TEST_ASSERT_EQUAL_size_t(0, M.log_len);
    /* 形式が不正なら未起動でも 400（判定の順序） */
    api(KVM_API_EP_POWER_PRESS, "{\"channel\":9}");
    rsp(400, "{\"error\":\"invalid_request\"}");
}

void test_kvm_ctrl_not_initialized_is_503_not_ready(void) {
    memset(&C, 0, sizeof(C)); /* init されていない ctrl */
    api(KVM_API_EP_STATUS, NULL);
    rsp(503, "{\"error\":\"not_ready\"}");
    api(KVM_API_EP_SELECT, SEL(2));
    rsp(503, "{\"error\":\"not_ready\"}");
    api(KVM_API_EP_POWER_PRESS, SEL(2));
    rsp(503, "{\"error\":\"not_ready\"}");
    TEST_ASSERT_EQUAL_INT(g_locks, g_unlocks);
}

void test_invalid_body_never_reaches_kvm_ctrl(void) {
    mock_reset_calls(&M);
    const uint32_t next_op = C.next_op_id;
    api(KVM_API_EP_SELECT, "{\"channel\":2,\"channel\":3}");
    rsp(400, "{\"error\":\"invalid_request\"}");
    api(KVM_API_EP_POWER_FORCE, "{\"channel\":\"1\"}");
    rsp(400, "{\"error\":\"invalid_request\"}");
    api(KVM_API_EP_POWER_FORCE, "{\"channel\":1,\"duration_ms\":99999}");
    rsp(400, "{\"error\":\"invalid_request\"}");
    TEST_ASSERT_EQUAL_INT(0, g_locks);
    TEST_ASSERT_EQUAL_UINT32(next_op, C.next_op_id);
    TEST_ASSERT_EQUAL_size_t(0, M.log_len);
}

void test_select_during_cooldown_is_not_blocked(void) {
    api(KVM_API_EP_POWER_RESET, SEL(1));
    TEST_ASSERT_TRUE(fx_run_until(fx_state_is_idle, 1000));
    api(KVM_API_EP_POWER_RESET, SEL(1));
    TEST_ASSERT_EQUAL_INT(429, R.status);
    api(KVM_API_EP_SELECT, SEL(2));
    rsp(202, "{\"result\":\"accepted\",\"channel\":2,\"op_id\":2}");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_status_after_boot);
    RUN_TEST(test_select_flow_to_completion_is_reflected_in_status);
    RUN_TEST(test_power_mismatch_is_409_and_outputs_nothing);
    RUN_TEST(test_power_flow_busy_cooldown_and_retry_after);
    RUN_TEST(test_each_power_action_pulses_its_own_line_only_for_the_active_channel);
    RUN_TEST(test_unknown_state_is_409_for_select_and_power);
    RUN_TEST(test_atx_locked_is_409);
    RUN_TEST(test_tracking_manual_switch_is_busy);
    RUN_TEST(test_order_unknown_beats_mismatch_and_cooldown);
    RUN_TEST(test_fault_gives_503_hw_fault_and_nothing_is_driven);
    RUN_TEST(test_fault_during_a_pulse_aborts_the_op_and_is_reported);
    RUN_TEST(test_service_not_started_is_503_not_ready);
    RUN_TEST(test_kvm_ctrl_not_initialized_is_503_not_ready);
    RUN_TEST(test_invalid_body_never_reaches_kvm_ctrl);
    RUN_TEST(test_select_during_cooldown_is_not_blocked);
    return UNITY_END();
}

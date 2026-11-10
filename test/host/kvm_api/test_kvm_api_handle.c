#include "ka_common.h"

/*
 * kvm_api_handle(): モック backend での、全結果コードの対応、Retry-After、lock/unlock の対応、
 * サービス未起動、判定の順序（docs/WEB_API.md 7.2 / 7.3）。
 */

void setUp(void) { be_reset(); }
void tearDown(void) {}

static const kvm_api_endpoint_t POWER_EPS[3] = {KVM_API_EP_POWER_PRESS, KVM_API_EP_POWER_RESET, KVM_API_EP_POWER_FORCE};
static const char *const POWER_NAMES[3] = {"press", "reset", "force"};
static const char *const POWER_KINDS[3] = {"power_press", "power_reset", "power_force"};
static const kvm_ctrl_power_action_t POWER_ACTIONS[3] = {KVM_CTRL_POWER_PRESS, KVM_CTRL_POWER_RESET, KVM_CTRL_POWER_FORCE};

/* ---------------------------------------------------------------- select */

void test_select_accepted(void) {
    B.select_ret = KVM_CTRL_OK;
    B.op_id_out = 8;
    call(KVM_API_EP_SELECT, "{\"channel\":3}");
    expect_rsp(202, "{\"result\":\"accepted\",\"channel\":3,\"op_id\":8}");
    expect_log(true, "select", 3, "accepted");
    TEST_ASSERT_FALSE(RSP.has_retry_after);
    TEST_ASSERT_EQUAL_INT(1, B.select_calls);
    TEST_ASSERT_EQUAL_INT(0, B.power_calls);
    TEST_ASSERT_EQUAL_UINT8(3, B.last_channel);
    be_assert_balanced();
}

void test_select_op_id_is_passed_through(void) {
    B.op_id_out = 4294967295u;
    call(KVM_API_EP_SELECT, "{\"channel\":1}");
    expect_rsp(202, "{\"result\":\"accepted\",\"channel\":1,\"op_id\":4294967295}");
    B.op_id_out = 1;
    call(KVM_API_EP_SELECT, "{\"channel\":4}");
    expect_rsp(202, "{\"result\":\"accepted\",\"channel\":4,\"op_id\":1}");
}

void test_select_already_selected_is_200(void) {
    B.select_ret = KVM_CTRL_ALREADY_SELECTED;
    call(KVM_API_EP_SELECT, "{\"channel\":2}");
    expect_rsp(200, "{\"result\":\"already_selected\",\"channel\":2}");
    expect_log(true, "select", 2, "already_selected");
    TEST_ASSERT_FALSE(RSP.has_retry_after);
    be_assert_balanced();
}

typedef struct {
    kvm_ctrl_result_t r;
    int status;
    const char *body;
    const char *reason;
} err_row_t;

/* select が返しうる拒否 */
static const err_row_t SELECT_ERRS[] = {
    {KVM_CTRL_ERR_INVALID_PARAM, 400, "{\"error\":\"invalid_request\"}", "invalid_request"},
    {KVM_CTRL_ERR_BUSY, 409, "{\"error\":\"busy\"}", "busy"},
    {KVM_CTRL_ERR_UNKNOWN_STATE, 409, "{\"error\":\"unknown_state\"}", "unknown_state"},
    {KVM_CTRL_ERR_FAULT, 503, "{\"error\":\"hw_fault\"}", "hw_fault"},
    {KVM_CTRL_ERR_NOT_READY, 503, "{\"error\":\"not_ready\"}", "not_ready"},
};

void test_select_error_mapping(void) {
    for (size_t i = 0; i < sizeof(SELECT_ERRS) / sizeof(SELECT_ERRS[0]); i++) {
        be_reset();
        B.select_ret = SELECT_ERRS[i].r;
        call(KVM_API_EP_SELECT, "{\"channel\":4}");
        expect_rsp(SELECT_ERRS[i].status, SELECT_ERRS[i].body);
        expect_log(true, "select", 4, SELECT_ERRS[i].reason);
        TEST_ASSERT_FALSE(RSP.has_retry_after);
        be_assert_balanced();
    }
}

/* ---------------------------------------------------------------- power */

void test_power_accepted_each_action(void) {
    for (int a = 0; a < 3; a++) {
        be_reset();
        B.op_id_out = 9;
        call(POWER_EPS[a], "{\"channel\":2}");
        char want[96];
        snprintf(want, sizeof(want), "{\"result\":\"accepted\",\"channel\":2,\"action\":\"%s\",\"op_id\":9}", POWER_NAMES[a]);
        expect_rsp(202, want);
        expect_log(true, POWER_KINDS[a], 2, "accepted");
        TEST_ASSERT_EQUAL_INT(1, B.power_calls);
        TEST_ASSERT_EQUAL_INT(0, B.select_calls);
        TEST_ASSERT_EQUAL_UINT8(2, B.last_channel);
        TEST_ASSERT_EQUAL_INT(POWER_ACTIONS[a], B.last_action);
        be_assert_balanced();
    }
}

void test_power_channel_is_passed_to_backend(void) {
    for (uint8_t ch = 1; ch <= 4; ch++) {
        be_reset();
        char body[32];
        snprintf(body, sizeof(body), "{\"channel\":%u}", (unsigned)ch);
        call(KVM_API_EP_POWER_RESET, body);
        TEST_ASSERT_EQUAL_INT(202, RSP.status);
        TEST_ASSERT_EQUAL_UINT8(ch, B.last_channel);
        TEST_ASSERT_EQUAL_UINT8(ch, RSP.log_channel);
    }
}

static const err_row_t POWER_ERRS[] = {
    {KVM_CTRL_ERR_INVALID_PARAM, 400, "{\"error\":\"invalid_request\"}", "invalid_request"},
    {KVM_CTRL_ERR_BUSY, 409, "{\"error\":\"busy\"}", "busy"},
    {KVM_CTRL_ERR_UNKNOWN_STATE, 409, "{\"error\":\"unknown_state\"}", "unknown_state"},
    {KVM_CTRL_ERR_ATX_LOCKED, 409, "{\"error\":\"atx_locked\"}", "atx_locked"},
    {KVM_CTRL_ERR_FAULT, 503, "{\"error\":\"hw_fault\"}", "hw_fault"},
    {KVM_CTRL_ERR_NOT_READY, 503, "{\"error\":\"not_ready\"}", "not_ready"},
};

void test_power_error_mapping_each_action(void) {
    for (int a = 0; a < 3; a++) {
        for (size_t i = 0; i < sizeof(POWER_ERRS) / sizeof(POWER_ERRS[0]); i++) {
            be_reset();
            B.power_ret = POWER_ERRS[i].r;
            call(POWER_EPS[a], "{\"channel\":1}");
            expect_rsp(POWER_ERRS[i].status, POWER_ERRS[i].body);
            expect_log(true, POWER_KINDS[a], 1, POWER_ERRS[i].reason);
            TEST_ASSERT_FALSE(RSP.has_retry_after);
            TEST_ASSERT_EQUAL_INT(0, B.status_calls); /* これらの拒否では状態を読まない */
            be_assert_balanced();
        }
    }
}

void test_channel_mismatch_reports_active_channel(void) {
    for (uint8_t active = 1; active <= 4; active++) {
        be_reset();
        B.power_ret = KVM_CTRL_ERR_CHANNEL_MISMATCH;
        B.status.active_channel = active;
        call(KVM_API_EP_POWER_PRESS, "{\"channel\":2}");
        char want[80];
        snprintf(want, sizeof(want), "{\"error\":\"channel_mismatch\",\"active_channel\":%u}", (unsigned)active);
        expect_rsp(409, want);
        expect_log(true, "power_press", 2, "channel_mismatch");
        TEST_ASSERT_FALSE(RSP.has_retry_after);
        be_assert_balanced();
    }
}

void test_channel_mismatch_status_is_read_under_the_same_lock(void) {
    B.power_ret = KVM_CTRL_ERR_CHANNEL_MISMATCH;
    call(KVM_API_EP_POWER_FORCE, "{\"channel\":1}");
    TEST_ASSERT_EQUAL_INT(1, B.lock_calls);
    TEST_ASSERT_EQUAL_INT(1, B.unlock_calls);
    TEST_ASSERT_EQUAL_INT(1, B.status_calls);
    TEST_ASSERT_EQUAL_INT(1, B.power_calls);
    be_assert_balanced();
}

void test_channel_mismatch_with_unusable_active_channel_is_null(void) {
    B.power_ret = KVM_CTRL_ERR_CHANNEL_MISMATCH;
    B.status.active_channel = 0; /* 起こらないはず。null にして、偽の値は出さない */
    call(KVM_API_EP_POWER_PRESS, "{\"channel\":2}");
    expect_rsp(409, "{\"error\":\"channel_mismatch\",\"active_channel\":null}");
    B.status.active_channel = 5;
    call(KVM_API_EP_POWER_PRESS, "{\"channel\":2}");
    expect_rsp(409, "{\"error\":\"channel_mismatch\",\"active_channel\":null}");
    B.status_ret = KVM_CTRL_ERR_NOT_READY;
    B.status.active_channel = 3;
    call(KVM_API_EP_POWER_PRESS, "{\"channel\":2}");
    expect_rsp(409, "{\"error\":\"channel_mismatch\",\"active_channel\":null}");
    be_assert_balanced();
}

static uint32_t ceil_s(uint32_t ms) { return (uint32_t)(((unsigned long long)ms + 999ull) / 1000ull); }

void test_cooldown_retry_after_rounds_up_with_minimum_1(void) {
    static const struct { uint32_t ms; uint32_t sec; } T[] = {
        {0, 1},      /* 最小 1 */
        {1, 1},      {500, 1},    {999, 1},    {1000, 1},
        {1001, 2},   {1500, 2},   {1999, 2},   {2000, 2},
        {2001, 3},   {2500, 3},   {2999, 3},   {3000, 3},
        {3001, 4},   {4500, 5},   {4999, 5},   {5000, 5},
        {59999, 60}, {60000, 60}, {60001, 61},
        {4294967295u, 4294968u},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        be_reset();
        B.power_ret = KVM_CTRL_ERR_COOLDOWN;
        B.status.cooldown_ms = T[i].ms;
        call(KVM_API_EP_POWER_PRESS, "{\"channel\":2}");
        char msg[64];
        snprintf(msg, sizeof(msg), "cooldown_ms=%lu", (unsigned long)T[i].ms);
        TEST_ASSERT_EQUAL_INT_MESSAGE(429, RSP.status, msg);
        TEST_ASSERT_EQUAL_STRING("{\"error\":\"cooldown\"}", RSP.body);
        TEST_ASSERT_TRUE_MESSAGE(RSP.has_retry_after, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(T[i].sec, RSP.retry_after_s, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(T[i].ms == 0 ? 1 : ceil_s(T[i].ms), RSP.retry_after_s, msg); /* 独立な式でも確認 */
        expect_log(true, "power_press", 2, "cooldown");
        be_assert_balanced();
    }
}

void test_cooldown_retry_after_for_every_ms_up_to_5_seconds(void) {
    for (uint32_t ms = 0; ms <= 5001; ms++) {
        B.power_ret = KVM_CTRL_ERR_COOLDOWN;
        B.status.cooldown_ms = ms;
        call(KVM_API_EP_POWER_RESET, "{\"channel\":3}");
        uint32_t want = ms == 0 ? 1 : ceil_s(ms);
        if (RSP.retry_after_s != want) TEST_FAIL_MESSAGE("retry_after mismatch");
    }
}

void test_cooldown_with_unreadable_status_still_has_retry_after_1(void) {
    B.power_ret = KVM_CTRL_ERR_COOLDOWN;
    B.status_ret = KVM_CTRL_ERR_NOT_READY;
    B.status.cooldown_ms = 9000; /* 読めなかったときの値は使わない */
    call(KVM_API_EP_POWER_FORCE, "{\"channel\":4}");
    TEST_ASSERT_EQUAL_INT(429, RSP.status);
    TEST_ASSERT_TRUE(RSP.has_retry_after);
    TEST_ASSERT_EQUAL_UINT32(1, RSP.retry_after_s);
    be_assert_balanced();
}

void test_cooldown_status_is_read_under_the_same_lock(void) {
    B.power_ret = KVM_CTRL_ERR_COOLDOWN;
    B.status.cooldown_ms = 1500;
    call(KVM_API_EP_POWER_PRESS, "{\"channel\":2}");
    TEST_ASSERT_EQUAL_INT(1, B.lock_calls);
    TEST_ASSERT_EQUAL_INT(1, B.status_calls);
    TEST_ASSERT_EQUAL_INT(1, B.unlock_calls);
    be_assert_balanced();
}

void test_retry_after_only_for_cooldown(void) {
    static const kvm_ctrl_result_t R[] = {
        KVM_CTRL_OK, KVM_CTRL_ALREADY_SELECTED, KVM_CTRL_ERR_INVALID_PARAM, KVM_CTRL_ERR_BUSY, KVM_CTRL_ERR_UNKNOWN_STATE,
        KVM_CTRL_ERR_CHANNEL_MISMATCH, KVM_CTRL_ERR_ATX_LOCKED, KVM_CTRL_ERR_FAULT, KVM_CTRL_ERR_NOT_READY,
    };
    for (size_t i = 0; i < sizeof(R) / sizeof(R[0]); i++) {
        be_reset();
        B.power_ret = R[i];
        B.status.cooldown_ms = 5000;
        call(KVM_API_EP_POWER_PRESS, "{\"channel\":2}");
        TEST_ASSERT_FALSE(RSP.has_retry_after);
        TEST_ASSERT_EQUAL_UINT32(0, RSP.retry_after_s);
    }
}

/* ---------------------------------------------------------------- 想定外の結果は何も実行したことにしない */

void test_unexpected_results_are_500(void) {
    be_reset();
    B.power_ret = KVM_CTRL_ALREADY_SELECTED; /* power に「すでに選択済み」は無い */
    call(KVM_API_EP_POWER_PRESS, "{\"channel\":2}");
    expect_rsp(500, "{\"error\":\"internal_error\"}");
    expect_log(true, "power_press", 2, "internal_error");
    be_assert_balanced();

    be_reset();
    B.select_ret = (kvm_ctrl_result_t)99;
    call(KVM_API_EP_SELECT, "{\"channel\":2}");
    expect_rsp(500, "{\"error\":\"internal_error\"}");
    be_assert_balanced();

    be_reset();
    B.power_ret = (kvm_ctrl_result_t)-1;
    call(KVM_API_EP_POWER_FORCE, "{\"channel\":2}");
    expect_rsp(500, "{\"error\":\"internal_error\"}");
    be_assert_balanced();
}

void test_invalid_endpoint_is_500_and_touches_nothing(void) {
    call((kvm_api_endpoint_t)99, "{\"channel\":2}");
    expect_rsp(500, "{\"error\":\"internal_error\"}");
    TEST_ASSERT_EQUAL_STRING("unknown", RSP.log_kind);
    TEST_ASSERT_EQUAL_INT(0, B.lock_calls + B.ready_calls + B.select_calls + B.power_calls + B.status_calls);
    call((kvm_api_endpoint_t)-1, "{\"channel\":2}");
    TEST_ASSERT_EQUAL_INT(500, RSP.status);
}

void test_null_response_is_ignored(void) {
    kvm_api_backend_t t = be_table();
    kvm_api_handle(&t, KVM_API_EP_SELECT, "{\"channel\":2}", 13, NULL);
    TEST_ASSERT_EQUAL_INT(0, B.lock_calls);
}

/* ---------------------------------------------------------------- 形式 / 範囲 */

void test_invalid_body_is_400_and_does_not_reach_the_backend(void) {
    static const char *const BAD[] = {"", "{}", "{\"channel\":0}", "{\"channel\":5}", "{\"channel\":\"2\"}", "not json",
                                      "{\"channel\":2,\"x\":1}", "{\"channel\":2}x"};
    static const kvm_api_endpoint_t EPS[] = {KVM_API_EP_SELECT, KVM_API_EP_POWER_PRESS, KVM_API_EP_POWER_RESET,
                                             KVM_API_EP_POWER_FORCE};
    for (size_t e = 0; e < 4; e++) {
        for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
            be_reset();
            call(EPS[e], BAD[i]);
            expect_rsp(400, "{\"error\":\"invalid_request\"}");
            expect_log(true, e == 0 ? "select" : POWER_KINDS[e - 1], 0, "invalid_request");
            TEST_ASSERT_EQUAL_INT(0, B.ready_calls + B.lock_calls + B.unlock_calls + B.select_calls + B.power_calls + B.status_calls);
        }
    }
}

void test_body_over_64_bytes_is_400(void) {
    char body[80];
    memset(body, ' ', sizeof(body));
    memcpy(body, "{\"channel\":2}", 13);
    body[65] = '\0';
    kvm_api_backend_t t = be_table();
    kvm_api_handle(&t, KVM_API_EP_SELECT, body, 65, &RSP);
    expect_rsp(400, "{\"error\":\"invalid_request\"}");
    kvm_api_handle(&t, KVM_API_EP_SELECT, body, 64, &RSP);
    TEST_ASSERT_EQUAL_INT(202, RSP.status);
}

void test_null_body_is_400(void) {
    kvm_api_backend_t t = be_table();
    kvm_api_handle(&t, KVM_API_EP_POWER_PRESS, NULL, 0, &RSP);
    expect_rsp(400, "{\"error\":\"invalid_request\"}");
    kvm_api_handle(&t, KVM_API_EP_POWER_PRESS, NULL, 13, &RSP);
    expect_rsp(400, "{\"error\":\"invalid_request\"}");
}

void test_body_is_ignored_for_status(void) {
    call(KVM_API_EP_STATUS, "this is not json at all");
    TEST_ASSERT_EQUAL_INT(200, RSP.status);
    kvm_api_backend_t t = be_table();
    kvm_api_handle(&t, KVM_API_EP_STATUS, NULL, 0, &RSP);
    TEST_ASSERT_EQUAL_INT(200, RSP.status);
}

/* ---------------------------------------------------------------- サービス未起動 */

void test_not_ready_cases_for_every_post_endpoint(void) {
    static const kvm_api_endpoint_t EPS[] = {KVM_API_EP_SELECT, KVM_API_EP_POWER_PRESS, KVM_API_EP_POWER_RESET,
                                             KVM_API_EP_POWER_FORCE};
    for (size_t e = 0; e < 4; e++) {
        /* (a) backend が NULL */
        memset(&RSP, 0xAB, sizeof(RSP));
        kvm_api_handle(NULL, EPS[e], "{\"channel\":3}", 13, &RSP);
        expect_rsp(503, "{\"error\":\"not_ready\"}");
        TEST_ASSERT_EQUAL_UINT8(3, RSP.log_channel);
        TEST_ASSERT_EQUAL_STRING("not_ready", RSP.log_reason);
        TEST_ASSERT_TRUE(RSP.log);

        /* (b) ready が false: lock も呼ばない */
        be_reset();
        B.ready_result = false;
        call(EPS[e], "{\"channel\":3}");
        expect_rsp(503, "{\"error\":\"not_ready\"}");
        TEST_ASSERT_EQUAL_INT(0, B.lock_calls + B.unlock_calls + B.select_calls + B.power_calls + B.status_calls);

        /* (c) lock が取れない: unlock は呼ばない、何も呼ばない */
        be_reset();
        B.lock_result = false;
        call(EPS[e], "{\"channel\":3}");
        expect_rsp(503, "{\"error\":\"not_ready\"}");
        TEST_ASSERT_EQUAL_INT(1, B.lock_calls);
        TEST_ASSERT_EQUAL_INT(0, B.unlock_calls + B.select_calls + B.power_calls + B.status_calls);
        be_assert_balanced();

        /* (d) kvm_ctrl が NOT_READY: unlock する */
        be_reset();
        B.select_ret = KVM_CTRL_ERR_NOT_READY;
        B.power_ret = KVM_CTRL_ERR_NOT_READY;
        call(EPS[e], "{\"channel\":3}");
        expect_rsp(503, "{\"error\":\"not_ready\"}");
        be_assert_balanced();
    }
}

void test_backend_with_missing_function_is_not_ready(void) {
    for (int missing = 0; missing < 6; missing++) {
        be_reset();
        kvm_api_backend_t t = be_table();
        switch (missing) {
            case 0: t.lock = NULL; break;
            case 1: t.unlock = NULL; break;
            case 2: t.ready = NULL; break;
            case 3: t.get_status = NULL; break;
            case 4: t.select = NULL; break;
            default: t.power = NULL; break;
        }
        kvm_api_handle(&t, KVM_API_EP_POWER_PRESS, "{\"channel\":3}", 13, &RSP);
        expect_rsp(503, "{\"error\":\"not_ready\"}");
        kvm_api_handle(&t, KVM_API_EP_STATUS, NULL, 0, &RSP);
        expect_rsp(503, "{\"error\":\"not_ready\"}");
        TEST_ASSERT_EQUAL_INT(0, B.lock_calls);
        TEST_ASSERT_EQUAL_INT(0, B.power_calls + B.select_calls + B.status_calls);
    }
}

void test_form_error_beats_not_ready(void) {
    /* 判定の順序: 形式 -> 範囲 -> 未起動。未起動でも、不正なボディは 400 */
    kvm_api_handle(NULL, KVM_API_EP_SELECT, "{\"channel\":9}", 13, &RSP);
    expect_rsp(400, "{\"error\":\"invalid_request\"}");
    B.ready_result = false;
    call(KVM_API_EP_POWER_PRESS, "{}");
    expect_rsp(400, "{\"error\":\"invalid_request\"}");
    B.lock_result = false;
    B.ready_result = true;
    call(KVM_API_EP_POWER_PRESS, "{\"channel\":0}");
    expect_rsp(400, "{\"error\":\"invalid_request\"}");
}

void test_not_ready_beats_every_kvm_ctrl_result_by_construction(void) {
    /* 未起動なら kvm_ctrl に触れないので、ほかの拒否理由（busy など）は出ない */
    B.ready_result = false;
    B.power_ret = KVM_CTRL_ERR_BUSY;
    B.select_ret = KVM_CTRL_ERR_BUSY;
    call(KVM_API_EP_POWER_PRESS, "{\"channel\":3}");
    expect_rsp(503, "{\"error\":\"not_ready\"}");
    call(KVM_API_EP_SELECT, "{\"channel\":3}");
    expect_rsp(503, "{\"error\":\"not_ready\"}");
}

/* ---------------------------------------------------------------- status */

void test_status_ok(void) {
    B.status.state = KVM_CTRL_STATE_SWITCHING;
    B.status.active_channel = 4;
    B.status.cooldown_ms = 12;
    call(KVM_API_EP_STATUS, NULL);
    expect_rsp(200,
        "{\"state\":\"switching\",\"active_channel\":4,\"video_sync\":\"locked\",\"atx_locked\":false,\"atx_anomaly\":false,"
        "\"fault\":false,\"cooldown_ms\":12,\"last_op\":null}");
    TEST_ASSERT_FALSE(RSP.log); /* 成功はポーリングで溢れるので記録しない */
    TEST_ASSERT_EQUAL_STRING("status", RSP.log_kind);
    TEST_ASSERT_FALSE(RSP.has_retry_after);
    TEST_ASSERT_EQUAL_INT(1, B.lock_calls);
    TEST_ASSERT_EQUAL_INT(1, B.status_calls);
    TEST_ASSERT_EQUAL_INT(0, B.select_calls + B.power_calls);
    be_assert_balanced();
}

void test_status_with_fault_is_still_200(void) {
    B.status.fault = true;
    call(KVM_API_EP_STATUS, NULL);
    TEST_ASSERT_EQUAL_INT(200, RSP.status);
    TEST_ASSERT_NOT_NULL(strstr(RSP.body, "\"fault\":true"));
}

void test_status_not_ready_cases(void) {
    kvm_api_handle(NULL, KVM_API_EP_STATUS, NULL, 0, &RSP);
    expect_rsp(503, "{\"error\":\"not_ready\"}");
    TEST_ASSERT_FALSE(RSP.log); /* 未起動の間のポーリングで、ログを溢れさせない */

    be_reset();
    B.ready_result = false;
    call(KVM_API_EP_STATUS, NULL);
    expect_rsp(503, "{\"error\":\"not_ready\"}");
    TEST_ASSERT_EQUAL_INT(0, B.lock_calls);

    be_reset();
    B.lock_result = false;
    call(KVM_API_EP_STATUS, NULL);
    expect_rsp(503, "{\"error\":\"not_ready\"}");
    TEST_ASSERT_EQUAL_INT(0, B.unlock_calls + B.status_calls);
    be_assert_balanced();

    be_reset();
    B.status_ret = KVM_CTRL_ERR_NOT_READY;
    call(KVM_API_EP_STATUS, NULL);
    expect_rsp(503, "{\"error\":\"not_ready\"}");
    be_assert_balanced();
}

void test_status_unexpected_failures_are_500(void) {
    B.status_ret = KVM_CTRL_ERR_INVALID_PARAM;
    call(KVM_API_EP_STATUS, NULL);
    expect_rsp(500, "{\"error\":\"internal_error\"}");
    TEST_ASSERT_TRUE(RSP.log);
    be_assert_balanced();

    be_reset();
    B.status.state = (kvm_ctrl_state_t)42; /* JSON にできない */
    call(KVM_API_EP_STATUS, NULL);
    expect_rsp(500, "{\"error\":\"internal_error\"}");
    be_assert_balanced();
}

/* ---------------------------------------------------------------- 応答の共通の性質 */

void test_every_response_is_a_json_object_with_matching_length(void) {
    static const kvm_ctrl_result_t R[] = {
        KVM_CTRL_OK, KVM_CTRL_ALREADY_SELECTED, KVM_CTRL_ERR_INVALID_PARAM, KVM_CTRL_ERR_BUSY, KVM_CTRL_ERR_UNKNOWN_STATE,
        KVM_CTRL_ERR_CHANNEL_MISMATCH, KVM_CTRL_ERR_ATX_LOCKED, KVM_CTRL_ERR_COOLDOWN, KVM_CTRL_ERR_FAULT,
        KVM_CTRL_ERR_NOT_READY, (kvm_ctrl_result_t)77,
    };
    for (size_t i = 0; i < sizeof(R) / sizeof(R[0]); i++) {
        for (int e = 0; e < 4; e++) {
            be_reset();
            B.select_ret = R[i];
            B.power_ret = R[i];
            B.status.active_channel = 3;
            B.status.cooldown_ms = 1234;
            kvm_api_endpoint_t ep = e == 0 ? KVM_API_EP_SELECT : POWER_EPS[e - 1];
            call(ep, "{\"channel\":1}");
            TEST_ASSERT_EQUAL_UINT((unsigned)strlen(RSP.body), (unsigned)RSP.body_len);
            TEST_ASSERT_TRUE(RSP.body_len >= 2);
            TEST_ASSERT_EQUAL_CHAR('{', RSP.body[0]);
            TEST_ASSERT_EQUAL_CHAR('}', RSP.body[RSP.body_len - 1]);
            TEST_ASSERT_TRUE(RSP.status == 200 || RSP.status == 202 || RSP.status == 400 || RSP.status == 409 ||
                             RSP.status == 429 || RSP.status == 500 || RSP.status == 503);
            TEST_ASSERT_NOT_NULL(RSP.log_kind);
            TEST_ASSERT_NOT_NULL(RSP.log_reason);
            be_assert_balanced();
        }
    }
}

/* ログには、ボディの生データを出さない（種類・チャンネル・理由は固定の語彙のみ） */
void test_log_fields_never_echo_the_request_body(void) {
    call(KVM_API_EP_SELECT, "{\"channel\":3,\"secret\":\"hunter2\"}");
    TEST_ASSERT_NULL(strstr(RSP.body, "hunter2"));
    TEST_ASSERT_NULL(strstr(RSP.log_reason, "hunter2"));
    TEST_ASSERT_NULL(strstr(RSP.log_kind, "hunter2"));
    TEST_ASSERT_EQUAL_UINT8(0, RSP.log_channel);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_select_accepted);
    RUN_TEST(test_select_op_id_is_passed_through);
    RUN_TEST(test_select_already_selected_is_200);
    RUN_TEST(test_select_error_mapping);
    RUN_TEST(test_power_accepted_each_action);
    RUN_TEST(test_power_channel_is_passed_to_backend);
    RUN_TEST(test_power_error_mapping_each_action);
    RUN_TEST(test_channel_mismatch_reports_active_channel);
    RUN_TEST(test_channel_mismatch_status_is_read_under_the_same_lock);
    RUN_TEST(test_channel_mismatch_with_unusable_active_channel_is_null);
    RUN_TEST(test_cooldown_retry_after_rounds_up_with_minimum_1);
    RUN_TEST(test_cooldown_retry_after_for_every_ms_up_to_5_seconds);
    RUN_TEST(test_cooldown_with_unreadable_status_still_has_retry_after_1);
    RUN_TEST(test_cooldown_status_is_read_under_the_same_lock);
    RUN_TEST(test_retry_after_only_for_cooldown);
    RUN_TEST(test_unexpected_results_are_500);
    RUN_TEST(test_invalid_endpoint_is_500_and_touches_nothing);
    RUN_TEST(test_null_response_is_ignored);
    RUN_TEST(test_invalid_body_is_400_and_does_not_reach_the_backend);
    RUN_TEST(test_body_over_64_bytes_is_400);
    RUN_TEST(test_null_body_is_400);
    RUN_TEST(test_body_is_ignored_for_status);
    RUN_TEST(test_not_ready_cases_for_every_post_endpoint);
    RUN_TEST(test_backend_with_missing_function_is_not_ready);
    RUN_TEST(test_form_error_beats_not_ready);
    RUN_TEST(test_not_ready_beats_every_kvm_ctrl_result_by_construction);
    RUN_TEST(test_status_ok);
    RUN_TEST(test_status_with_fault_is_still_200);
    RUN_TEST(test_status_not_ready_cases);
    RUN_TEST(test_status_unexpected_failures_are_500);
    RUN_TEST(test_every_response_is_a_json_object_with_matching_length);
    RUN_TEST(test_log_fields_never_echo_the_request_body);
    return UNITY_END();
}

/*
 * kvm_ctrl: 不変条件を全 tick で検査するファズ(固定シード。ASan/UBSan で実行)。
 *
 * 乱数で、LED の変化(手動切替、無点灯、複数点灯)、映像の同期状態、切替・電源操作の要求(範囲外を含む)、
 * HAL の失敗の注入、フォールトからの再起動を与える。検査(すべての tick の後):
 *   1. モックの違反フラグが立たない(set_addr は全線 High のときだけ。assert_line は同時に 1 本まで。引数は範囲内)
 *   2. 同時に有効な線は 1 本以下
 *   3. フォールト後は assert_line / set_addr / release_line が 0 回で、tick ごとに release_all が 1 回。
 *      線は遅くともラッチの次の tick までに全部下がる
 *   4. すべての assert_line の max_ms は、種別の上限以下(ATX は kvm_logic のハード上限)で 1 以上
 *   5. 線が有効なのは、対応する kvm_logic の状態のときだけ(KVM: SWITCH_PULSE、ATX: ATX_PULSE かつ LED == 現在チャンネル)
 *   6. フォールトでなければ、ボードの A0/A1 が kvm_logic の出力と一致する
 *   7. 要求の結果が、状態から決まる期待値(WEB_API.md 7 章の判定順序の独立な実装)と一致する。
 *      受理したときだけ op_id が 1 進み、HAL は呼ばれない
 *   8. 実行中(pending)の操作は、対応する kvm_logic の状態にいる間だけ。フォールトなら pending は残らない
 * 加えて、ファズが空振りでないことを、受理・完了・中断・パルス・フォールトの件数の下限で確認する。
 */
#include "kc_common.h"

static uint64_t rs;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

typedef struct {
    unsigned select_ok, power_ok, already, rejected;
    unsigned op_completed, op_aborted;
    unsigned atx_pulses, kvm_pulses;
    unsigned faults, reboots;
    unsigned fault_by[8];
} stats_t;
static stats_t ST;

#define CHECK(cond, msg)                                                                                   \
    do {                                                                                                   \
        if (!(cond)) {                                                                                     \
            char b[200];                                                                                   \
            snprintf(b, sizeof(b), "seed=%d tick=%ld: %s", cur_seed, cur_tick, msg);                       \
            TEST_FAIL_MESSAGE(b);                                                                          \
        }                                                                                                  \
    } while (0)

static int cur_seed;
static long cur_tick;

static kvm_ctrl_result_t expect_select(const kvm_ctrl_status_t *st, uint8_t ch) {
    if (ch < 1 || ch > 4) return KVM_CTRL_ERR_INVALID_PARAM;
    if (st->fault) return KVM_CTRL_ERR_FAULT;
    if (st->state == KVM_CTRL_STATE_UNKNOWN) return KVM_CTRL_ERR_UNKNOWN_STATE;
    if (st->state == KVM_CTRL_STATE_IDLE && st->active_channel == ch) return KVM_CTRL_ALREADY_SELECTED;
    if (st->state != KVM_CTRL_STATE_IDLE) return KVM_CTRL_ERR_BUSY;
    return KVM_CTRL_OK;
}

static kvm_ctrl_result_t expect_power(const kvm_ctrl_status_t *st, uint8_t ch, int action) {
    if (ch < 1 || ch > 4 || action < 0 || action > 2) return KVM_CTRL_ERR_INVALID_PARAM;
    if (st->fault) return KVM_CTRL_ERR_FAULT;
    if (st->state == KVM_CTRL_STATE_UNKNOWN) return KVM_CTRL_ERR_UNKNOWN_STATE;
    if (st->state != KVM_CTRL_STATE_IDLE) return KVM_CTRL_ERR_BUSY;
    if (st->active_channel != ch) return KVM_CTRL_ERR_CHANNEL_MISMATCH;
    if (st->atx_locked) return KVM_CTRL_ERR_ATX_LOCKED;
    if (st->cooldown_ms > 0) return KVM_CTRL_ERR_COOLDOWN;
    return KVM_CTRL_OK;
}

static uint32_t line_cap(kvm_board_line_t line) {
    switch (line) {
        case KVM_BOARD_LINE_POWER:       return KVM_ATX_HARD_MAX_POWER_MS;
        case KVM_BOARD_LINE_POWER_FORCE: return KVM_ATX_HARD_MAX_FORCE_MS;
        case KVM_BOARD_LINE_RESET:       return KVM_ATX_HARD_MAX_RESET_MS;
        default:                         return UINT32_MAX;
    }
}

static void do_reboot(void) {
    mock_clear_failures(&M);
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());
    ST.reboots++;
}

static void run_seed(int seed, bool inject_faults, long ticks) {
    cur_seed = seed;
    rs = 88172645463325252ULL ^ ((uint64_t)seed * 0x9E3779B97F4A7C15ULL);
    fx_reset((uint8_t)(1 + rnd() % 4));
    /* 設定も少し振る(境界に近い値を含む) */
    if (rnd() % 2) CFG.press_ms = 1 + rnd() % KVM_ATX_HARD_MAX_POWER_MS;
    if (rnd() % 2) CFG.reset_ms = 1 + rnd() % KVM_ATX_HARD_MAX_RESET_MS;
    if (rnd() % 2) CFG.force_ms = 1 + rnd() % KVM_ATX_HARD_MAX_FORCE_MS;
    CFG.atx_min_interval_ms = (rnd() % 3 == 0) ? 0 : rnd() % 1500;
    CFG.assert_margin_ms = 1 + rnd() % 200;
    mock_set_led_channel(&M, (uint8_t)(1 + rnd() % 4));
    TEST_ASSERT_EQUAL(KVM_CTRL_OK, fx_init());

    bool prev_kvm_active = false;
    bool latched_seen = false;
    long latch_tick = 0;
    unsigned base_assert = 0, base_set_addr = 0, base_release_line = 0;
    unsigned prev_release_all = 0;
    uint32_t last_op_id = 0;
    kvm_ctrl_op_result_t prev_result = KVM_CTRL_OP_COMPLETED;
    uint32_t prev_result_op = 0;
    uint64_t now = 0;

    for (long i = 0; i < ticks; i++) {
        cur_tick = i;
        uint32_t dt = (rnd() % 50 == 0) ? 100 + rnd() % 200 : 1 + rnd() % 30;
        now += dt;

        /* ---- 入力 ---- */
        uint32_t x = rnd() % 1000;
        if (x < 6) {
            static const uint8_t masks[] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x03, 0x0F, 0x11, 0xFF, 0x05};
            mock_set_led_mask(&M, masks[rnd() % (sizeof(masks))]);
        } else if (x < 12) {
            mock_set_led_channel(&M, (uint8_t)(1 + rnd() % 4));
        }
        if (rnd() % 60 == 0) mock_set_sync(&M, (kvm_video_sync_state_t)(rnd() % 4)); /* 3 は範囲外 */
        /* KVM のボタンを押し終えたら、LED が A0/A1 の選択に変わる(ことが多い) */
        bool kvm_active = mock_line_active(&M, KVM_BOARD_LINE_KVM);
        if (prev_kvm_active && !kvm_active && rnd() % 100 < 85) mock_set_led_channel(&M, M.addr);
        prev_kvm_active = kvm_active;

        /* ---- HAL の失敗の注入 ---- */
        if (inject_faults && !latched_seen && rnd() % 3000 == 0) {
            bool armed = false;
            for (int f = 0; f < MOCK_FN_COUNT; f++) if (M.fail_in[f]) armed = true;
            if (!armed) {
                /* release_all は健全なときは呼ばれず、注入が残り続けるので対象にしない(単体テストで確認済み) */
                static const mock_fn_t fns[] = {MOCK_FN_SET_ADDR, MOCK_FN_ASSERT_LINE, MOCK_FN_RELEASE_LINE, MOCK_FN_READ_LED, MOCK_FN_READ_LED};
                mock_fail_nth(&M, fns[rnd() % 5], 1 + (int)(rnd() % 3));
            }
        }

        /* ---- tick(まず時刻を進めてから) ---- */
        /* 呼び出しの記録だけを空にする(calls[] の累積カウンターは残す)。記録は 1 tick 分だけ見ればよく、
         * 空にしないと 4096 件で満杯になり、以降の tick で max_ms の検査とパルスの集計が何も見なくなる */
        M.log_len = 0;
        M.log_dropped = 0;
        size_t log_before = M.log_len;
        kvm_ctrl_result_t tr = fx_tick(now);
        (void)tr;
        CHECK(M.log_dropped == 0, "mock log overflowed within one tick");
        kvm_ctrl_status_t st = fx_status();

        /* 件数 */
        if (st.fault && !latched_seen) {
            latched_seen = true;
            latch_tick = i;
            ST.faults++;
            ST.fault_by[st.fault_reason & 7]++;
            base_assert = mock_calls(&M, MOCK_FN_ASSERT_LINE);
            base_set_addr = mock_calls(&M, MOCK_FN_SET_ADDR);
            base_release_line = mock_calls(&M, MOCK_FN_RELEASE_LINE);
        }

        /* 1, 2 */
        CHECK(!mock_violated(&M), "HAL contract violated by kvm_ctrl");
        CHECK(mock_active_count(&M) <= 1, "more than one line active");

        /* 4 */
        for (size_t k = log_before; k < M.log_len; k++) {
            const mock_call_t *e = &M.log[k];
            if (e->fn == MOCK_FN_ASSERT_LINE) {
                CHECK(e->max_ms >= 1, "assert_line max_ms == 0");
                if (e->arg == KVM_BOARD_LINE_KVM) {
                    CHECK(e->max_ms <= (uint64_t)CFG.logic.switch_kvm_pulse_time_ms + CFG.assert_margin_ms, "KVM max_ms too long");
                    ST.kvm_pulses += (e->ret == 0);
                } else {
                    CHECK(e->max_ms <= line_cap((kvm_board_line_t)e->arg), "ATX max_ms over the hard max");
                    ST.atx_pulses += (e->ret == 0);
                }
            }
        }

        /* 3, 5, 6 */
        if (st.fault) {
            CHECK(mock_calls(&M, MOCK_FN_ASSERT_LINE) == base_assert, "assert_line after fault");
            CHECK(mock_calls(&M, MOCK_FN_SET_ADDR) == base_set_addr, "set_addr after fault");
            CHECK(mock_calls(&M, MOCK_FN_RELEASE_LINE) == base_release_line, "release_line after fault");
            if (i > latch_tick) {
                CHECK(mock_calls(&M, MOCK_FN_RELEASE_ALL) == prev_release_all + 1, "release_all must be retried every tick");
                CHECK(mock_active_count(&M) == 0, "line still active after latch");
            }
            CHECK(!(st.last_op.result == KVM_CTRL_OP_PENDING && st.has_last_op), "pending op after fault");
        } else {
            CHECK(M.addr == C.logic.current_outputs.a0a1_channel, "board A0/A1 differs from kvm_logic output");
            if (mock_line_active(&M, KVM_BOARD_LINE_KVM)) {
                CHECK(C.logic.state == KVM_STATE_SWITCH_PULSE, "KVM line active outside SWITCH_PULSE");
            }
            if (mock_line_active(&M, KVM_BOARD_LINE_POWER) || mock_line_active(&M, KVM_BOARD_LINE_POWER_FORCE) ||
                mock_line_active(&M, KVM_BOARD_LINE_RESET)) {
                CHECK(C.logic.state == KVM_STATE_ATX_PULSE, "ATX line active outside ATX_PULSE");
                CHECK(kvm_ctrl_led_mask_to_raw(M.led_mask) == C.logic.current_outputs.active_channel,
                      "ATX line active while LED != active channel");
            }
            /* 8 */
            if (st.has_last_op && st.last_op.result == KVM_CTRL_OP_PENDING) {
                if (st.last_op.kind == KVM_CTRL_OP_SELECT) {
                    CHECK(C.logic.state != KVM_STATE_IDLE, "pending select while idle");
                } else {
                    CHECK(C.logic.state == KVM_STATE_ATX_WAIT_SETUP || C.logic.state == KVM_STATE_ATX_PULSE,
                          "pending ATX op outside ATX states");
                }
            }
        }
        prev_release_all = mock_calls(&M, MOCK_FN_RELEASE_ALL);

        /* 操作の結果の遷移: pending -> completed / aborted だけ。件数を数える */
        if (st.has_last_op) {
            if (st.last_op.op_id == prev_result_op && prev_result == KVM_CTRL_OP_PENDING) {
                if (st.last_op.result == KVM_CTRL_OP_COMPLETED) ST.op_completed++;
                if (st.last_op.result == KVM_CTRL_OP_ABORTED) ST.op_aborted++;
            }
            if (st.last_op.op_id == prev_result_op) {
                CHECK(!(prev_result != KVM_CTRL_OP_PENDING && st.last_op.result != prev_result), "op result changed after it was decided");
            }
            prev_result = st.last_op.result;
            prev_result_op = st.last_op.op_id;
        }

        /* ---- 要求(tick の間に入る) ---- */
        if (rnd() % 100 < 12) {
            kvm_ctrl_status_t pre = fx_status();
            unsigned calls_before = 0;
            for (int f = 0; f < MOCK_FN_COUNT; f++) calls_before += mock_calls(&M, (mock_fn_t)f);
            uint32_t op = 12345;
            uint32_t next_before = C.next_op_id;
            kvm_ctrl_result_t got, want;
            if (rnd() % 2) {
                uint8_t ch = (rnd() % 8 == 0) ? (uint8_t)(rnd() % 7) : (uint8_t)(1 + rnd() % 4);
                want = expect_select(&pre, ch);
                got = kvm_ctrl_select(&C, ch, &op);
            } else {
                uint8_t ch = (rnd() % 4) ? pre.active_channel : (uint8_t)(rnd() % 7);
                int action = (rnd() % 10 == 0) ? 3 + (int)(rnd() % 3) : (int)(rnd() % 3);
                want = expect_power(&pre, ch, action);
                got = kvm_ctrl_power(&C, ch, (kvm_ctrl_power_action_t)action, &op);
            }
            CHECK(got == want, "request result differs from the WEB_API.md 7 order");
            unsigned calls_after = 0;
            for (int f = 0; f < MOCK_FN_COUNT; f++) calls_after += mock_calls(&M, (mock_fn_t)f);
            CHECK(calls_after == calls_before, "a request touched the HAL");
            if (got == KVM_CTRL_OK) {
                CHECK(op == next_before && C.next_op_id == next_before + 1, "op_id must advance by one on accept");
                CHECK(op > last_op_id, "op_id not increasing");
                last_op_id = op;
                kvm_ctrl_status_t post = fx_status();
                CHECK(post.has_last_op && post.last_op.op_id == op && post.last_op.result == KVM_CTRL_OP_PENDING,
                      "accepted op must be recorded as pending");
                if (post.last_op.kind == KVM_CTRL_OP_SELECT) ST.select_ok++; else ST.power_ok++;
                prev_result = KVM_CTRL_OP_PENDING;
                prev_result_op = op;
            } else {
                CHECK(op == 0 && C.next_op_id == next_before, "rejected request must not consume an op_id");
                if (got == KVM_CTRL_ALREADY_SELECTED) ST.already++; else ST.rejected++;
            }
        }

        /* ---- フォールトからの再起動(しばらく後) ---- */
        if (latched_seen && i - latch_tick > (long)(50 + rnd() % 250)) {
            do_reboot();
            latched_seen = false;
            prev_release_all = mock_calls(&M, MOCK_FN_RELEASE_ALL);
            prev_result = KVM_CTRL_OP_COMPLETED;
            prev_result_op = 0;
            last_op_id = 0;
        }
    }
}

void setUp(void) {}
void tearDown(void) {}

void test_fuzz_invariants_without_hal_failures(void) {
    memset(&ST, 0, sizeof(ST));
    for (int seed = 1; seed <= 150; seed++) run_seed(seed, false, 20000);
    printf("fuzz(no faults): select_ok=%u power_ok=%u already=%u rejected=%u completed=%u aborted=%u kvm_pulses=%u atx_pulses=%u faults=%u\n",
           ST.select_ok, ST.power_ok, ST.already, ST.rejected, ST.op_completed, ST.op_aborted, ST.kvm_pulses, ST.atx_pulses, ST.faults);
    TEST_ASSERT_TRUE_MESSAGE(ST.select_ok > 1000, "fuzz explored too few switches");
    TEST_ASSERT_TRUE_MESSAGE(ST.power_ok > 1000, "fuzz explored too few ATX operations");
    TEST_ASSERT_TRUE_MESSAGE(ST.kvm_pulses > 1000, "fuzz produced too few KVM pulses");
    TEST_ASSERT_TRUE_MESSAGE(ST.atx_pulses > 500, "fuzz produced too few ATX pulses");
    TEST_ASSERT_TRUE_MESSAGE(ST.op_completed > 500, "fuzz completed too few operations");
    TEST_ASSERT_TRUE_MESSAGE(ST.op_aborted > 100, "fuzz aborted too few operations");
    TEST_ASSERT_EQUAL_UINT(0, ST.faults); /* HAL が正常なら、kvm_logic の出力でフォールトにならない */
}

void test_fuzz_invariants_with_hal_failures(void) {
    memset(&ST, 0, sizeof(ST));
    for (int seed = 1001; seed <= 1150; seed++) run_seed(seed, true, 20000);
    printf("fuzz(faults): select_ok=%u power_ok=%u completed=%u aborted=%u kvm_pulses=%u atx_pulses=%u faults=%u reboots=%u (by reason: init=%u set_addr=%u assert=%u release_line=%u read_led=%u logic=%u rewind=%u)\n",
           ST.select_ok, ST.power_ok, ST.op_completed, ST.op_aborted, ST.kvm_pulses, ST.atx_pulses, ST.faults, ST.reboots,
           ST.fault_by[1], ST.fault_by[2], ST.fault_by[3], ST.fault_by[4], ST.fault_by[5], ST.fault_by[6], ST.fault_by[7]);
    TEST_ASSERT_TRUE_MESSAGE(ST.faults > 100, "fuzz injected too few faults");
    TEST_ASSERT_TRUE_MESSAGE(ST.fault_by[KVM_CTRL_FAULT_SET_ADDR] > 5, "no set_addr faults");
    TEST_ASSERT_TRUE_MESSAGE(ST.fault_by[KVM_CTRL_FAULT_ASSERT_LINE] > 5, "no assert_line faults");
    TEST_ASSERT_TRUE_MESSAGE(ST.fault_by[KVM_CTRL_FAULT_RELEASE_LINE] > 5, "no release_line faults");
    TEST_ASSERT_TRUE_MESSAGE(ST.fault_by[KVM_CTRL_FAULT_READ_LED] > 5, "no read_led faults");
    TEST_ASSERT_TRUE_MESSAGE(ST.atx_pulses > 500, "fuzz produced too few ATX pulses");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_invariants_without_hal_failures);
    RUN_TEST(test_fuzz_invariants_with_hal_failures);
    return UNITY_END();
}

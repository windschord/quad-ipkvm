#define _POSIX_C_SOURCE 200809L
#include "../kvm_ctrl/kc_common.h"
#include "ka_common.h"
#include <regex.h>
#include <stdlib.h>

/*
 * 固定シードのファズ(ASan/UBSan 下)。
 *  1. パーサー: 有効な本文を壊して(変異)、独立なオラクル(POSIX 正規表現)と結果を突き合わせる。
 *  2. kvm_api_handle: ランダムなエンドポイント・本文・backend の台本で、応答の不変条件を確かめる。
 *  3. 実際の kvm_ctrl + モック HAL に対するランダムな API 呼び出しと tick の列。
 *     不変条件: 電源操作が受理されるのは、要求の channel が「その時点の」現在チャンネルのときだけ。
 *     モックの違反フラグが立たない。拒否は何も出さない。
 */

void setUp(void) {}
void tearDown(void) {}

static uint32_t seed = 20260710u;
static uint32_t rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

/* ------------------------------------------------------------------ 1. パーサー */

static regex_t g_re;

/* オラクル: NUL を含まず、長さ 64 以下で、正規表現に一致するときだけ受理。値は ':' の後の最初の非空白文字 */
static int oracle(const char *s, size_t n, uint8_t *val) {
    if (n == 0 || n > 64) return -1;
    if (memchr(s, 0, n) != NULL) return -1;
    char tmp[80];
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    if (regexec(&g_re, tmp, 0, NULL, 0) != 0) return -1;
    const char *p = strchr(tmp, ':');
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    *val = (uint8_t)(*p - '0');
    return 0;
}

static size_t gen_valid(char *buf) {
    static const char WS[] = {' ', '\t', '\n', '\r'};
    size_t n = 0;
#define WSP() do { unsigned k = rnd() % 3; while (k--) buf[n++] = WS[rnd() % 4]; } while (0)
    WSP(); buf[n++] = '{'; WSP();
    memcpy(buf + n, "\"channel\"", 9); n += 9;
    WSP(); buf[n++] = ':'; WSP();
    buf[n++] = (char)('1' + rnd() % 4);
    WSP(); buf[n++] = '}'; WSP();
#undef WSP
    return n;
}

static const char ALPHA[] = "{}\":,0123456789.eE+-\\ \t\n\r\vchanl/x[]'\x7f\x80\xff";

static size_t mutate(char *buf, size_t n, size_t cap) {
    unsigned k = 1 + rnd() % 3;
    while (k--) {
        unsigned op = rnd() % 5;
        if (op == 0 && n > 0) { /* 置換 */
            buf[rnd() % n] = (rnd() % 4 == 0) ? (char)(rnd() & 0xFF) : ALPHA[rnd() % (sizeof(ALPHA) - 1)];
        } else if (op == 1 && n + 1 < cap) { /* 挿入 */
            size_t pos = rnd() % (n + 1);
            memmove(buf + pos + 1, buf + pos, n - pos);
            buf[pos] = (rnd() % 8 == 0) ? '\0' : ALPHA[rnd() % (sizeof(ALPHA) - 1)];
            n++;
        } else if (op == 2 && n > 0) { /* 削除 */
            size_t pos = rnd() % n;
            memmove(buf + pos, buf + pos + 1, n - pos - 1);
            n--;
        } else if (op == 3 && n > 0) { /* 切り詰め */
            n = rnd() % (n + 1);
        } else if (op == 4 && n * 2 < cap) { /* 重複 */
            size_t pos = rnd() % (n + 1);
            size_t len = rnd() % (n - pos + 1);
            memmove(buf + pos + len, buf + pos, n - pos);
            memcpy(buf + pos, buf + pos + len, len);
            n += len;
        }
    }
    return n;
}

void test_parser_matches_independent_oracle(void) {
    TEST_ASSERT_EQUAL_INT(0, regcomp(&g_re,
        "^[ \t\r\n]*\\{[ \t\r\n]*\"channel\"[ \t\r\n]*:[ \t\r\n]*[1-4][ \t\r\n]*\\}[ \t\r\n]*$", REG_EXTENDED | REG_NOSUB));
    long accepted = 0, rejected = 0;
    for (int it = 0; it < 300000; it++) {
        char buf[256];
        size_t n = gen_valid(buf);
        if (rnd() % 8 == 0) { /* 長さの境界を狙う: 空白で埋める */
            size_t target = 60 + rnd() % 8;
            while (n < target) buf[n++] = ' ';
        }
        if (rnd() % 10 != 0) n = mutate(buf, n, sizeof(buf));
        if (rnd() % 50 == 0) { /* 完全にランダム */
            n = rnd() % 80;
            for (size_t i = 0; i < n; i++) buf[i] = (char)(rnd() & 0xFF);
        }
        /* ちょうどの大きさのヒープに複製(ASan で読み過ぎを捕まえる) */
        char *exact = malloc(n ? n : 1);
        TEST_ASSERT_NOT_NULL(exact);
        if (n) memcpy(exact, buf, n);
        uint8_t got = 0xEE, want = 0xEE;
        int g = kvm_api_parse_channel(exact, n, &got);
        int w = oracle(buf, n, &want);
        free(exact);
        if (g != w || (g == 0 && got != want) || (g != 0 && got != 0xEE)) {
            char msg[200];
            snprintf(msg, sizeof(msg), "mismatch at iteration %d: impl=%d(%u) oracle=%d(%u) len=%u", it, g, got, w, want, (unsigned)n);
            TEST_FAIL_MESSAGE(msg);
        }
        if (g == 0) accepted++; else rejected++;
    }
    regfree(&g_re);
    /* 空振りでないこと */
    TEST_ASSERT_TRUE_MESSAGE(accepted > 20000, "fuzz must exercise accepted bodies");
    TEST_ASSERT_TRUE_MESSAGE(rejected > 20000, "fuzz must exercise rejected bodies");
}

/* ------------------------------------------------------------------ 2. handle(モック backend) */

void test_handle_invariants_with_random_backend(void) {
    static const kvm_ctrl_result_t RES[] = {
        KVM_CTRL_OK, KVM_CTRL_ALREADY_SELECTED, KVM_CTRL_ERR_INVALID_PARAM, KVM_CTRL_ERR_BUSY, KVM_CTRL_ERR_UNKNOWN_STATE,
        KVM_CTRL_ERR_CHANNEL_MISMATCH, KVM_CTRL_ERR_ATX_LOCKED, KVM_CTRL_ERR_COOLDOWN, KVM_CTRL_ERR_FAULT,
        KVM_CTRL_ERR_NOT_READY, (kvm_ctrl_result_t)42, (kvm_ctrl_result_t)-5,
    };
    long by_status[600] = {0};
    for (int it = 0; it < 200000; it++) {
        be_reset();
        B.ready_result = rnd() % 10 != 0;
        B.lock_result = rnd() % 10 != 0;
        B.select_ret = RES[rnd() % (sizeof(RES) / sizeof(RES[0]))];
        B.power_ret = RES[rnd() % (sizeof(RES) / sizeof(RES[0]))];
        B.status_ret = (rnd() % 8 == 0) ? RES[rnd() % (sizeof(RES) / sizeof(RES[0]))] : KVM_CTRL_OK;
        B.op_id_out = rnd();
        B.status.active_channel = (uint8_t)(rnd() % 8);
        B.status.cooldown_ms = (rnd() % 3 == 0) ? rnd() * 4096u : rnd() % 6000u;
        B.status.state = (kvm_ctrl_state_t)(rnd() % 7); /* 範囲外も含む */
        B.status.video_sync = (kvm_video_sync_state_t)(rnd() % 4);
        B.status.has_last_op = rnd() % 2;
        B.status.last_op.kind = (kvm_ctrl_op_kind_t)(rnd() % 5);
        B.status.last_op.result = (kvm_ctrl_op_result_t)(rnd() % 4);

        kvm_api_endpoint_t ep = (kvm_api_endpoint_t)(rnd() % 7); /* 範囲外(5, 6)も含む */
        char buf[100];
        size_t n;
        if (rnd() % 3 == 0) {
            n = rnd() % 90;
            for (size_t i = 0; i < n; i++) buf[i] = (char)(rnd() & 0xFF);
        } else {
            n = gen_valid(buf);
            if (rnd() % 3 == 0) n = mutate(buf, n, sizeof(buf));
        }
        char *exact = malloc(n ? n : 1);
        TEST_ASSERT_NOT_NULL(exact);
        if (n) memcpy(exact, buf, n);
        kvm_api_backend_t t = be_table();
        const kvm_api_backend_t *tp = (rnd() % 20 == 0) ? NULL : &t;
        memset(&RSP, 0xCD, sizeof(RSP));
        kvm_api_handle(tp, ep, exact, n, &RSP);

        uint8_t ch = 0xEE;
        bool body_ok = kvm_api_parse_channel(exact, n, &ch) == 0;
        free(exact);

        /* 応答の形 */
        TEST_ASSERT_TRUE(RSP.status == 200 || RSP.status == 202 || RSP.status == 400 || RSP.status == 409 || RSP.status == 429 ||
                         RSP.status == 500 || RSP.status == 503);
        TEST_ASSERT_EQUAL_UINT((unsigned)strlen(RSP.body), (unsigned)RSP.body_len);
        TEST_ASSERT_TRUE(RSP.body_len >= 2 && RSP.body_len < KVM_API_RESPONSE_MAX);
        TEST_ASSERT_EQUAL_CHAR('{', RSP.body[0]);
        TEST_ASSERT_EQUAL_CHAR('}', RSP.body[RSP.body_len - 1]);
        TEST_ASSERT_NOT_NULL(RSP.log_kind);
        TEST_ASSERT_NOT_NULL(RSP.log_reason);
        TEST_ASSERT_EQUAL(RSP.status == 429, RSP.has_retry_after);
        if (RSP.has_retry_after) TEST_ASSERT_TRUE(RSP.retry_after_s >= 1);
        else TEST_ASSERT_EQUAL_UINT32(0, RSP.retry_after_s);
        TEST_ASSERT_TRUE(RSP.log_channel <= 4);

        /* backend の使い方 */
        TEST_ASSERT_FALSE(B.viol_call_without_lock);
        TEST_ASSERT_FALSE(B.viol_double_lock);
        TEST_ASSERT_FALSE(B.viol_unlock_without_lock);
        TEST_ASSERT_FALSE(B.held);
        TEST_ASSERT_EQUAL_INT(B.lock_result ? B.lock_calls : 0, B.unlock_calls);
        TEST_ASSERT_TRUE(B.lock_calls <= 1);

        bool is_post = (ep == KVM_API_EP_SELECT || ep == KVM_API_EP_POWER_PRESS || ep == KVM_API_EP_POWER_RESET ||
                        ep == KVM_API_EP_POWER_FORCE);
        if ((unsigned)ep > (unsigned)KVM_API_EP_POWER_FORCE) {
            TEST_ASSERT_EQUAL_INT(500, RSP.status);
            TEST_ASSERT_EQUAL_INT(0, B.lock_calls);
        } else if (is_post && !body_ok) {
            /* 形式の不正は 400 で、backend に触らない */
            TEST_ASSERT_EQUAL_INT(400, RSP.status);
            TEST_ASSERT_EQUAL_STRING("{\"error\":\"invalid_request\"}", RSP.body);
            TEST_ASSERT_EQUAL_INT(0, B.ready_calls + B.lock_calls + B.select_calls + B.power_calls + B.status_calls);
        }
        if (is_post && body_ok) {
            TEST_ASSERT_EQUAL_UINT8(ch, RSP.log_channel);
            if (B.select_calls + B.power_calls > 0) {
                TEST_ASSERT_EQUAL_UINT8(ch, B.last_channel);
                TEST_ASSERT_EQUAL_INT(1, B.select_calls + B.power_calls);
                TEST_ASSERT_EQUAL_INT(ep == KVM_API_EP_SELECT ? 1 : 0, B.select_calls);
            }
            if (RSP.status == 202) {
                TEST_ASSERT_TRUE(ep == KVM_API_EP_SELECT ? B.select_ret == KVM_CTRL_OK : B.power_ret == KVM_CTRL_OK);
            }
            if (!B.ready_result || !B.lock_result || !tp) {
                TEST_ASSERT_EQUAL_INT(503, RSP.status);
                TEST_ASSERT_EQUAL_INT(0, B.select_calls + B.power_calls);
            }
        }
        if (ep == KVM_API_EP_STATUS) {
            TEST_ASSERT_EQUAL_INT(0, B.select_calls + B.power_calls);
            TEST_ASSERT_TRUE(RSP.status == 200 || RSP.status == 500 || RSP.status == 503);
            if (RSP.status == 200) TEST_ASSERT_FALSE(RSP.log);
        }
        by_status[RSP.status]++;
    }
    /* 空振りでないこと: 全種類のステータスが出ている */
    TEST_ASSERT_TRUE(by_status[200] > 100 && by_status[202] > 100 && by_status[400] > 100 && by_status[409] > 100);
    TEST_ASSERT_TRUE(by_status[429] > 100 && by_status[500] > 100 && by_status[503] > 100);
}

/* ------------------------------------------------------------------ 3. 実際の kvm_ctrl + モック HAL */

static bool f_held;
static bool fb_ready(void *c) { (void)c; return true; }
static bool fb_lock(void *c) { (void)c; if (f_held) TEST_FAIL_MESSAGE("double lock"); f_held = true; return true; }
static void fb_unlock(void *c) { (void)c; if (!f_held) TEST_FAIL_MESSAGE("unlock without lock"); f_held = false; }
static kvm_ctrl_result_t fb_status(void *c, kvm_ctrl_status_t *s) { (void)c; return kvm_ctrl_get_status(&C, s); }
static kvm_ctrl_result_t fb_select(void *c, uint8_t ch, uint32_t *op) { (void)c; return kvm_ctrl_select(&C, ch, op); }
static kvm_ctrl_result_t fb_power(void *c, uint8_t ch, kvm_ctrl_power_action_t a, uint32_t *op) {
    (void)c;
    return kvm_ctrl_power(&C, ch, a, op);
}

static int json_int_after(const char *body, const char *key) {
    const char *p = strstr(body, key);
    if (!p) return -1;
    p += strlen(key);
    if (strncmp(p, "null", 4) == 0) return 0;
    return atoi(p);
}

void test_random_api_sequences_on_real_ctrl(void) {
    long accepted_power = 0, mismatch = 0, accepted_select = 0, cooldowns = 0;
    kvm_api_backend_t t;
    t.ctx = NULL;
    t.lock = fb_lock;
    t.unlock = fb_unlock;
    t.ready = fb_ready;
    t.get_status = fb_status;
    t.select = fb_select;
    t.power = fb_power;

    for (int run = 0; run < 60; run++) {
        f_held = false;
        fx_boot(1 + rnd() % 4);
        for (int step = 0; step < 4000; step++) {
            unsigned what = rnd() % 16;
            if (what < 7) { /* 時間を進める */
                if (rnd() % 20 == 0) mock_set_led_mask(&M, (uint8_t)(rnd() & 0x0F));
                if (rnd() % 40 == 0) fx_tick(T + 1500 + rnd() % 3000); /* 長い間隔(クールダウンの満了) */
                else fx_step();
                if (C.logic.state == KVM_STATE_SWITCH_WAIT_LED && rnd() % 2) {
                    mock_set_led_channel(&M, 1 + rnd() % 4);
                }
            } else if (what < 10) { /* select */
                char body[32];
                unsigned ch = 1 + rnd() % 4;
                snprintf(body, sizeof(body), "{\"channel\":%u}", ch);
                kvm_api_response_t r;
                kvm_api_handle(&t, KVM_API_EP_SELECT, body, strlen(body), &r);
                TEST_ASSERT_FALSE(f_held);
                if (r.status == 202) accepted_select++;
            } else if (what < 15) { /* power */
                kvm_ctrl_status_t before = fx_status();
                unsigned ch = 1 + rnd() % 4;
                char body[32];
                snprintf(body, sizeof(body), "{\"channel\":%u}", ch);
                kvm_api_endpoint_t ep = (kvm_api_endpoint_t)(KVM_API_EP_POWER_PRESS + rnd() % 3);
                uint32_t next_before = C.next_op_id;
                kvm_api_response_t r;
                kvm_api_handle(&t, ep, body, strlen(body), &r);
                TEST_ASSERT_FALSE(f_held);
                if (r.status == 202) {
                    /* 受理されたのは、その時点の現在チャンネルへの操作だけ */
                    TEST_ASSERT_EQUAL(KVM_CTRL_STATE_IDLE, before.state);
                    TEST_ASSERT_EQUAL_UINT8(ch, before.active_channel);
                    TEST_ASSERT_FALSE(before.fault);
                    TEST_ASSERT_EQUAL_UINT32(next_before + 1, C.next_op_id);
                    accepted_power++;
                } else {
                    TEST_ASSERT_EQUAL_UINT32(next_before, C.next_op_id); /* 拒否は何も始めない */
                }
                if (r.status == 409 && strstr(r.body, "channel_mismatch")) {
                    mismatch++;
                    TEST_ASSERT_TRUE(ch != before.active_channel);
                    TEST_ASSERT_EQUAL_INT(before.active_channel, json_int_after(r.body, "\"active_channel\":"));
                }
                if (r.status == 429) {
                    cooldowns++;
                    TEST_ASSERT_TRUE(r.retry_after_s >= 1);
                    TEST_ASSERT_TRUE(r.retry_after_s == (before.cooldown_ms + 999) / 1000);
                }
            } else { /* status */
                kvm_api_response_t r;
                kvm_api_handle(&t, KVM_API_EP_STATUS, NULL, 0, &r);
                TEST_ASSERT_EQUAL_INT(200, r.status);
                TEST_ASSERT_FALSE(f_held);
            }
            TEST_ASSERT_FALSE_MESSAGE(mock_violated(&M), "kvm_api must not make kvm_ctrl violate the board rules");
            TEST_ASSERT_TRUE(mock_active_count(&M) <= 1);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(accepted_power > 200, "fuzz must accept some power ops");
    TEST_ASSERT_TRUE_MESSAGE(accepted_select > 200, "fuzz must accept some selects");
    TEST_ASSERT_TRUE_MESSAGE(mismatch > 200, "fuzz must hit channel mismatches");
    TEST_ASSERT_TRUE_MESSAGE(cooldowns > 20, "fuzz must hit cooldowns");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parser_matches_independent_oracle);
    RUN_TEST(test_handle_invariants_with_random_backend);
    RUN_TEST(test_random_api_sequences_on_real_ctrl);
    return UNITY_END();
}

#ifndef KA_COMMON_H
#define KA_COMMON_H

/*
 * kvm_api のテスト共通の部品: 台本どおりに答えるモック backend と、応答の確認の道具。
 * 各テストファイルが include する（実行ファイルごとに別のコピー）。
 *
 * モック backend が固定すること（違反フラグとして残し、テストが検査する）:
 *   - get_status / select / power は lock を持っている間だけ呼ばれる
 *   - lock は二重に取られない
 *   - unlock は lock が true を返したときだけ、ちょうど 1 回呼ばれる（lock_calls と unlock_calls が揃う）
 */

#include "unity.h"
#include "kvm_api.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    /* 台本 */
    bool ready_result;
    bool lock_result;
    kvm_ctrl_result_t status_ret;
    kvm_ctrl_status_t status;
    kvm_ctrl_result_t select_ret;
    kvm_ctrl_result_t power_ret;
    uint32_t op_id_out;

    /* 記録 */
    int ready_calls, lock_calls, unlock_calls, status_calls, select_calls, power_calls;
    bool held;
    uint8_t last_channel;
    kvm_ctrl_power_action_t last_action;

    /* 違反 */
    bool viol_call_without_lock;
    bool viol_double_lock;
    bool viol_unlock_without_lock;
} mock_be_t;

static mock_be_t B;

static inline bool be_ready(void *ctx) { mock_be_t *m = ctx; m->ready_calls++; return m->ready_result; }
static inline bool be_lock(void *ctx) {
    mock_be_t *m = ctx;
    m->lock_calls++;
    if (!m->lock_result) return false;
    if (m->held) m->viol_double_lock = true;
    m->held = true;
    return true;
}
static inline void be_unlock(void *ctx) {
    mock_be_t *m = ctx;
    m->unlock_calls++;
    if (!m->held) m->viol_unlock_without_lock = true;
    m->held = false;
}
static inline kvm_ctrl_result_t be_get_status(void *ctx, kvm_ctrl_status_t *st) {
    mock_be_t *m = ctx;
    m->status_calls++;
    if (!m->held) m->viol_call_without_lock = true;
    *st = m->status;
    return m->status_ret;
}
static inline kvm_ctrl_result_t be_select(void *ctx, uint8_t ch, uint32_t *op_id) {
    mock_be_t *m = ctx;
    m->select_calls++;
    if (!m->held) m->viol_call_without_lock = true;
    m->last_channel = ch;
    *op_id = m->op_id_out;
    return m->select_ret;
}
static inline kvm_ctrl_result_t be_power(void *ctx, uint8_t ch, kvm_ctrl_power_action_t a, uint32_t *op_id) {
    mock_be_t *m = ctx;
    m->power_calls++;
    if (!m->held) m->viol_call_without_lock = true;
    m->last_channel = ch;
    m->last_action = a;
    *op_id = m->op_id_out;
    return m->power_ret;
}

static inline kvm_api_backend_t be_table(void) {
    kvm_api_backend_t t;
    t.ctx = &B;
    t.lock = be_lock;
    t.unlock = be_unlock;
    t.ready = be_ready;
    t.get_status = be_get_status;
    t.select = be_select;
    t.power = be_power;
    return t;
}

/* 正常な台本（ready、lock 成功、idle ch2、すべて OK） */
static inline void be_reset(void) {
    memset(&B, 0, sizeof(B));
    B.ready_result = true;
    B.lock_result = true;
    B.status_ret = KVM_CTRL_OK;
    B.status.state = KVM_CTRL_STATE_IDLE;
    B.status.active_channel = 2;
    B.status.video_sync = KVM_VIDEO_SYNC_LOCKED;
    B.select_ret = KVM_CTRL_OK;
    B.power_ret = KVM_CTRL_OK;
    B.op_id_out = 8;
}

/* lock と unlock の対応、lock の外で呼ばれていないこと */
static inline void be_assert_balanced(void) {
    TEST_ASSERT_FALSE(B.viol_call_without_lock);
    TEST_ASSERT_FALSE(B.viol_double_lock);
    TEST_ASSERT_FALSE(B.viol_unlock_without_lock);
    TEST_ASSERT_FALSE(B.held);
    /* lock が成功した回数 == unlock の回数 */
    int ok_locks = B.lock_result ? B.lock_calls : 0;
    TEST_ASSERT_EQUAL_INT(ok_locks, B.unlock_calls);
}

static kvm_api_response_t RSP;

/* 文字列のボディで kvm_api_handle を呼ぶ（backend は B） */
static inline void call(kvm_api_endpoint_t ep, const char *body) {
    kvm_api_backend_t t = be_table();
    memset(&RSP, 0xAB, sizeof(RSP));
    kvm_api_handle(&t, ep, body, body ? strlen(body) : 0, &RSP);
}

/* 応答の確認 */
static inline void expect_rsp(int status, const char *body) {
    TEST_ASSERT_EQUAL_INT(status, RSP.status);
    TEST_ASSERT_EQUAL_STRING(body, RSP.body);
    TEST_ASSERT_EQUAL_UINT((unsigned)strlen(body), (unsigned)RSP.body_len);
}

static inline void expect_log(bool log, const char *kind, int channel, const char *reason) {
    TEST_ASSERT_EQUAL(log, RSP.log);
    TEST_ASSERT_EQUAL_STRING(kind, RSP.log_kind);
    TEST_ASSERT_EQUAL_INT(channel, RSP.log_channel);
    TEST_ASSERT_EQUAL_STRING(reason, RSP.log_reason);
}

#endif /* KA_COMMON_H */

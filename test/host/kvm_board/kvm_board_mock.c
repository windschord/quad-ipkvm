#include "kvm_board_mock.h"
#include <string.h>

uint32_t mock_line_cap_ms(kvm_board_line_t line) {
    switch (line) {
        case KVM_BOARD_LINE_POWER:       return MOCK_MAX_POWER_MS;
        case KVM_BOARD_LINE_POWER_FORCE: return MOCK_MAX_POWER_FORCE_MS;
        case KVM_BOARD_LINE_RESET:       return MOCK_MAX_RESET_MS;
        case KVM_BOARD_LINE_KVM:         return MOCK_MAX_KVM_MS;
        default:                         return 0;
    }
}

static bool valid_line(int line) {
    return line >= 0 && line < (int)KVM_BOARD_LINE_COUNT;
}

/* 呼び出しを記録し、注入された失敗なら true を返す */
static bool record_and_check_failure(kvm_board_mock_t *m, mock_fn_t fn, int arg, uint32_t max_ms, int *ret_slot) {
    m->calls[fn]++;
    bool fail = m->fail_all[fn];
    if (m->fail_in[fn] > 0) {
        m->fail_in[fn]--;
        if (m->fail_in[fn] == 0) fail = true;
    }
    if (m->log_len < MOCK_LOG_MAX) {
        mock_call_t *e = &m->log[m->log_len++];
        memset(e, 0, sizeof(*e));
        e->fn = fn;
        e->arg = arg;
        e->max_ms = max_ms;
        e->at_ms = m->now_ms;
        e->ret = 0;
        *ret_slot = (int)(m->log_len - 1);
    } else {
        m->log_dropped++;
        *ret_slot = -1;
    }
    return fail;
}

static void set_ret(kvm_board_mock_t *m, int slot, int ret, uint32_t effective_ms) {
    if (slot < 0) return;
    m->log[slot].ret = ret;
    m->log[slot].effective_ms = effective_ms;
}

static int hal_set_addr(void *ctx, uint8_t channel) {
    kvm_board_mock_t *m = (kvm_board_mock_t *)ctx;
    int slot;
    bool fail = record_and_check_failure(m, MOCK_FN_SET_ADDR, channel, 0, &slot);
    if (mock_active_count(m) > 0) m->viol_set_addr_while_active = true;
    if (channel < 1 || channel > 4) m->viol_bad_arg = true;
    if (mock_active_count(m) > 0 || channel < 1 || channel > 4 || fail) {
        set_ret(m, slot, -1, 0);
        return -1;
    }
    m->addr = channel;
    set_ret(m, slot, 0, 0);
    return 0;
}

static int hal_assert_line(void *ctx, kvm_board_line_t line, uint32_t max_ms) {
    kvm_board_mock_t *m = (kvm_board_mock_t *)ctx;
    int slot;
    bool fail = record_and_check_failure(m, MOCK_FN_ASSERT_LINE, (int)line, max_ms, &slot);
    if (!valid_line((int)line)) {
        m->viol_bad_arg = true;
        set_ret(m, slot, -1, 0);
        return -1;
    }
    if (max_ms == 0) m->viol_zero_max_ms = true;
    if (mock_active_count(m) > 0) {
        m->viol_second_assert = true;
        set_ret(m, slot, -1, 0);
        return -1;
    }
    if (fail) {
        set_ret(m, slot, -1, 0);
        return -1;
    }
    uint32_t cap = mock_line_cap_ms(line);
    uint32_t eff = (max_ms == 0 || max_ms > cap) ? cap : max_ms;
    m->active[line] = true;
    m->deadline_ms[line] = m->now_ms + eff;
    set_ret(m, slot, 0, eff);
    return 0;
}

static int hal_release_line(void *ctx, kvm_board_line_t line) {
    kvm_board_mock_t *m = (kvm_board_mock_t *)ctx;
    int slot;
    bool fail = record_and_check_failure(m, MOCK_FN_RELEASE_LINE, (int)line, 0, &slot);
    if (!valid_line((int)line)) {
        m->viol_bad_arg = true;
        set_ret(m, slot, -1, 0);
        return -1;
    }
    if (fail) {
        set_ret(m, slot, -1, 0);
        return -1;
    }
    m->active[line] = false;
    set_ret(m, slot, 0, 0);
    return 0;
}

static int hal_release_all(void *ctx) {
    kvm_board_mock_t *m = (kvm_board_mock_t *)ctx;
    int slot;
    bool fail = record_and_check_failure(m, MOCK_FN_RELEASE_ALL, 0, 0, &slot);
    if (fail) {
        set_ret(m, slot, -1, 0);
        return -1;
    }
    for (int i = 0; i < (int)KVM_BOARD_LINE_COUNT; i++) m->active[i] = false;
    set_ret(m, slot, 0, 0);
    return 0;
}

static int hal_read_led_mask(void *ctx, uint8_t *mask) {
    kvm_board_mock_t *m = (kvm_board_mock_t *)ctx;
    int slot;
    bool fail = record_and_check_failure(m, MOCK_FN_READ_LED, 0, 0, &slot);
    if (!mask) {
        m->viol_bad_arg = true;
        set_ret(m, slot, -1, 0);
        return -1;
    }
    if (fail) {
        set_ret(m, slot, -1, 0);
        return -1;
    }
    if (m->chatter) {
        *mask = m->chatter_phase ? m->chatter_b : m->chatter_a;
        m->chatter_phase = !m->chatter_phase;
    } else {
        *mask = m->led_mask;
    }
    set_ret(m, slot, 0, 0);
    return 0;
}

static kvm_video_sync_state_t hal_video_sync(void *ctx) {
    kvm_board_mock_t *m = (kvm_board_mock_t *)ctx;
    int slot;
    (void)record_and_check_failure(m, MOCK_FN_VIDEO_SYNC, 0, 0, &slot);
    set_ret(m, slot, 0, 0);
    return m->sync;
}

void mock_init(kvm_board_mock_t *m, uint8_t initial_addr) {
    memset(m, 0, sizeof(*m));
    m->addr = initial_addr;
    m->sync = KVM_VIDEO_SYNC_UNKNOWN;
}

kvm_board_hal_t mock_hal(kvm_board_mock_t *m) {
    kvm_board_hal_t h;
    h.ctx = m;
    h.set_addr = hal_set_addr;
    h.assert_line = hal_assert_line;
    h.release_line = hal_release_line;
    h.release_all = hal_release_all;
    h.read_led_mask = hal_read_led_mask;
    h.video_sync = hal_video_sync;
    return h;
}

void mock_advance(kvm_board_mock_t *m, uint64_t now_ms) {
    if (now_ms > m->now_ms) m->now_ms = now_ms;
    for (int i = 0; i < (int)KVM_BOARD_LINE_COUNT; i++) {
        if (m->active[i] && m->now_ms >= m->deadline_ms[i]) {
            m->active[i] = false;
            m->auto_releases++;
        }
    }
}

bool mock_line_active(const kvm_board_mock_t *m, kvm_board_line_t line) {
    return valid_line((int)line) && m->active[line];
}

int mock_active_count(const kvm_board_mock_t *m) {
    int n = 0;
    for (int i = 0; i < (int)KVM_BOARD_LINE_COUNT; i++) if (m->active[i]) n++;
    return n;
}

bool mock_violated(const kvm_board_mock_t *m) {
    return m->viol_set_addr_while_active || m->viol_second_assert || m->viol_bad_arg || m->viol_zero_max_ms;
}

void mock_set_led_mask(kvm_board_mock_t *m, uint8_t mask) { m->led_mask = mask; }
void mock_set_led_channel(kvm_board_mock_t *m, uint8_t channel) {
    m->led_mask = (channel >= 1 && channel <= 4) ? (uint8_t)(1u << (channel - 1)) : 0;
}
void mock_set_led_off(kvm_board_mock_t *m) { m->led_mask = 0; }
void mock_set_led_multi(kvm_board_mock_t *m) { m->led_mask = 0x03; }
void mock_set_led_chatter(kvm_board_mock_t *m, uint8_t mask_a, uint8_t mask_b) {
    m->chatter = true;
    m->chatter_a = mask_a;
    m->chatter_b = mask_b;
    m->chatter_phase = false;
}
void mock_clear_led_chatter(kvm_board_mock_t *m) { m->chatter = false; }
void mock_set_sync(kvm_board_mock_t *m, kvm_video_sync_state_t s) { m->sync = s; }

void mock_fail_nth(kvm_board_mock_t *m, mock_fn_t fn, int n) { m->fail_in[fn] = n; }
void mock_fail_all(kvm_board_mock_t *m, mock_fn_t fn, bool on) { m->fail_all[fn] = on; }
void mock_clear_failures(kvm_board_mock_t *m) {
    memset(m->fail_in, 0, sizeof(m->fail_in));
    memset(m->fail_all, 0, sizeof(m->fail_all));
}

void mock_reset_calls(kvm_board_mock_t *m) {
    m->log_len = 0;
    m->log_dropped = 0;
    memset(m->calls, 0, sizeof(m->calls));
}

unsigned mock_calls(const kvm_board_mock_t *m, mock_fn_t fn) { return m->calls[fn]; }

const mock_call_t *mock_log_at(const kvm_board_mock_t *m, size_t index) {
    return index < m->log_len ? &m->log[index] : NULL;
}

unsigned mock_count_since(const kvm_board_mock_t *m, size_t index, mock_fn_t fn) {
    unsigned n = 0;
    for (size_t i = index; i < m->log_len; i++) if (m->log[i].fn == fn) n++;
    return n;
}

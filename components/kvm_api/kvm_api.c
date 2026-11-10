#include "kvm_api.h"
#include <string.h>

/* ------------------------------------------------------------------------------------------------
 * ボディの解析（厳密）
 * ---------------------------------------------------------------------------------------------- */

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static void skip_ws(const char *s, size_t len, size_t *i) {
    while (*i < len && is_ws(s[*i])) (*i)++;
}

/* s[*i] が c なら進めて true */
static bool expect_char(const char *s, size_t len, size_t *i, char c) {
    if (*i < len && s[*i] == c) {
        (*i)++;
        return true;
    }
    return false;
}

int kvm_api_parse_channel(const char *body, size_t body_len, uint8_t *channel) {
    static const char key[] = "\"channel\"";
    const size_t key_len = sizeof(key) - 1;

    if (!body || !channel || body_len == 0 || body_len > KVM_API_BODY_MAX) return -1;

    size_t i = 0;
    skip_ws(body, body_len, &i);
    if (!expect_char(body, body_len, &i, '{')) return -1;
    skip_ws(body, body_len, &i);
    /* キーは "channel" の 9 文字そのもの（エスケープした綴りは受け付けない）。ほかのキー・重複キーはここか、
     * 値の後の '}' の検査で落ちる（キーは 1 つだけ） */
    if (body_len - i < key_len || memcmp(body + i, key, key_len) != 0) return -1;
    i += key_len;
    skip_ws(body, body_len, &i);
    if (!expect_char(body, body_len, &i, ':')) return -1;
    skip_ws(body, body_len, &i);

    /* 値は '1'..'4' の 1 文字だけ。先頭ゼロ・負数・符号・文字列・2 桁以上・小数・指数は、
     * ここで落ちるか、次の '}' の検査で落ちる（"10"、"3.0"、"3e0" は 2 文字目が空白でも '}' でもない） */
    if (i >= body_len || body[i] < '1' || body[i] > '4') return -1;
    uint8_t value = (uint8_t)(body[i] - '0');
    i++;

    skip_ws(body, body_len, &i);
    if (!expect_char(body, body_len, &i, '}')) return -1;
    skip_ws(body, body_len, &i);
    if (i != body_len) return -1; /* '}' の後に空白以外がある（NUL を含む） */

    *channel = value;
    return 0;
}

/* ------------------------------------------------------------------------------------------------
 * JSON の書き出し（固定バッファ。溢れたら ok = false）
 * ---------------------------------------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ok;
} writer_t;

static void w_init(writer_t *w, char *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ok = (buf != NULL && cap > 0);
    if (w->ok) buf[0] = '\0';
}

static void w_str(writer_t *w, const char *s) {
    if (!w->ok) return;
    size_t n = strlen(s);
    if (n >= w->cap - w->len) { /* NUL の 1 バイトを残す */
        w->ok = false;
        return;
    }
    memcpy(w->buf + w->len, s, n + 1);
    w->len += n;
}

static void w_u32(writer_t *w, uint32_t v) {
    char tmp[11];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0);
    char out[11];
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    w_str(w, out);
}

/* 失敗なら出力を空にして -1 */
static int w_finish(writer_t *w, size_t *out_len) {
    if (!w->ok) {
        if (w->buf && w->cap > 0) w->buf[0] = '\0';
        if (out_len) *out_len = 0;
        return -1;
    }
    if (out_len) *out_len = w->len;
    return 0;
}

/* ------------------------------------------------------------------------------------------------
 * 状態の JSON（WEB_API.md 7.1）
 * ---------------------------------------------------------------------------------------------- */

static const char *state_name(kvm_ctrl_state_t s) {
    switch (s) {
        case KVM_CTRL_STATE_UNKNOWN:   return "unknown";
        case KVM_CTRL_STATE_IDLE:      return "idle";
        case KVM_CTRL_STATE_SWITCHING: return "switching";
        case KVM_CTRL_STATE_TRACKING:  return "tracking";
        case KVM_CTRL_STATE_ATX:       return "atx";
        default:                       return NULL;
    }
}

static const char *sync_name(kvm_video_sync_state_t s) {
    switch (s) {
        case KVM_VIDEO_SYNC_UNKNOWN:   return "unknown";
        case KVM_VIDEO_SYNC_NO_SIGNAL: return "no_signal";
        case KVM_VIDEO_SYNC_LOCKED:    return "locked";
        default:                       return NULL;
    }
}

static const char *op_kind_name(kvm_ctrl_op_kind_t k) {
    switch (k) {
        case KVM_CTRL_OP_SELECT:      return "select";
        case KVM_CTRL_OP_POWER_PRESS: return "power_press";
        case KVM_CTRL_OP_POWER_RESET: return "power_reset";
        case KVM_CTRL_OP_POWER_FORCE: return "power_force";
        default:                      return NULL;
    }
}

static const char *op_result_name(kvm_ctrl_op_result_t r) {
    switch (r) {
        case KVM_CTRL_OP_PENDING:   return "pending";
        case KVM_CTRL_OP_COMPLETED: return "completed";
        case KVM_CTRL_OP_ABORTED:   return "aborted";
        default:                    return NULL;
    }
}

static void w_bool(writer_t *w, bool v) { w_str(w, v ? "true" : "false"); }

int kvm_api_format_status(const kvm_ctrl_status_t *st, char *out, size_t out_cap, size_t *out_len) {
    writer_t w;
    w_init(&w, out, out_cap);
    if (!st) w.ok = false;
    if (!w.ok) return w_finish(&w, out_len);

    const char *state = state_name(st->state);
    const char *sync = sync_name(st->video_sync);
    if (!state || !sync) {
        w.ok = false;
        return w_finish(&w, out_len);
    }

    w_str(&w, "{\"state\":\"");
    w_str(&w, state);
    w_str(&w, "\",\"active_channel\":");
    if (st->state == KVM_CTRL_STATE_UNKNOWN || st->active_channel < 1 || st->active_channel > 4) {
        w_str(&w, "null");
    } else {
        w_u32(&w, st->active_channel);
    }
    w_str(&w, ",\"video_sync\":\"");
    w_str(&w, sync);
    w_str(&w, "\",\"atx_locked\":");
    w_bool(&w, st->atx_locked);
    w_str(&w, ",\"atx_anomaly\":");
    w_bool(&w, st->atx_anomaly);
    w_str(&w, ",\"fault\":");
    w_bool(&w, st->fault);
    w_str(&w, ",\"cooldown_ms\":");
    w_u32(&w, st->cooldown_ms);
    w_str(&w, ",\"last_op\":");
    if (!st->has_last_op) {
        w_str(&w, "null");
    } else {
        const char *kind = op_kind_name(st->last_op.kind);
        const char *res = op_result_name(st->last_op.result);
        if (!kind || !res) {
            w.ok = false;
            return w_finish(&w, out_len);
        }
        w_str(&w, "{\"op_id\":");
        w_u32(&w, st->last_op.op_id);
        w_str(&w, ",\"kind\":\"");
        w_str(&w, kind);
        w_str(&w, "\",\"channel\":");
        w_u32(&w, st->last_op.channel);
        w_str(&w, ",\"result\":\"");
        w_str(&w, res);
        w_str(&w, "\"}");
    }
    w_str(&w, "}");
    return w_finish(&w, out_len);
}

/* ------------------------------------------------------------------------------------------------
 * 応答の組み立て
 * ---------------------------------------------------------------------------------------------- */

static const char *endpoint_kind(kvm_api_endpoint_t ep) {
    switch (ep) {
        case KVM_API_EP_STATUS:      return "status";
        case KVM_API_EP_SELECT:      return "select";
        case KVM_API_EP_POWER_PRESS: return "power_press";
        case KVM_API_EP_POWER_RESET: return "power_reset";
        case KVM_API_EP_POWER_FORCE: return "power_force";
        default:                     return NULL;
    }
}

/* {"error":"<code>"} と、ログの内容。code は静的文字列 */
static void set_error(kvm_api_response_t *r, int status, const char *code) {
    writer_t w;
    w_init(&w, r->body, sizeof(r->body));
    w_str(&w, "{\"error\":\"");
    w_str(&w, code);
    w_str(&w, "\"}");
    r->status = status;
    r->body_len = w.len;
    r->log_reason = code;
}

/* 500。想定外の結果（バグ）。何も実行したことにしない */
static void set_internal_error(kvm_api_response_t *r) {
    set_error(r, 500, "internal_error");
}

static void set_not_ready(kvm_api_response_t *r) {
    set_error(r, 503, "not_ready");
}

static bool backend_usable(const kvm_api_backend_t *b) {
    return b && b->lock && b->unlock && b->ready && b->get_status && b->select && b->power && b->ready(b->ctx);
}

/* ------------------------------------------------------------------------------------------------
 * GET /api/v1/kvm/status
 * ---------------------------------------------------------------------------------------------- */

static void handle_status(const kvm_api_backend_t *b, kvm_api_response_t *r) {
    if (!backend_usable(b) || !b->lock(b->ctx)) {
        set_not_ready(r); /* 未起動の間もポーリングされるので、記録しない */
        return;
    }
    kvm_ctrl_status_t st;
    memset(&st, 0, sizeof(st));
    kvm_ctrl_result_t res = b->get_status(b->ctx, &st);
    b->unlock(b->ctx);

    if (res == KVM_CTRL_ERR_NOT_READY) {
        set_not_ready(r);
        return;
    }
    size_t len = 0;
    if (res != KVM_CTRL_OK || kvm_api_format_status(&st, r->body, sizeof(r->body), &len) != 0) {
        set_internal_error(r);
        r->log = true;
        return;
    }
    r->status = 200;
    r->body_len = len;
    r->log = false; /* 成功はポーリングで溢れるので記録しない */
    r->log_reason = "ok";
}

/* ------------------------------------------------------------------------------------------------
 * POST /api/v1/kvm/select、/api/v1/power/press, reset, force
 * ---------------------------------------------------------------------------------------------- */

/* power のエンドポイント -> 動作（呼び出し側は STATUS / SELECT を渡さない） */
static kvm_ctrl_power_action_t ep_action(kvm_api_endpoint_t ep) {
    if (ep == KVM_API_EP_POWER_PRESS) return KVM_CTRL_POWER_PRESS;
    if (ep == KVM_API_EP_POWER_RESET) return KVM_CTRL_POWER_RESET;
    return KVM_CTRL_POWER_FORCE;
}

static const char *action_name(kvm_ctrl_power_action_t a) {
    switch (a) {
        case KVM_CTRL_POWER_PRESS: return "press";
        case KVM_CTRL_POWER_RESET: return "reset";
        case KVM_CTRL_POWER_FORCE: return "force";
        default:                   return NULL;
    }
}

/* 残り(ms)を切り上げた秒。最小 1（0 秒の Retry-After は「すぐ再送してよい」に見えてしまうため） */
static uint32_t retry_after_seconds(uint32_t remaining_ms) {
    uint64_t s = ((uint64_t)remaining_ms + 999u) / 1000u;
    if (s < 1) s = 1;
    return (uint32_t)s;
}

/* 受理(202) / すでに選択済み(200) の本文 */
static void set_accepted(kvm_api_response_t *r, kvm_api_endpoint_t ep, uint8_t channel, uint32_t op_id) {
    writer_t w;
    w_init(&w, r->body, sizeof(r->body));
    w_str(&w, "{\"result\":\"accepted\",\"channel\":");
    w_u32(&w, channel);
    if (ep != KVM_API_EP_SELECT) {
        w_str(&w, ",\"action\":\"");
        w_str(&w, action_name(ep_action(ep)));
        w_str(&w, "\"");
    }
    w_str(&w, ",\"op_id\":");
    w_u32(&w, op_id);
    w_str(&w, "}");
    r->status = 202;
    r->body_len = w.len;
    r->log_reason = "accepted";
}

static void set_already_selected(kvm_api_response_t *r, uint8_t channel) {
    writer_t w;
    w_init(&w, r->body, sizeof(r->body));
    w_str(&w, "{\"result\":\"already_selected\",\"channel\":");
    w_u32(&w, channel);
    w_str(&w, "}");
    r->status = 200;
    r->body_len = w.len;
    r->log_reason = "already_selected";
}

/* 409 channel_mismatch + active_channel（unknown は起こらない。万一 1..4 でなければ null） */
static void set_channel_mismatch(kvm_api_response_t *r, uint8_t active) {
    writer_t w;
    w_init(&w, r->body, sizeof(r->body));
    w_str(&w, "{\"error\":\"channel_mismatch\",\"active_channel\":");
    if (active >= 1 && active <= 4) {
        w_u32(&w, active);
    } else {
        w_str(&w, "null");
    }
    w_str(&w, "}");
    r->status = 409;
    r->body_len = w.len;
    r->log_reason = "channel_mismatch";
}

static void handle_op(const kvm_api_backend_t *b, kvm_api_endpoint_t ep, const char *body, size_t body_len,
                      kvm_api_response_t *r) {
    uint8_t channel = 0;
    if (kvm_api_parse_channel(body, body_len, &channel) != 0) {
        set_error(r, 400, "invalid_request");
        return;
    }
    r->log_channel = channel;

    if (!backend_usable(b) || !b->lock(b->ctx)) {
        set_not_ready(r);
        return;
    }

    uint32_t op_id = 0;
    kvm_ctrl_result_t res;
    if (ep == KVM_API_EP_SELECT) {
        res = b->select(b->ctx, channel, &op_id);
    } else {
        res = b->power(b->ctx, channel, ep_action(ep), &op_id);
    }

    /* 409 の active_channel と 429 の Retry-After は、同じロックの中で読む（拒否の理由と値を揃える） */
    uint8_t active = 0;
    uint32_t cooldown_ms = 0;
    if (res == KVM_CTRL_ERR_CHANNEL_MISMATCH || res == KVM_CTRL_ERR_COOLDOWN) {
        kvm_ctrl_status_t st;
        memset(&st, 0, sizeof(st));
        if (b->get_status(b->ctx, &st) == KVM_CTRL_OK) {
            active = st.active_channel;
            cooldown_ms = st.cooldown_ms;
        }
    }
    b->unlock(b->ctx);

    switch (res) {
        case KVM_CTRL_OK:
            set_accepted(r, ep, channel, op_id);
            break;
        case KVM_CTRL_ALREADY_SELECTED:
            if (ep == KVM_API_EP_SELECT) {
                set_already_selected(r, channel);
            } else {
                set_internal_error(r); /* power に「すでに選択済み」は無い */
            }
            break;
        case KVM_CTRL_ERR_INVALID_PARAM:
            set_error(r, 400, "invalid_request");
            break;
        case KVM_CTRL_ERR_BUSY:
            set_error(r, 409, "busy");
            break;
        case KVM_CTRL_ERR_UNKNOWN_STATE:
            set_error(r, 409, "unknown_state");
            break;
        case KVM_CTRL_ERR_CHANNEL_MISMATCH:
            set_channel_mismatch(r, active);
            break;
        case KVM_CTRL_ERR_ATX_LOCKED:
            set_error(r, 409, "atx_locked");
            break;
        case KVM_CTRL_ERR_COOLDOWN:
            set_error(r, 429, "cooldown");
            r->has_retry_after = true;
            r->retry_after_s = retry_after_seconds(cooldown_ms);
            break;
        case KVM_CTRL_ERR_FAULT:
            set_error(r, 503, "hw_fault");
            break;
        case KVM_CTRL_ERR_NOT_READY:
            set_not_ready(r);
            break;
        default:
            set_internal_error(r);
            break;
    }
}

/* ------------------------------------------------------------------------------------------------
 * エントリ
 * ---------------------------------------------------------------------------------------------- */

void kvm_api_handle(const kvm_api_backend_t *backend, kvm_api_endpoint_t endpoint,
                    const char *body, size_t body_len, kvm_api_response_t *response) {
    if (!response) return;
    memset(response, 0, sizeof(*response));

    response->log_kind = endpoint_kind(endpoint);
    if (!response->log_kind) {
        response->log_kind = "unknown";
        set_internal_error(response);
        response->log = true;
        return;
    }

    if (endpoint == KVM_API_EP_STATUS) {
        handle_status(backend, response);
        return;
    }

    response->log = true;
    handle_op(backend, endpoint, body, body_len, response);
}

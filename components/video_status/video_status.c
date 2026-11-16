#include "video_status.h"
#include <stdio.h>
#include <string.h>

/* ctx と fn を 1 つの構造体にまとめる。書き込みは、読み手の起動前の 1 回の代入だけ（video_status.h） */
typedef struct {
    video_status_get_fn fn;
    void *ctx;
} provider_t;

static provider_t s_provider; /* 既定は fn == NULL（提供元なし） */

/* 提供元の値を、KVM に渡してよい形に直す */
static void normalize(video_status_t *s) {
    if (s->state == KVM_VIDEO_SYNC_LOCKED) {
        if (s->width == 0 || s->height == 0) {
            s->state = KVM_VIDEO_SYNC_UNKNOWN; /* 不整合: 解像度がない LOCKED は信用しない */
        }
    } else if (s->state != KVM_VIDEO_SYNC_UNKNOWN && s->state != KVM_VIDEO_SYNC_NO_SIGNAL) {
        s->state = KVM_VIDEO_SYNC_UNKNOWN; /* 範囲外 */
    }
    if (s->state != KVM_VIDEO_SYNC_LOCKED) {
        s->width = 0;
        s->height = 0;
    }
}

void video_status_set_provider(video_status_get_fn fn, void *ctx) {
    provider_t p;
    p.fn = fn;
    p.ctx = fn ? ctx : NULL;
    s_provider = p;
}

void video_status_get(video_status_t *out) {
    if (!out) return;
    provider_t p = s_provider; /* 1 回だけ読む */
    out->state = KVM_VIDEO_SYNC_UNKNOWN;
    out->width = 0;
    out->height = 0;
    if (!p.fn) return;
    p.fn(p.ctx, out);
    normalize(out);
}

kvm_video_sync_state_t video_status_hal_sync(void *ctx) {
    (void)ctx;
    video_status_t st;
    video_status_get(&st);
    return st.state;
}

/* ---- 状態の通知用の JSON ---- */

int video_status_format_json(const video_status_t *st, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    if (!st) return -1;

    video_status_t s = *st;
    normalize(&s); /* video_status_get と同じ補正。範囲外・不整合な値を、そのまま外に出さない */

    const char *name = "unknown";
    if (s.state == KVM_VIDEO_SYNC_NO_SIGNAL) {
        name = "no_signal";
    } else if (s.state == KVM_VIDEO_SYNC_LOCKED) {
        name = "locked";
    }

    /* 一時バッファに作ってから写す。足りないときに、out に部分的な文字列を残さない */
    char tmp[VIDEO_STATUS_JSON_MAX];
    int n = snprintf(tmp, sizeof(tmp), "{\"t\":\"video\",\"state\":\"%s\",\"w\":%lu,\"h\":%lu}", name,
                     (unsigned long)s.width, (unsigned long)s.height);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1; /* 起こらない（最長 60 バイト）。防御 */
    if ((size_t)n + 1 > cap) return -1;
    memcpy(out, tmp, (size_t)n + 1);
    return n;
}

/* ---- 偽の提供元 ---- */

void video_status_fake_init(video_status_fake_t *f) {
    if (!f) return;
    f->status.state = KVM_VIDEO_SYNC_UNKNOWN;
    f->status.width = 0;
    f->status.height = 0;
}

void video_status_fake_set(video_status_fake_t *f, kvm_video_sync_state_t state, uint32_t width, uint32_t height) {
    if (!f) return;
    f->status.state = state;
    f->status.width = width;
    f->status.height = height;
}

void video_status_fake_provider(void *ctx, video_status_t *out) {
    if (!out) return;
    const video_status_fake_t *f = (const video_status_fake_t *)ctx;
    if (!f) {
        out->state = KVM_VIDEO_SYNC_UNKNOWN;
        out->width = 0;
        out->height = 0;
        return;
    }
    *out = f->status;
}

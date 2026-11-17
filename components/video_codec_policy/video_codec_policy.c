#include "video_codec_policy.h"

#include <string.h>

video_codec_policy_config_t video_codec_policy_config_default(void) {
    video_codec_policy_config_t c;
    c.h264_allowed = false;
    c.upgrade_dwell_ms = VIDEO_CODEC_POLICY_DEFAULT_DWELL_MS;
    c.max_width = VIDEO_CODEC_POLICY_DEFAULT_MAX_WIDTH;
    c.max_height = VIDEO_CODEC_POLICY_DEFAULT_MAX_HEIGHT;
    return c;
}

void video_codec_policy_init(video_codec_policy_t *policy, const video_codec_policy_config_t *config) {
    if (!policy) return;
    memset(policy, 0, sizeof(*policy));
    policy->config = config ? *config : video_codec_policy_config_default();
    /* codec は memset で MJPEG（= 0）、待ちなし、統計 0 */
}

void video_codec_policy_set_h264_allowed(video_codec_policy_t *policy, bool allowed) {
    if (!policy) return;
    policy->config.h264_allowed = allowed;
}

/* 許可・能力・入力（接続数に関係なく、外れたら即 MJPEG になる条件）。 */
static bool source_conditions_ok(const video_codec_policy_t *p, const video_codec_policy_input_t *in) {
    if (!p->config.h264_allowed) return false;
    if ((in->source_supported_codecs & VIDEO_CODEC_POLICY_CAP_H264) == 0) return false;
    if (!in->locked) return false;
    if (in->width == 0 || in->height == 0) return false;
    if (in->width > p->config.max_width || in->height > p->config.max_height) return false;
    return true;
}

video_codec_policy_codec_t video_codec_policy_update(video_codec_policy_t *policy, const video_codec_policy_input_t *input) {
    if (!policy) return VIDEO_CODEC_POLICY_MJPEG;

    /* 入力がない: すべての条件が外れた扱い（安全側）。時計は、入力が渡されたときだけ進める。
     * h264_client_count > client_count（不整合）は、下の「h264_client_count == client_count」の比較が偽になるので、
     * H.264 なら即 MJPEG、MJPEG なら昇格しない（接続が 0 個で h264_client_count > 0 の場合も、`!=` で降格する）。 */
    bool src_ok = input && source_conditions_ok(policy, input);

    if (input) {
        if (input->now_ms < policy->last_now_ms) {
            policy->dwell_start_ms = input->now_ms; /* 時計の逆行: 待ちの最中なら、いまからやり直す（最中でなければ、使われない） */
        }
        policy->last_now_ms = input->now_ms;
    }

    if (policy->codec == VIDEO_CODEC_POLICY_H264) {
        policy->dwell_active = false; /* 待ちは MJPEG のときだけ意味がある */
        if (!src_ok) {
            policy->codec = VIDEO_CODEC_POLICY_MJPEG; /* 許可・能力・入力・不整合: 接続数に関係なく即時 */
            policy->stats.downgrades++;
        } else if (input->h264_client_count != input->client_count) {
            /* 申告していないクライアントがいる、または不整合: 即時。整合した入力（h264 <= client）では、
             * client_count == 0 のとき h264 == client == 0 なので、ここには来ない（接続が 0 個なら変えない） */
            policy->codec = VIDEO_CODEC_POLICY_MJPEG;
            policy->stats.downgrades++;
        }
        /* client_count == 0 なら、H.264 のまま */
        return policy->codec;
    }

    /* MJPEG: 5 つの条件が連続して満たされたときだけ、待ちを数える。 */
    bool declared = src_ok && input->client_count >= 1 && input->h264_client_count == input->client_count;
    if (!declared) {
        policy->dwell_active = false; /* 接続が 0 個のときも、ここ（待ちは捨てる） */
        return policy->codec;
    }
    if (!policy->dwell_active) {
        policy->dwell_active = true;
        policy->dwell_start_ms = input->now_ms;
    }
    if (input->now_ms - policy->dwell_start_ms >= policy->config.upgrade_dwell_ms) {
        policy->codec = VIDEO_CODEC_POLICY_H264;
        policy->stats.upgrades++;
    }
    return policy->codec;
}

video_codec_policy_codec_t video_codec_policy_get_codec(const video_codec_policy_t *policy) {
    return policy ? policy->codec : VIDEO_CODEC_POLICY_MJPEG;
}

void video_codec_policy_get_stats(const video_codec_policy_t *policy, video_codec_policy_stats_t *out) {
    if (!out) return;
    if (!policy) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = policy->stats;
}

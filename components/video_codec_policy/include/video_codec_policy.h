#ifndef VIDEO_CODEC_POLICY_H
#define VIDEO_CODEC_POLICY_H

/*
 * video_codec_policy: 全クライアントに配る映像のコーデック（MJPEG / H.264）を決める、状態を持つ純 C。IDF 非依存。
 * 契約は docs/VIDEO_TRANSPORT.md 9.5（方針）と 9.4（申告）、docs/INTERFACES.md 1.4。
 *
 * 呼び出し側（web_server の結線。このコンポーネントの範囲外）は、一定の周期（tick）で
 * video_codec_policy_update() に「いまの入力」を渡し、返ったコーデックが前回と違えば
 * video_source_t.set_codec() に伝える。このコンポーネントは時計も IDF も知らない（now_ms は呼び出し側が渡す）。
 *
 * 規則（9.5。H.264 になるには 5 つの条件のすべてが要る）
 *   許可: config.h264_allowed（Kconfig VIDEO_CODEC_H264_ALLOWED。既定 n）。false なら、どんな入力でも MJPEG。
 *   能力: source_supported_codecs に H.264 のビットがある。
 *   申告: client_count >= 1 かつ h264_client_count == client_count（全員が accept=h264）。
 *   入力: locked かつ 0 < width <= max_width かつ 0 < height <= max_height。
 *   安定: MJPEG から H.264 に上げるのは、上の 4 つが upgrade_dwell_ms 連続して満たされたとき（経過 >= upgrade_dwell_ms）。
 *         途中で 1 tick でも外れたら、待ちを最初からやり直す。待ちは、条件が最初に満たされた tick の now_ms から数える。
 *
 * 降格と、接続が 0 個のとき（9.5 の「接続が 0 個になったら、コーデックは変えない」の解釈。判断を固定する）
 *   - H.264 -> MJPEG は即時。許可・能力・入力のどれかが外れたら、接続数に関係なく、その tick で MJPEG。
 *     接続が 1 つ以上あって、全員が申告していないときも、その tick で MJPEG。
 *   - client_count == 0 のときは、コーデックを変えない（H.264 のままなら H.264、MJPEG のままなら MJPEG）。
 *     ただし許可・能力・入力が外れたときは上のとおり MJPEG に戻す（許可が外れたのに H.264 のまま、は、おかしい）。
 *   - client_count == 0 の間は「申告」が満たされないので、MJPEG からの待ちは進まない。**待ちは捨てる**
 *     （次の接続で、upgrade_dwell_ms を最初から数える。切断をまたいで待ちを持ち越さない）。
 *
 * 入力の不整合と NULL（安全側 = MJPEG に倒す）
 *   - h264_client_count > client_count は、不整合。「全員が申告」が偽になる（H.264 なら即 MJPEG、MJPEG なら待ちは捨てる）。
 *     client_count == 0 で h264_client_count > 0 のときも、H.264 のままにせず MJPEG に下げる。
 *   - policy == NULL: 何もしない（update は MJPEG を返す）。input == NULL: 不整合な入力と同じ（MJPEG、待ちは捨てる）。
 *     config == NULL（init）: 既定値（h264_allowed = false）。
 *   - now_ms が戻った（直前の tick より小さい）: 時計を信用できないので、待ちの最中なら、待ちを、その now_ms から
 *     やり直す（昇格が遅れる側に倒す。不正な昇格をしない）。降格には影響しない。now_ms は 64 ビットで、引き算しかしない。
 *
 * スレッド: 状態の更新は 1 つのタスク（tick を回すタスク）からだけ行う。ロックしない。
 */

#include <stdbool.h>
#include <stdint.h>

/* 値は docs/INTERFACES.md 1.4 の video_codec_t と同じ（MJPEG = 0、H.264 = 1）。
 * video_codec_t が frame_hub.h に入ったら、結線側で揃える（このコンポーネントは frame_hub に依存しない）。 */
typedef enum {
    VIDEO_CODEC_POLICY_MJPEG = 0,
    VIDEO_CODEC_POLICY_H264 = 1,
} video_codec_policy_codec_t;

/* video_source_t.supported_codecs のビット（1u << VIDEO_CODEC_*）。MJPEG のビットは見ない（常に作れる前提）。 */
#define VIDEO_CODEC_POLICY_CAP_H264 (1u << VIDEO_CODEC_POLICY_H264)

#define VIDEO_CODEC_POLICY_DEFAULT_DWELL_MS 5000u /* TODO(hw-verify): 実機で、接続の出入りの揺れと、IDR の大きさから決める */
#define VIDEO_CODEC_POLICY_DEFAULT_MAX_WIDTH 1920u
#define VIDEO_CODEC_POLICY_DEFAULT_MAX_HEIGHT 1080u

typedef struct {
    bool h264_allowed;          /* VIDEO_CODEC_H264_ALLOWED */
    uint32_t upgrade_dwell_ms;  /* VIDEO_CODEC_UPGRADE_DWELL_MS。0 なら、条件が満たされた tick で、すぐ昇格 */
    uint32_t max_width;         /* H.264 にする解像度の上限（含む）。0 なら、どの解像度も不可 */
    uint32_t max_height;
} video_codec_policy_config_t;

typedef struct {
    uint64_t now_ms;                  /* 呼び出し側の単調な時計（ミリ秒） */
    uint32_t source_supported_codecs; /* video_source_t.supported_codecs */
    uint32_t client_count;            /* 接続中の映像クライアントの数 */
    uint32_t h264_client_count;       /* そのうち accept=h264 を申告したクライアントの数 */
    bool locked;                      /* 映像の状態が LOCKED */
    uint32_t width;                   /* locked のときの解像度（locked でないときは見ない） */
    uint32_t height;
} video_codec_policy_input_t;

typedef struct {
    uint32_t upgrades;   /* MJPEG -> H.264 の回数 */
    uint32_t downgrades; /* H.264 -> MJPEG の回数 */
} video_codec_policy_stats_t;

typedef struct {
    video_codec_policy_config_t config;
    video_codec_policy_codec_t codec;
    bool dwell_active;       /* 昇格の条件が連続して満たされている最中（MJPEG のときだけ真になりうる） */
    uint64_t dwell_start_ms; /* 連続の始まりの now_ms（dwell_active のときだけ有効） */
    uint64_t last_now_ms;    /* 直前の tick の now_ms（最初は 0） */
    video_codec_policy_stats_t stats;
} video_codec_policy_t;

/* 既定: h264_allowed = false、待ち 5000 ms、上限 1920x1080。 */
video_codec_policy_config_t video_codec_policy_config_default(void);

/* 状態を MJPEG・待ちなし・統計 0 にして、config を保持する。config == NULL は既定値。policy == NULL は何もしない。 */
void video_codec_policy_init(video_codec_policy_t *policy, const video_codec_policy_config_t *config);

/* 許可の切替（実行中に外れたとき）。false にしても、コーデックは次の update で MJPEG になる。 */
void video_codec_policy_set_h264_allowed(video_codec_policy_t *policy, bool allowed);

/* 1 tick 進めて、配るコーデックを返す。規則は上の説明。 */
video_codec_policy_codec_t video_codec_policy_update(video_codec_policy_t *policy, const video_codec_policy_input_t *input);

/* 直近の update の結果（update していなければ MJPEG）。policy == NULL は MJPEG。 */
video_codec_policy_codec_t video_codec_policy_get_codec(const video_codec_policy_t *policy);

/* 切替の回数。policy == NULL なら 0。out == NULL なら何もしない。 */
void video_codec_policy_get_stats(const video_codec_policy_t *policy, video_codec_policy_stats_t *out);

#endif /* VIDEO_CODEC_POLICY_H */

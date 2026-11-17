/*
 * ws_video_core: H.264 のキーフレーム待ち (P7。docs/VIDEO_TRANSPORT.md 9.6、docs/INTERFACES.md 1.4) のテスト。
 *  - 接続直後 (H.264): P を配らず、IDR から配る。IDR の直後の P (seq が連続) は配る
 *  - seq の飛び: need_key になり、次の IDR まで P を配らない。IDR 自身は、飛んでいても配る
 *  - MJPEG は、need_key に関係なく配り、配ったら need_key が偽になる。MJPEG だけのときは、待ちも要求も起きない
 *  - MJPEG -> H.264 の切替で need_key (IDR で直る)。H.264 -> MJPEG -> H.264 でも、その都度
 *  - request_keyframe: need_key を真にしたとき、tick の最後に呼ぶ。複数のクライアントの要求は 1 つにまとまり、
 *    idr_min_interval_ms (境界 -1 / ちょうど / +1) が経った最初の tick で 1 回だけ呼ぶ。誰も待たなくなれば、溜めた要求は捨てる。
 *    NULL なら、要求は呼ばない (待ち・切断は動く)
 *  - key_wait_timeout_ms: H.264 のフレームを配らずに待ち始めた時刻から数える (境界 -1 / ちょうど)。0 = 閉じない。
 *    フレームが 1 つも来ない間 (信号なしなど) は、数えない (従来どおり、閉じない)
 *  - 映像の状態のテキスト (C-6b) との併用、複数のクライアントの独立、同じスロットでの再接続、時刻の巻き戻り
 *  - 不変条件ファズ (固定シード。独立した「配ってよい」の不変条件と、モデルの両方を検査する)
 */
#include "unity.h"
#include "ws_video_core.h"
#include "video_status.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXFD 16
#define MAXID 16
#define MAXDEL 8192

enum { K_NONE = 0, K_FRAME = 1, K_TEXT = 2 };

static ws_video_core_t core;

/* ---- ハブのモック (本物と同じく、クライアントごとに「最後に見た seq」を持ち、新しいフレームだけを返す) ---- */
typedef struct {
    bool used;
    int held;
    int64_t last_seen;
} hubc_t;
static hubc_t hubc[MAXID];
static video_frame_t hub_frames[MAXID];
static uint8_t hub_data[MAXID][16]; // [0] = codec, [1] = flags, [2..9] = seq (io_begin_send が配った中身を読み取る)
static bool lat_on;
static int64_t lat_seq = -1;
static uint8_t lat_codec, lat_flags;
static int unreg_count, rel_count, double_hold, bad_release, bad_unregister;

/* ---- 送信のモック ---- */
static int kind[MAXFD];
static bool dead[MAXFD];
static int close_cnt[MAXFD];
static int frames_tick[MAXFD];
static int begin_calls;             // io_begin_send の呼び出し回数
static int begin_ret;
static int text_calls;
static int viol_double, viol_dead;

typedef struct {
    int fd;
    int64_t seq;
    uint8_t codec, flags;
} del_t;
static del_t dels[MAXDEL];
static int del_n;

/* ---- request_keyframe のモック ---- */
static int req_count;
static int req_tick;
static uint64_t req_times[MAXDEL];
static uint64_t g_now;

static video_status_t g_status;

static int m_register(void *u) {
    (void)u;
    for (int i = 0; i < MAXID; i++) {
        if (!hubc[i].used) {
            hubc[i].used = true;
            hubc[i].held = 0;
            hubc[i].last_seen = -1;
            return i;
        }
    }
    return -1;
}
static void m_unregister(void *u, int id) {
    (void)u;
    if (id < 0 || id >= MAXID || !hubc[id].used) { bad_unregister++; return; }
    hubc[id].used = false;
    hubc[id].held = 0;
    unreg_count++;
}
static const video_frame_t *m_get_latest(void *u, int id) {
    (void)u;
    if (!lat_on || lat_seq <= hubc[id].last_seen) return NULL;
    if (hubc[id].held > 0) { double_hold++; return NULL; }
    hubc[id].held++;
    hubc[id].last_seen = lat_seq;
    hub_data[id][0] = lat_codec;
    hub_data[id][1] = lat_flags;
    memcpy(&hub_data[id][2], &lat_seq, sizeof(lat_seq));
    hub_frames[id].data = hub_data[id];
    hub_frames[id].size = 10;
    hub_frames[id].width = 1920;
    hub_frames[id].height = 1080;
    hub_frames[id].seq = lat_seq;
    hub_frames[id].codec = lat_codec;
    hub_frames[id].flags = lat_flags;
    return &hub_frames[id];
}
static void m_release(void *u, int id, const video_frame_t *f) {
    (void)u; (void)f;
    if (hubc[id].held <= 0) { bad_release++; return; }
    hubc[id].held--;
    rel_count++;
}
static int m_begin_send(void *u, ws_video_conn_t c, const uint8_t *d, size_t l) {
    (void)u; (void)l;
    uintptr_t fd = (uintptr_t)c;
    begin_calls++;
    if (dead[fd]) viol_dead++;
    if (kind[fd] != K_NONE) viol_double++;
    frames_tick[fd]++;
    if (begin_ret != 0) return begin_ret;
    kind[fd] = K_FRAME;
    if (del_n < MAXDEL) {
        dels[del_n].fd = (int)fd;
        dels[del_n].codec = d[0];
        dels[del_n].flags = d[1];
        memcpy(&dels[del_n].seq, &d[2], sizeof(int64_t));
        del_n++;
    }
    return 0;
}
static int m_begin_send_text(void *u, ws_video_conn_t c, const uint8_t *d, size_t l) {
    (void)u; (void)d; (void)l;
    uintptr_t fd = (uintptr_t)c;
    text_calls++;
    if (dead[fd]) viol_dead++;
    if (kind[fd] != K_NONE) viol_double++;
    kind[fd] = K_TEXT;
    return 0;
}
static void m_close(void *u, ws_video_conn_t c) {
    (void)u;
    uintptr_t fd = (uintptr_t)c;
    if (fd < MAXFD) { close_cnt[fd]++; dead[fd] = true; }
}
static bool m_validate(void *u, const uint8_t t[32]) { (void)u; (void)t; return true; }
static void m_status_get(void *u, video_status_t *out) { (void)u; *out = g_status; }
static int req_user;
static void m_request_keyframe(void *u) {
    TEST_ASSERT_EQUAL_PTR(&req_user, u);
    if (req_count < MAXDEL) req_times[req_count] = g_now;
    req_count++;
    req_tick++;
}

static void reset_all(void) {
    memset(hubc, 0, sizeof(hubc));
    memset(hub_frames, 0, sizeof(hub_frames));
    memset(hub_data, 0, sizeof(hub_data));
    memset(kind, 0, sizeof(kind));
    memset(dead, 0, sizeof(dead));
    memset(close_cnt, 0, sizeof(close_cnt));
    memset(frames_tick, 0, sizeof(frames_tick));
    lat_on = false;
    lat_seq = -1;
    lat_codec = VIDEO_CODEC_MJPEG;
    lat_flags = 0;
    unreg_count = rel_count = double_hold = bad_release = bad_unregister = 0;
    begin_calls = text_calls = 0;
    begin_ret = 0;
    viol_double = viol_dead = 0;
    del_n = 0;
    req_count = req_tick = 0;
    g_now = 0;
    g_status.state = KVM_VIDEO_SYNC_LOCKED;
    g_status.width = 1920;
    g_status.height = 1080;
}

/* 既定: 要求あり、idr_min_interval 1000、key_wait 5000、状態の通知なし */
static ws_video_core_config_t base_cfg(void) {
    ws_video_core_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_clients = 4;
    cfg.session_check_interval_ms = 1000000000ull;
    cfg.request_keyframe = m_request_keyframe;
    cfg.idr_min_interval_ms = 1000;
    cfg.key_wait_timeout_ms = 5000;
    cfg.hub_register = m_register;
    cfg.hub_unregister = m_unregister;
    cfg.hub_get_latest = m_get_latest;
    cfg.hub_release = m_release;
    cfg.io_begin_send = m_begin_send;
    cfg.io_request_close = m_close;
    cfg.io_validate_session = m_validate;
    cfg.user_data = &req_user;
    return cfg;
}

static void start_cfg(const ws_video_core_config_t *cfg) {
    ws_video_core_destroy(&core);
    reset_all();
    TEST_ASSERT_EQUAL(0, ws_video_core_init(&core, cfg));
}
static void start(void) {
    ws_video_core_config_t cfg = base_cfg();
    start_cfg(&cfg);
}

static int connect_fd(int fd, uint64_t now) {
    uint8_t tok[32] = {1};
    close_cnt[fd] = 0;
    dead[fd] = false;
    kind[fd] = K_NONE;
    int slot = ws_video_core_on_connect(&core, (ws_video_conn_t)(uintptr_t)fd, fd, tok, now);
    TEST_ASSERT_TRUE(slot >= 0);
    // これらのテストは、H.264 を受け取れるクライアントを扱う (接続したときは、申告なし = H.264 を配らない。申告なしの扱いは test_ws_video_core_accepts.c)。
    ws_video_core_set_accepts_h264(&core, fd, true);
    return slot;
}

static void done(int fd, bool ok) {
    ws_video_core_on_send_done(&core, fd, ok);
    kind[fd] = K_NONE;
}

static void tick(uint64_t now) {
    memset(frames_tick, 0, sizeof(frames_tick));
    req_tick = 0;
    g_now = now;
    ws_video_core_tick(&core, now);
}

/* フレームを出す (frame_hub の commit / commit_ex 相当)。seq は 1 ずつ増える */
static void publish(uint8_t codec, uint8_t flags) {
    lat_seq++;
    lat_on = true;
    lat_codec = codec;
    lat_flags = flags;
}
#define PUB_P() publish(VIDEO_CODEC_H264, 0)
#define PUB_IDR() publish(VIDEO_CODEC_H264, VIDEO_FRAME_FLAG_KEY)
#define PUB_JPEG() publish(VIDEO_CODEC_MJPEG, VIDEO_FRAME_FLAG_KEY)

/* tick して、fd に配られたか。配られたら、その送信を完了させる (次の tick で次のフレームを配れるように) */
static bool tick_delivers(int fd, uint64_t now) {
    tick(now);
    bool got = frames_tick[fd] > 0;
    if (got) done(fd, true);
    return got;
}

static int holds_total(void) {
    int n = 0;
    for (int i = 0; i < MAXID; i++) n += hubc[i].held;
    return n;
}

static void expect_no_violations(void) {
    TEST_ASSERT_EQUAL(0, viol_double);
    TEST_ASSERT_EQUAL(0, viol_dead);
    TEST_ASSERT_EQUAL(0, double_hold);
    TEST_ASSERT_EQUAL(0, bad_release);
    TEST_ASSERT_EQUAL(0, bad_unregister);
}

void setUp(void) { memset(&core, 0, sizeof(core)); reset_all(); }
void tearDown(void) { ws_video_core_destroy(&core); }

/* ================= 初期化 ================= */

void test_init_new_fields_are_optional(void) {
    ws_video_core_t c;
    ws_video_core_config_t cfg = base_cfg();
    cfg.request_keyframe = NULL;
    cfg.idr_min_interval_ms = 0;
    cfg.key_wait_timeout_ms = 0;
    TEST_ASSERT_EQUAL(0, ws_video_core_init(&c, &cfg));
    ws_video_core_destroy(&c);
    cfg.key_wait_timeout_ms = 100; // request_keyframe なしで、待ちの期限だけ
    TEST_ASSERT_EQUAL(0, ws_video_core_init(&c, &cfg));
    ws_video_core_destroy(&c);
}

/* ================= 接続直後 (H.264) ================= */

void test_connect_h264_p_is_not_delivered_until_idr(void) {
    start();
    connect_fd(10, 0);

    PUB_P(); // seq 0
    tick(0);
    TEST_ASSERT_EQUAL(0, begin_calls);
    TEST_ASSERT_EQUAL(1, rel_count); // 配らなかったフレームは release される
    TEST_ASSERT_EQUAL(0, holds_total());
    TEST_ASSERT_EQUAL(0, close_cnt[10]);

    PUB_P(); // seq 1
    PUB_P(); // seq 2 (seq 1 は見ない: ハブは最新へ飛ぶ)
    tick(10);
    TEST_ASSERT_EQUAL(0, begin_calls);
    TEST_ASSERT_EQUAL(2, rel_count);

    PUB_IDR(); // seq 3
    TEST_ASSERT_TRUE(tick_delivers(10, 20));
    TEST_ASSERT_EQUAL(1, del_n);
    TEST_ASSERT_EQUAL(3, (int)dels[0].seq);
    TEST_ASSERT_EQUAL(VIDEO_CODEC_H264, dels[0].codec);
    TEST_ASSERT_EQUAL(VIDEO_FRAME_FLAG_KEY, dels[0].flags);

    PUB_P(); // seq 4: IDR の直後 (連続)。配る
    TEST_ASSERT_TRUE(tick_delivers(10, 30));
    TEST_ASSERT_EQUAL(4, (int)dels[1].seq);
    PUB_P(); // seq 5
    TEST_ASSERT_TRUE(tick_delivers(10, 40));
    TEST_ASSERT_EQUAL(0, holds_total());
    expect_no_violations();
}

void test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request(void) {
    start();
    connect_fd(10, 0);
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    TEST_ASSERT_EQUAL_MESSAGE(0, req_count, "IDR が同じ tick で配れたので、溜めた要求は不要 (捨てる)");
    PUB_P();
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    PUB_P();
    TEST_ASSERT_TRUE(tick_delivers(10, 20));
    TEST_ASSERT_EQUAL(0, req_count);
}

void test_connect_without_frames_requests_once_at_end_of_first_tick(void) {
    start();
    connect_fd(10, 100);
    TEST_ASSERT_EQUAL(0, req_count); // on_connect では呼ばない (tick で呼ぶ)
    tick(100);
    TEST_ASSERT_EQUAL(1, req_count);
    tick(110);
    tick(120);
    TEST_ASSERT_EQUAL_MESSAGE(1, req_count, "待っている間は、同じ待ちで繰り返し要求しない");
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
}

/* ================= seq の飛び ================= */

void test_seq_gap_makes_need_key_until_next_idr(void) {
    start();
    connect_fd(10, 0);
    PUB_IDR(); // 0
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    PUB_P(); // 1
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    TEST_ASSERT_EQUAL(0, req_count);

    PUB_P(); // 2 (見ない)
    PUB_P(); // 3: 2 を飛ばした (遅いクライアント)
    TEST_ASSERT_FALSE(tick_delivers(10, 20));
    TEST_ASSERT_EQUAL(1, req_count); // need_key が真になった -> 要求
    PUB_P(); // 4: 連続だが、need_key の間
    TEST_ASSERT_FALSE(tick_delivers(10, 30));
    PUB_IDR(); // 5
    TEST_ASSERT_TRUE(tick_delivers(10, 40));
    PUB_P(); // 6
    TEST_ASSERT_TRUE(tick_delivers(10, 50));
    expect_no_violations();
}

void test_idr_with_seq_gap_is_delivered(void) {
    start();
    connect_fd(10, 0);
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    PUB_P();
    PUB_IDR(); // 飛んでいるが IDR
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    PUB_P(); // 直後は連続: 配る
    TEST_ASSERT_TRUE(tick_delivers(10, 20));
    TEST_ASSERT_EQUAL(0, req_count);
}

void test_backward_seq_is_a_discontinuity(void) {
    /* seq が戻る (ハブの作り直し、など): last + 1 でないので、飛びと同じ扱い (P は、IDR まで配らない) */
    start();
    connect_fd(10, 0);
    PUB_IDR(); // 0
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    PUB_P(); // 1
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    PUB_P(); // 2
    TEST_ASSERT_TRUE(tick_delivers(10, 20));
    lat_seq = 0;
    hubc[0].last_seen = -1; // ハブの側の状態も作り直された
    PUB_P();                // seq 1 (最後に配った 2 より小さい)
    TEST_ASSERT_FALSE(tick_delivers(10, 30));
    TEST_ASSERT_TRUE(core.need_key[0]);
    PUB_P();   // seq 2 (最後に配った seq と同じ値。従来どおり、重複として配らない)
    TEST_ASSERT_FALSE(tick_delivers(10, 35));
    PUB_IDR(); // seq 3
    TEST_ASSERT_TRUE(tick_delivers(10, 40));
}

void test_gap_boundary_seq_plus_one_is_continuous_plus_two_is_not(void) {
    start();
    connect_fd(10, 0);
    PUB_IDR(); // 0
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    PUB_P(); // 1: last + 1 (連続)
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    lat_seq++; // 2 は誰にも見せない (ハブが別のクライアントに配った、など)
    PUB_P();   // 3: last + 2
    TEST_ASSERT_FALSE(tick_delivers(10, 20));
}

void test_seq_gap_after_idr_dropped_by_publisher_is_visible_only_as_gap_in_delivery(void) {
    /* 公開の側の失敗 (捨てた IDR) は、seq が連続して見えるので、core では検出できない (契約 9.6)。
     * ここでは、配った seq の連続だけで判断することを確認する: 配っていない seq が間にあれば、飛びとみなす */
    start();
    connect_fd(10, 0);
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 0)); // 0
    PUB_P();
    PUB_P();
    TEST_ASSERT_FALSE(tick_delivers(10, 10)); // 1 を見ずに 2
}

/* ================= MJPEG は影響されない ================= */

void test_mjpeg_only_is_unchanged_and_never_waits_or_requests(void) {
    start();
    connect_fd(10, 0);
    for (int i = 0; i < 50; i++) {
        PUB_JPEG();
        if (i % 3 == 0) PUB_JPEG(); // 飛びを作る
        if (i % 7 == 0) publish(VIDEO_CODEC_MJPEG, 0); // KEY なしの MJPEG も配る
        TEST_ASSERT_TRUE(tick_delivers(10, (uint64_t)i * 10));
    }
    TEST_ASSERT_EQUAL(50, begin_calls);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    TEST_ASSERT_EQUAL_MESSAGE(0, req_count, "MJPEG だけなら、要求しない (フレームを配れた tick に、溜めた要求を捨てる)");
    TEST_ASSERT_EQUAL(50, del_n);
    TEST_ASSERT_FALSE(core.need_key[0]);
}

void test_mjpeg_delivered_regardless_of_need_key_and_clears_it(void) {
    start();
    connect_fd(10, 0);
    TEST_ASSERT_TRUE(core.need_key[0]);
    publish(VIDEO_CODEC_MJPEG, 0); // KEY なしでも、MJPEG は配る
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    TEST_ASSERT_FALSE(core.need_key[0]);
    TEST_ASSERT_EQUAL(VIDEO_CODEC_MJPEG, dels[0].codec);
}

void test_unknown_codec_value_is_not_gated(void) {
    start();
    connect_fd(10, 0);
    publish(7, 0); // 契約の外の値は、H.264 ではないので、配る (配らない側に倒さない)
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    TEST_ASSERT_FALSE(core.need_key[0]);
}

void test_h264_flag_bits_other_than_key_do_not_count_as_key(void) {
    start();
    connect_fd(10, 0);
    publish(VIDEO_CODEC_H264, 0xFEu); // KEY (bit 0) だけが立っていない
    TEST_ASSERT_FALSE(tick_delivers(10, 0));
    publish(VIDEO_CODEC_H264, 0x03u); // KEY が立つ (他のビットは無関係)
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
}

/* ================= コーデックの切替 ================= */

void test_mjpeg_to_h264_switch_sets_need_key(void) {
    start();
    connect_fd(10, 0);
    PUB_JPEG(); // 0
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    PUB_JPEG(); // 1
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    TEST_ASSERT_EQUAL(0, req_count);
    TEST_ASSERT_FALSE(core.need_key[0]);

    PUB_P(); // 2: seq は連続。コーデックが MJPEG -> H.264 に変わったので、P は配らない
    TEST_ASSERT_FALSE(tick_delivers(10, 20));
    TEST_ASSERT_TRUE(core.need_key[0]);
    TEST_ASSERT_EQUAL(1, req_count);
    PUB_IDR(); // 3
    TEST_ASSERT_TRUE(tick_delivers(10, 30));
    PUB_P(); // 4
    TEST_ASSERT_TRUE(tick_delivers(10, 40));
}

void test_mjpeg_to_h264_switch_with_idr_first_is_delivered(void) {
    start();
    connect_fd(10, 0);
    PUB_JPEG();
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    PUB_IDR(); // 切替の後の最初の H.264 は IDR (契約 9.5)
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    PUB_P();
    TEST_ASSERT_TRUE(tick_delivers(10, 20));
    TEST_ASSERT_EQUAL(0, req_count);
}

void test_h264_to_mjpeg_to_h264_sets_need_key_each_time(void) {
    start();
    connect_fd(10, 0);
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    PUB_JPEG(); // H.264 -> MJPEG: 配る
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    TEST_ASSERT_FALSE(core.need_key[0]);
    PUB_P(); // MJPEG -> H.264 (連続): 待つ
    TEST_ASSERT_FALSE(tick_delivers(10, 20));
    TEST_ASSERT_TRUE(core.need_key[0]);
    PUB_JPEG(); // また MJPEG: 配って、待ちが終わる
    TEST_ASSERT_TRUE(tick_delivers(10, 30));
    TEST_ASSERT_FALSE(core.need_key[0]);
    PUB_P(); // また H.264 (P)
    TEST_ASSERT_FALSE(tick_delivers(10, 40));
    expect_no_violations();
}

void test_mjpeg_frame_ends_the_wait_during_h264_stream(void) {
    start();
    connect_fd(10, 0);
    PUB_P();
    TEST_ASSERT_FALSE(tick_delivers(10, 0));
    TEST_ASSERT_TRUE(core.need_key[0]);
    PUB_JPEG(); // 待っている間に MJPEG が来た (方針が MJPEG に下がった)
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    TEST_ASSERT_FALSE(core.need_key[0]);
    PUB_JPEG();
    TEST_ASSERT_TRUE(tick_delivers(10, 20));
}

/* ================= request_keyframe のまとめ ================= */

void test_requests_of_several_clients_are_merged_into_one(void) {
    start();
    connect_fd(10, 0);
    connect_fd(11, 0);
    connect_fd(12, 0);
    PUB_P();
    tick(0);
    TEST_ASSERT_EQUAL_MESSAGE(1, req_count, "3 台の need_key の要求が 1 回にまとまる");
    TEST_ASSERT_EQUAL(0, begin_calls);
}

void test_request_boundary_waits_idr_min_interval_then_one_call(void) {
    start(); // idr_min_interval_ms = 1000
    connect_fd(10, 0);
    PUB_P();
    tick(0);
    TEST_ASSERT_EQUAL(1, req_count); // t = 0 に要求

    connect_fd(11, 100); // 2 台目が、間隔の途中で接続
    PUB_P();
    tick(100);
    TEST_ASSERT_EQUAL(1, req_count);
    PUB_P();
    tick(999); // 境界 -1
    TEST_ASSERT_EQUAL(1, req_count);
    PUB_P();
    tick(1000); // ちょうど
    TEST_ASSERT_EQUAL_MESSAGE(2, req_count, "間隔が経った最初の tick で、溜めた要求が 1 回出る");
    TEST_ASSERT_EQUAL(1000, (int)req_times[1]);
    PUB_P();
    tick(1001);
    tick(1500);
    TEST_ASSERT_EQUAL_MESSAGE(2, req_count, "溜めた要求は 1 回だけ。待ちが続いても、同じ要求を繰り返さない");
}

void test_request_boundary_one_ms_after_interval(void) {
    start();
    connect_fd(10, 0);
    tick(0);
    TEST_ASSERT_EQUAL(1, req_count);
    connect_fd(11, 5);
    tick(5);
    tick(1000); // ちょうど (最後の要求から 1000)
    TEST_ASSERT_EQUAL(2, req_count);
    connect_fd(12, 1001);
    tick(1001); // 最後の要求 (1000) から 1: まだ
    TEST_ASSERT_EQUAL(2, req_count);
    tick(1999); // 999
    TEST_ASSERT_EQUAL(2, req_count);
    tick(2000); // 1000
    TEST_ASSERT_EQUAL(3, req_count);
}

void test_pending_request_is_dropped_when_nobody_needs_a_key_any_more(void) {
    start();
    connect_fd(10, 0);
    tick(0);
    TEST_ASSERT_EQUAL(1, req_count);
    connect_fd(11, 100);
    PUB_IDR(); // 次の tick で、2 台目に IDR が届く (要求は溜まっている)
    TEST_ASSERT_TRUE(tick_delivers(10, 100)); // 10 も IDR を受ける
    TEST_ASSERT_EQUAL(1, frames_tick[11]);
    done(11, true);
    tick(1000);
    TEST_ASSERT_EQUAL_MESSAGE(1, req_count, "誰も待っていないので、溜めた要求は捨てる");
}

void test_pending_request_is_dropped_when_the_waiting_client_is_closed(void) {
    start();
    connect_fd(10, 0);
    tick(0);
    connect_fd(11, 10);
    tick(10); // 要求は溜まる (間隔の途中)
    TEST_ASSERT_EQUAL(1, req_count);
    ws_video_core_on_close(&core, 10);
    ws_video_core_on_close(&core, 11);
    tick(1000);
    TEST_ASSERT_EQUAL(1, req_count);
}

void test_interval_zero_does_not_merge_across_ticks_but_merges_within_a_tick(void) {
    ws_video_core_config_t cfg = base_cfg();
    cfg.idr_min_interval_ms = 0;
    start_cfg(&cfg);
    connect_fd(10, 0);
    connect_fd(11, 0);
    tick(0);
    TEST_ASSERT_EQUAL_MESSAGE(1, req_count, "同じ tick の要求は、間隔が 0 でも 1 回");
    connect_fd(12, 1);
    tick(1);
    TEST_ASSERT_EQUAL_MESSAGE(2, req_count, "間隔 0: 次の tick で、すぐ呼ぶ");
    tick(2);
    TEST_ASSERT_EQUAL(2, req_count);
}

void test_request_keyframe_null_still_gates_and_never_requests(void) {
    ws_video_core_config_t cfg = base_cfg();
    cfg.request_keyframe = NULL;
    start_cfg(&cfg);
    connect_fd(10, 0);
    PUB_P();
    tick(0);
    TEST_ASSERT_EQUAL(0, begin_calls); // P は配らない
    TEST_ASSERT_TRUE(core.need_key[0]);
    TEST_ASSERT_EQUAL(0, req_count);
    TEST_ASSERT_FALSE(core.idr_request_pending);
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    PUB_P();
    PUB_P();
    TEST_ASSERT_FALSE(tick_delivers(10, 20)); // 飛び
    TEST_ASSERT_EQUAL(0, req_count);
    TEST_ASSERT_FALSE(core.idr_request_pending);
    // 待ちの期限は、要求がなくても動く
    tick(20 + 4999);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(20 + 5000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_request_callback_receives_user_data_and_is_not_called_inside_connect(void) {
    start();
    connect_fd(10, 0);
    TEST_ASSERT_EQUAL(0, req_count); // m_request_keyframe は user_data を TEST_ASSERT で検査する
    tick(0);
    TEST_ASSERT_EQUAL(1, req_count);
}

void test_need_key_set_again_after_idr_requests_again_after_interval(void) {
    start();
    connect_fd(10, 0);
    PUB_P();
    tick(0);
    TEST_ASSERT_EQUAL(1, req_count);
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 100));
    PUB_P();
    PUB_P();
    tick(500); // 飛び -> need_key。ただし最後の要求から 500 ms
    TEST_ASSERT_EQUAL(1, req_count);
    tick(1000); // 最後の要求から 1000 ms
    TEST_ASSERT_EQUAL(2, req_count);
}

/* ================= key_wait_timeout_ms ================= */

void test_key_wait_timeout_boundary(void) {
    start(); // key_wait_timeout_ms = 5000
    connect_fd(10, 0);
    PUB_P();
    tick(100); // 最初に配らなかった時刻 = 100 (待ちの起点)
    PUB_P();
    tick(5099); // 起点から 4999 ms
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(5100); // ちょうど 5000 ms
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    tick(5200);
    TEST_ASSERT_EQUAL_MESSAGE(1, close_cnt[10], "閉じる要求は 1 回だけ");
}

void test_key_wait_does_not_restart_on_each_withheld_frame(void) {
    start();
    connect_fd(10, 0);
    for (uint64_t t = 0; t < 5000; t += 500) {
        PUB_P();
        tick(t);
        TEST_ASSERT_EQUAL(0, close_cnt[10]);
    }
    PUB_P();
    tick(5000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_idr_before_timeout_prevents_close_and_new_wait_has_new_origin(void) {
    start();
    connect_fd(10, 0);
    PUB_P();
    tick(0);
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 4000));
    TEST_ASSERT_FALSE(core.need_key[0]);
    PUB_P();
    PUB_P(); // 飛び -> need_key。起点は、この P を配らなかった時刻
    TEST_ASSERT_FALSE(tick_delivers(10, 6000));
    tick(6000 + 4999);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(6000 + 5000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_key_wait_is_not_applied_after_the_wait_ended(void) {
    start();
    connect_fd(10, 0);
    PUB_P();
    tick(0); // 待ちの起点 = 0
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 100)); // 待ちは終わり
    TEST_ASSERT_FALSE(core.need_key[0]);
    tick(5000); // 起点から 5000 ms 以上: 待っていないので、閉じない
    tick(100000);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
}

void test_key_wait_zero_never_closes(void) {
    ws_video_core_config_t cfg = base_cfg();
    cfg.key_wait_timeout_ms = 0;
    start_cfg(&cfg);
    connect_fd(10, 0);
    PUB_P();
    tick(0);
    PUB_P();
    tick(1000000000ull);
    PUB_P();
    tick(UINT64_MAX / 2);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
}

void test_key_wait_does_not_count_while_no_frame_arrives(void) {
    start();
    connect_fd(10, 0);
    tick(0);
    tick(5000);
    tick(60000);
    TEST_ASSERT_EQUAL_MESSAGE(0, close_cnt[10], "フレームが来ない間 (信号なしなど) は、従来どおり、閉じない");
    PUB_P(); // 最初のフレーム: ここから数える
    tick(60000);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(60000 + 4999);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(60000 + 5000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_key_wait_closes_only_the_waiting_client(void) {
    start();
    connect_fd(10, 0);
    connect_fd(11, 0);
    connect_fd(12, 0);
    PUB_IDR(); // 0: 3 台とも受ける
    tick(0);
    TEST_ASSERT_EQUAL(1, frames_tick[10]);
    TEST_ASSERT_EQUAL(1, frames_tick[11]);
    TEST_ASSERT_EQUAL(1, frames_tick[12]);
    done(10, true);
    done(11, true);
    done(12, true);
    PUB_P(); // 1: 3 台とも受ける。10 と 11 は、送信を完了しない (送信中のまま)
    tick(10);
    TEST_ASSERT_EQUAL(3, frames_tick[10] + frames_tick[11] + frames_tick[12]);
    done(12, true);
    for (int k = 2; k <= 3; k++) { // 12 だけが見続ける
        PUB_P();
        tick(10 + (uint64_t)k * 10);
        TEST_ASSERT_EQUAL(0, frames_tick[10] + frames_tick[11]);
        TEST_ASSERT_EQUAL(1, frames_tick[12]);
        done(12, true);
    }
    done(10, true);
    done(11, true);
    PUB_P(); // 4: 10 と 11 は、最後に 1 を配った。4 は連続ではない -> need_key (起点 = 100)
    tick(100);
    TEST_ASSERT_EQUAL(0, frames_tick[10] + frames_tick[11]);
    TEST_ASSERT_EQUAL(1, frames_tick[12]);
    TEST_ASSERT_TRUE(core.need_key[0] && core.need_key[1]);
    TEST_ASSERT_FALSE(core.need_key[2]);
    done(12, true);
    for (uint64_t t = 1100; t < 5100; t += 1000) {
        PUB_P();
        tick(t);
        TEST_ASSERT_EQUAL(1, frames_tick[12]);
        done(12, true);
    }
    TEST_ASSERT_EQUAL(0, close_cnt[10] + close_cnt[11] + close_cnt[12]);
    PUB_P();
    tick(5099);
    done(12, true);
    TEST_ASSERT_EQUAL(0, close_cnt[10] + close_cnt[11] + close_cnt[12]);
    PUB_P();
    tick(5100);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_EQUAL(1, close_cnt[11]);
    TEST_ASSERT_EQUAL(0, close_cnt[12]);
    expect_no_violations();
}

void test_key_wait_closes_a_client_that_is_sending_text(void) {
    ws_video_core_config_t cfg = base_cfg();
    cfg.status_get = m_status_get;
    cfg.io_begin_send_text = m_begin_send_text;
    start_cfg(&cfg);
    connect_fd(10, 0);
    PUB_P();
    tick(0); // 最初の tick は、状態のテキストが先。フレームは次の tick 以降
    TEST_ASSERT_EQUAL(1, text_calls);
    TEST_ASSERT_EQUAL(K_TEXT, kind[10]);
    tick(10); // テキストの送信中: フレームは扱わない (待ちの起点もまだ)
    done(10, true);
    PUB_P();
    tick(20); // 待ちの起点 = 20
    PUB_P();
    tick(5019);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(5020);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_key_wait_closes_even_while_a_send_is_in_flight(void) {
    /* 状態のテキストを送信中 (完了しない) のクライアントも、待ちの期限で閉じる。
     * (待ちの起点は、フレームを配らなかった時刻。テキストの送信が終わった後の tick で、最初の P を配らない) */
    ws_video_core_config_t cfg = base_cfg();
    cfg.status_get = m_status_get;
    cfg.io_begin_send_text = m_begin_send_text;
    start_cfg(&cfg);
    connect_fd(10, 0);
    PUB_P();
    tick(0); // テキストを送り始める
    done(10, true);
    tick(10); // P を配らない: 起点 = 10
    TEST_ASSERT_TRUE(core.need_key[0]);
    g_status.width = 640; // 状態が変わる -> 次の tick で、テキストを送り始める (完了しない)
    tick(20);
    TEST_ASSERT_EQUAL(2, text_calls);
    TEST_ASSERT_EQUAL(K_TEXT, kind[10]);
    tick(10 + 4999);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(10 + 5000);
    TEST_ASSERT_EQUAL_MESSAGE(1, close_cnt[10], "テキストの送信中でも、待ちの期限は数える");
}

/* ================= 映像の状態のテキスト (C-6b) との併用 ================= */

void test_status_text_does_not_disturb_the_key_wait(void) {
    ws_video_core_config_t cfg = base_cfg();
    cfg.status_get = m_status_get;
    cfg.io_begin_send_text = m_begin_send_text;
    cfg.status_min_interval_ms = 0;
    start_cfg(&cfg);
    connect_fd(10, 0);
    PUB_P(); // 0
    tick(0);
    TEST_ASSERT_EQUAL(1, text_calls); // テキストが先
    TEST_ASSERT_EQUAL(0, begin_calls);
    TEST_ASSERT_TRUE(core.need_key[0]);
    done(10, true);

    tick(10); // P (seq 0) をここで取る: 待っているので配らない
    TEST_ASSERT_EQUAL(0, begin_calls);
    TEST_ASSERT_TRUE(core.need_key[0]);

    // 状態が変わる -> テキストを送る。need_key は変わらない
    g_status.width = 1280;
    g_status.height = 720;
    tick(20);
    TEST_ASSERT_EQUAL(2, text_calls);
    TEST_ASSERT_TRUE(core.need_key[0]);
    done(10, true);

    PUB_IDR(); // 1
    tick(30);
    TEST_ASSERT_EQUAL(1, begin_calls);
    TEST_ASSERT_EQUAL(1, (int)dels[0].seq);
    done(10, true);
    TEST_ASSERT_FALSE(core.need_key[0]);

    // IDR のあとの P (連続) は配る。テキストは、配った seq に影響しない
    g_status.width = 1920;
    g_status.height = 1080;
    PUB_P(); // 2
    tick(40);
    TEST_ASSERT_EQUAL(3, text_calls); // 状態の変化が先
    TEST_ASSERT_EQUAL(1, begin_calls);
    done(10, true);
    tick(50);
    TEST_ASSERT_EQUAL(2, begin_calls);
    TEST_ASSERT_EQUAL(2, (int)dels[1].seq);
    TEST_ASSERT_EQUAL(0, holds_total() - 1); // 送信中の 1 つだけ保持
    done(10, true);
    TEST_ASSERT_EQUAL(0, holds_total());
    expect_no_violations();
}

void test_text_send_done_does_not_release_a_frame_for_a_waiting_client(void) {
    ws_video_core_config_t cfg = base_cfg();
    cfg.status_get = m_status_get;
    cfg.io_begin_send_text = m_begin_send_text;
    start_cfg(&cfg);
    connect_fd(10, 0);
    PUB_P();
    tick(0);
    int rel0 = rel_count;
    done(10, true);
    TEST_ASSERT_EQUAL_MESSAGE(rel0, rel_count, "テキストの完了で、フレームの解放は起きない");
    expect_no_violations();
}

/* ================= 複数のクライアント / 再接続 ================= */

void test_clients_are_independent(void) {
    start();
    connect_fd(10, 0);
    PUB_IDR(); // 0
    TEST_ASSERT_TRUE(tick_delivers(10, 0)); // 10 だけ IDR を受ける
    connect_fd(11, 5);
    PUB_P(); // 1: 10 には連続 (配る)、11 は接続直後 (配らない)
    tick(10);
    TEST_ASSERT_EQUAL(1, frames_tick[10]);
    TEST_ASSERT_EQUAL(0, frames_tick[11]);
    done(10, true);
    PUB_IDR(); // 2
    tick(20);
    TEST_ASSERT_EQUAL(1, frames_tick[10]);
    TEST_ASSERT_EQUAL(1, frames_tick[11]); // 11 も IDR から
    done(10, true);
    done(11, true);
    PUB_P(); // 3
    tick(30);
    TEST_ASSERT_EQUAL(1, frames_tick[10]);
    TEST_ASSERT_EQUAL(1, frames_tick[11]);
    TEST_ASSERT_EQUAL(0, holds_total() - 2);
    expect_no_violations();
}

void test_reconnect_on_the_same_slot_resets_the_wait_state(void) {
    start();
    connect_fd(10, 0);
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 0)); // seq 0 を配った。need_key 偽、last_codec = H.264
    PUB_P();
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    ws_video_core_on_close(&core, 10);
    connect_fd(10, 20); // 同じスロット、同じ fd で再接続
    PUB_P(); // seq 2
    TEST_ASSERT_FALSE(tick_delivers(10, 20)); // 前の接続の状態を引き継がない: P は配らない
    TEST_ASSERT_TRUE(core.need_key[0]);
    // 待ちの起点も、前の接続のものではない
    tick(20 + 4999);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 20 + 4999));
    expect_no_violations();
}

void test_reconnect_after_timeout_close_has_a_new_wait(void) {
    start();
    connect_fd(10, 0);
    PUB_P();
    tick(0);
    PUB_P();
    tick(5000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    ws_video_core_on_close(&core, 10);
    connect_fd(10, 5100);
    PUB_P();
    tick(5100);
    PUB_P();
    tick(5100 + 4999);
    TEST_ASSERT_EQUAL_MESSAGE(0, close_cnt[10], "再接続の待ちは、新しい起点から数える");
    tick(5100 + 5000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_failed_begin_send_of_idr_closes_and_does_not_count_as_delivered(void) {
    start();
    connect_fd(10, 0);
    PUB_IDR();
    begin_ret = -1;
    tick(0);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_EQUAL(0, del_n);
    TEST_ASSERT_EQUAL(0, holds_total()); // 失敗したフレームは release される
    TEST_ASSERT_EQUAL_MESSAGE(0, req_count, "閉じる予定のクライアントのために、要求しない");
}

/* ================= 時刻の巻き戻り ================= */

void test_clock_rollback_clamps_last_request_time(void) {
    start();
    connect_fd(10, 0);
    tick(5000); // 要求 (last = 5000)
    TEST_ASSERT_EQUAL(1, req_count);
    connect_fd(11, 5000);
    tick(100); // 巻き戻り: last = 100 に合わせる。要求は溜まる
    TEST_ASSERT_EQUAL(1, req_count);
    tick(1099);
    TEST_ASSERT_EQUAL(1, req_count);
    tick(1100);
    TEST_ASSERT_EQUAL_MESSAGE(2, req_count, "巻き戻りで、間隔がいつまでも経たなくならない");
}

void test_clock_rollback_clamps_key_wait_origin(void) {
    start();
    connect_fd(10, 0);
    PUB_P();
    tick(9000); // 起点 = 9000
    PUB_P();
    tick(100); // 巻き戻り: 起点を 100 に合わせる (すぐには閉じない)
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    PUB_P();
    tick(100 + 4999);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(100 + 5000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_clock_rollback_to_zero_does_not_underflow(void) {
    start();
    connect_fd(10, 0);
    PUB_P();
    tick(UINT64_MAX - 10);
    PUB_P();
    tick(0);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(4999);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(5000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

/* ================= 既存の挙動との非干渉 ================= */

void test_close_requested_client_is_not_served_and_not_counted_as_waiting(void) {
    start();
    connect_fd(10, 0);
    tick(0);
    TEST_ASSERT_EQUAL(1, req_count);
    ws_video_core_on_client_data(&core, 10); // クライアントからのデータ -> 閉じる要求
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    connect_fd(11, 10);
    tick(10); // 要求は溜まる (11 のため)
    ws_video_core_on_client_data(&core, 11);
    tick(1000); // 待っているのは閉じる予定のクライアントだけ: 呼ばない
    TEST_ASSERT_EQUAL(1, req_count);
}

/* ================= ファズ ================= */

typedef struct {
    bool connected;   // on_connect 済みで、完全に消えていない
    bool closing;     // on_close 済み (送信中だったので、on_send_done を待つ)
    bool close_req;   // core が閉じる要求を出した (モデル)
    bool inflight;    // 送信中 (フレームまたはテキスト)
    bool text_sent;   // 状態のテキストを送った
    int64_t last_seq;
    uint8_t last_codec;
    bool need, waiting;
    uint64_t since;
    int64_t seen;     // ハブの last_seen
    /* 独立した不変条件用: 実際に配られた最後のフレーム */
    int64_t ind_seq;
    int ind_codec;
} fm_t;

static long fz_withheld, fz_delivered_key_after_wait, fz_requests, fz_throttled, fz_timeouts, fz_rollbacks,
    fz_switch_marks, fz_texts, fz_gap_marks;

static uint32_t fz_state;
static uint32_t rnd(void) {
    fz_state ^= fz_state << 13;
    fz_state ^= fz_state >> 17;
    fz_state ^= fz_state << 5;
    return fz_state;
}

static int fz_one(uint32_t seed, int steps) {
    fz_state = seed ? seed : 1u;
    ws_video_core_config_t cfg = base_cfg();
    static const uint64_t mins[] = {0, 1, 300, 1000};
    static const uint64_t waits[] = {0, 1, 500, 2000};
    cfg.idr_min_interval_ms = mins[rnd() % 4];
    cfg.key_wait_timeout_ms = waits[rnd() % 4];
    const bool has_req = (rnd() % 4) != 0;
    if (!has_req) cfg.request_keyframe = NULL;
    const bool has_status = (rnd() % 2) == 0;
    if (has_status) {
        cfg.status_get = m_status_get;
        cfg.io_begin_send_text = m_begin_send_text;
        cfg.status_min_interval_ms = 0;
    }
    const bool rollback = (rnd() % 3) == 0;
    ws_video_core_destroy(&core);
    reset_all();
    if (ws_video_core_init(&core, &cfg) != 0) return -1;

    fm_t fm[MAXFD];
    memset(fm, 0, sizeof(fm));
    bool pend = false, requested = false;
    uint64_t last_req = 0;
    uint64_t now = 100000;
    int mode = 0; // 0 = MJPEG, 1 = H.264 (配信側が選んだコーデック)
    uint64_t last_real_req = 0;
    bool had_real_req = false;

    for (int step = 0; step < steps; step++) {
        uint32_t r = rnd() % 100;
        if (r < 8) { // 接続
            int f = 10 + (int)(rnd() % 4);
            if (!fm[f].connected) {
                connect_fd(f, now);
                memset(&fm[f], 0, sizeof(fm[f]));
                fm[f].connected = true;
                fm[f].last_seq = -1;
                fm[f].last_codec = VIDEO_CODEC_MJPEG;
                fm[f].seen = -1;
                fm[f].ind_seq = -100;
                fm[f].ind_codec = -1;
                fm[f].need = true; // (a)
                if (has_req) pend = true;
            }
        } else if (r < 14) { // transport の切断 (on_close)
            int f = 10 + (int)(rnd() % 4);
            if (fm[f].connected && !fm[f].closing) {
                ws_video_core_on_close(&core, f);
                if (fm[f].inflight) fm[f].closing = true;
                else memset(&fm[f], 0, sizeof(fm[f]));
            }
        } else if (r < 18) { // クライアントからのデータ (閉じる要求)
            int f = 10 + (int)(rnd() % 4);
            if (fm[f].connected && !fm[f].closing) {
                ws_video_core_on_client_data(&core, f);
                fm[f].close_req = true;
            }
        } else if (r < 40) { // 送信の完了
            int f = 10 + (int)(rnd() % 4);
            if (fm[f].connected && fm[f].inflight) {
                bool ok = (rnd() % 12) != 0;
                done(f, ok);
                fm[f].inflight = false;
                if (fm[f].closing) memset(&fm[f], 0, sizeof(fm[f]));
                else if (!ok) fm[f].close_req = true;
            }
        } else if (r < 62) { // フレームを出す
            if ((rnd() % 8) == 0) mode = !mode;
            int n = 1 + (int)(rnd() % 3 == 0 ? rnd() % 3 : 0);
            for (int k = 0; k < n; k++) {
                if (mode == 0) {
                    publish(VIDEO_CODEC_MJPEG, (rnd() % 8) ? VIDEO_FRAME_FLAG_KEY : 0);
                } else {
                    publish(VIDEO_CODEC_H264, (rnd() % 7) == 0 ? VIDEO_FRAME_FLAG_KEY : 0);
                }
            }
            if ((rnd() % 40) == 0) lat_on = false; // 信号なし: フレームが来ない
        } else { // tick
            if (rollback && (rnd() % 10) == 0) {
                uint64_t back = rnd() % 3000;
                now = back < now ? now - back : 0;
                fz_rollbacks++;
            } else {
                now += rnd() % 700;
            }
            begin_ret = ((rnd() % 30) == 0) ? -1 : 0;
            memset(frames_tick, 0, sizeof(frames_tick));
            int text_before = text_calls;
            int rel_before = rel_count;
            int begin_before = begin_calls;
            int del_before = del_n;
            int close_before[MAXFD];
            memcpy(close_before, close_cnt, sizeof(close_cnt));
            int exp_close_new[MAXFD] = {0};
            int exp_deliver_seq_fd[MAXFD];
            memset(exp_deliver_seq_fd, 0, sizeof(exp_deliver_seq_fd));
            int exp_withheld = 0, exp_texts = 0;
            int64_t exp_seq[MAXFD];
            uint8_t exp_codec[MAXFD], exp_flags[MAXFD];
            memset(exp_seq, 0, sizeof(exp_seq));
            memset(exp_codec, 0, sizeof(exp_codec));
            memset(exp_flags, 0, sizeof(exp_flags));

            /* ---- モデル ---- */
            for (int f = 10; f < 14; f++) {
                fm_t *q = &fm[f];
                if (!q->connected || q->closing || q->close_req) continue;
                if (q->need && q->waiting && cfg.key_wait_timeout_ms > 0) {
                    if (now < q->since) q->since = now;
                    if (now - q->since >= cfg.key_wait_timeout_ms) {
                        q->close_req = true;
                        exp_close_new[f] = 1;
                        fz_timeouts++;
                        continue;
                    }
                }
                if (q->inflight) continue;
                if (has_status && !q->text_sent) {
                    q->text_sent = true;
                    q->inflight = true;
                    exp_texts++;
                    fz_texts++;
                    continue;
                }
                if (!lat_on || lat_seq <= q->seen) continue;
                q->seen = lat_seq;
                int64_t seq = lat_seq;
                bool h264 = (lat_codec == VIDEO_CODEC_H264);
                bool key = (lat_flags & VIDEO_FRAME_FLAG_KEY) != 0;
                if (seq != q->last_seq + 1) {
                    if (!q->need) {
                        q->need = true;
                        q->waiting = false;
                        if (has_req) pend = true;
                        fz_gap_marks++;
                    }
                }
                if (h264 && q->last_codec != VIDEO_CODEC_H264) {
                    if (!q->need) {
                        q->need = true;
                        q->waiting = false;
                        if (has_req) pend = true;
                        fz_switch_marks++;
                    }
                }
                if (q->need && h264 && !key) {
                    if (!q->waiting) {
                        q->waiting = true;
                        q->since = now;
                    }
                    exp_withheld++;
                    fz_withheld++;
                    continue;
                }
                if (begin_ret != 0) {
                    q->close_req = true;
                    exp_close_new[f] = 1;
                    continue;
                }
                q->last_seq = seq;
                q->last_codec = h264 ? VIDEO_CODEC_H264 : VIDEO_CODEC_MJPEG;
                if (!h264 || key) {
                    if (q->need && q->waiting) fz_delivered_key_after_wait++;
                    q->need = false;
                    q->waiting = false;
                }
                q->inflight = true;
                exp_deliver_seq_fd[f] = 1;
                exp_seq[f] = seq;
                exp_codec[f] = lat_codec;
                exp_flags[f] = lat_flags;
            }
            int exp_req = 0;
            if (pend) {
                bool any = false;
                for (int f = 10; f < 14; f++) {
                    if (fm[f].connected && !fm[f].closing && !fm[f].close_req && fm[f].need) any = true;
                }
                if (!any || !has_req) {
                    pend = false;
                } else {
                    bool fire = true;
                    if (requested) {
                        if (now < last_req) last_req = now;
                        if (cfg.idr_min_interval_ms > 0 && now - last_req < cfg.idr_min_interval_ms) {
                            fire = false;
                            fz_throttled++;
                        }
                    }
                    if (fire) {
                        pend = false;
                        requested = true;
                        last_req = now;
                        exp_req = 1;
                        fz_requests++;
                    }
                }
            }

            req_tick = 0;
            g_now = now;
            ws_video_core_tick(&core, now);
            /* ---- 比較 ---- */
            if (pend != core.idr_request_pending) {
                printf("seed=%u step=%d pend model=%d core=%d (tick now=%llu)\n", (unsigned)seed, step, pend,
                       core.idr_request_pending, (unsigned long long)now);
                return -20;
            }
            int got_delivered = 0;
            for (int f = 10; f < 14; f++) {
                int d = frames_tick[f];
                if (d != exp_deliver_seq_fd[f] && !(begin_ret != 0 && exp_close_new[f] && d == 1)) {
                    printf("seed=%u step=%d fd=%d frames=%d expect=%d\n", (unsigned)seed, step, f, d, exp_deliver_seq_fd[f]);
                    return -2;
                }
                if (close_cnt[f] - close_before[f] != exp_close_new[f]) {
                    printf("seed=%u step=%d fd=%d close=%d expect=%d\n", (unsigned)seed, step, f,
                           close_cnt[f] - close_before[f], exp_close_new[f]);
                    return -3;
                }
                if (exp_deliver_seq_fd[f]) got_delivered++;
            }
            if (del_n - del_before != got_delivered) {
                printf("seed=%u step=%d delivered=%d expect=%d\n", (unsigned)seed, step, del_n - del_before, got_delivered);
                return -4;
            }
            if (text_calls - text_before != exp_texts) {
                printf("seed=%u step=%d texts=%d expect=%d\n", (unsigned)seed, step, text_calls - text_before, exp_texts);
                return -5;
            }
            for (int k = del_before; k < del_n; k++) {
                int f = dels[k].fd;
                if (!exp_deliver_seq_fd[f] || dels[k].seq != exp_seq[f] || dels[k].codec != exp_codec[f] ||
                    dels[k].flags != exp_flags[f]) {
                    printf("seed=%u step=%d fd=%d delivered seq=%lld codec=%d flags=%d expect seq=%lld codec=%d flags=%d\n",
                           (unsigned)seed, step, f, (long long)dels[k].seq, dels[k].codec, dels[k].flags, (long long)exp_seq[f],
                           exp_codec[f], exp_flags[f]);
                    return -6;
                }
            }
            /* 配らなかったフレームは、すべて release されている (配ったもの + 失敗したもの以外) */
            int begun = begin_calls - begin_before;
            int rel = rel_count - rel_before;
            if (rel != exp_withheld + (begun - got_delivered)) {
                printf("seed=%u step=%d released=%d expect=%d (withheld=%d failed=%d)\n", (unsigned)seed, step, rel,
                       exp_withheld + (begun - got_delivered), exp_withheld, begun - got_delivered);
                return -7;
            }
            if (req_tick != exp_req) {
                printf("seed=%u step=%d requests=%d expect=%d now=%llu last=%llu min=%llu has_req=%d rollback=%d pend=%d requested=%d\n",
                       (unsigned)seed, step, req_tick, exp_req, (unsigned long long)now, (unsigned long long)last_req,
                       (unsigned long long)cfg.idr_min_interval_ms, has_req, rollback, pend, requested);
                return -8;
            }

            /* ---- 独立した不変条件 (モデルの規則を使わない) ---- */
            for (int k = del_before; k < del_n; k++) {
                int f = dels[k].fd;
                fm_t *q = &fm[f];
                bool h264 = dels[k].codec == VIDEO_CODEC_H264;
                bool key = (dels[k].flags & VIDEO_FRAME_FLAG_KEY) != 0;
                if (h264 && !key) {
                    /* H.264 の P を受け取るのは、直前に配った (seq が連続した) H.264 のフレームがあるときだけ */
                    if (q->ind_codec != VIDEO_CODEC_H264 || q->ind_seq != dels[k].seq - 1) {
                        printf("seed=%u step=%d fd=%d INVARIANT: P seq=%lld delivered without its predecessor (prev codec=%d seq=%lld)\n",
                               (unsigned)seed, step, f, (long long)dels[k].seq, q->ind_codec, (long long)q->ind_seq);
                        return -9;
                    }
                }
                q->ind_codec = h264 ? VIDEO_CODEC_H264 : VIDEO_CODEC_MJPEG;
                q->ind_seq = dels[k].seq;
            }
            if (req_tick > 1) { printf("seed=%u step=%d several requests in one tick\n", (unsigned)seed, step); return -10; }
            if (req_tick == 1) {
                if (!has_req) { printf("seed=%u request without a callback?\n", (unsigned)seed); return -11; }
                if (!rollback && cfg.idr_min_interval_ms > 0 && had_real_req &&
                    now - last_real_req < cfg.idr_min_interval_ms) {
                    printf("seed=%u step=%d request %llu ms after the last (min %llu)\n", (unsigned)seed, step,
                           (unsigned long long)(now - last_real_req), (unsigned long long)cfg.idr_min_interval_ms);
                    return -12;
                }
                had_real_req = true;
                last_real_req = now;
            }
            /* tick 後に、モデルの inflight を実際に合わせる (送信を始めた / テキストを始めたもの) */
            for (int f = 10; f < 14; f++) {
                if (fm[f].connected && (kind[f] != K_NONE) != fm[f].inflight) {
                    printf("seed=%u step=%d fd=%d inflight model=%d actual=%d\n", (unsigned)seed, step, f, fm[f].inflight,
                           kind[f] != K_NONE);
                    return -13;
                }
            }
        }
        if (pend != core.idr_request_pending) {
            printf("seed=%u step=%d r=%u pend model=%d core=%d after op\n", (unsigned)seed, step, (unsigned)r, pend,
                   core.idr_request_pending);
            return -21;
        }
        if (viol_double || viol_dead || double_hold || bad_release || bad_unregister) {
            printf("seed=%u step=%d violations dbl=%d dead=%d hold=%d rel=%d unreg=%d\n", (unsigned)seed, step, viol_double,
                   viol_dead, double_hold, bad_release, bad_unregister);
            return -14;
        }
    }

    /* 後始末: 保持が残らない */
    for (int f = 10; f < 14; f++) {
        if (fm[f].connected) {
            if (fm[f].inflight) done(f, true);
            if (!fm[f].closing) {
                ws_video_core_on_close(&core, f);
                int slot = ws_clients_find(&core.clients, f);
                if (slot >= 0 && core.clients.clients[slot].is_sending) done(f, true);
            }
        }
    }
    for (int i = 0; i < MAXID; i++) {
        if (hubc[i].held != 0) { printf("seed=%u leftover hold id=%d\n", (unsigned)seed, i); return -15; }
        if (hubc[i].used) { printf("seed=%u leftover registration id=%d\n", (unsigned)seed, i); return -16; }
    }
    return 0;
}

void test_fuzz_keyframe_gating_matches_model_and_invariants(void) {
    fz_withheld = fz_delivered_key_after_wait = fz_requests = fz_throttled = fz_timeouts = fz_rollbacks = 0;
    fz_switch_marks = fz_texts = fz_gap_marks = 0;
    for (uint32_t seed = 1; seed <= 400; seed++) {
        int r = fz_one(seed * 2654435761u + 5u, 3000);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, r, "model mismatch or invariant violation (see printed seed)");
    }
    printf("fuzz: withheld=%ld key-after-wait=%ld requests=%ld throttled=%ld timeouts=%ld gap-marks=%ld switch-marks=%ld texts=%ld rollbacks=%ld\n",
           fz_withheld, fz_delivered_key_after_wait, fz_requests, fz_throttled, fz_timeouts, fz_gap_marks, fz_switch_marks,
           fz_texts, fz_rollbacks);
    /* 空振りでないこと (固定シードなので、毎回同じ値) */
    TEST_ASSERT_TRUE_MESSAGE(fz_withheld > 20000, "fuzz withheld too few frames");
    TEST_ASSERT_TRUE_MESSAGE(fz_delivered_key_after_wait > 2000, "fuzz never ended a wait with a key frame");
    TEST_ASSERT_TRUE_MESSAGE(fz_requests > 2000, "fuzz made too few requests");
    TEST_ASSERT_TRUE_MESSAGE(fz_throttled > 500, "fuzz never throttled a request");
    TEST_ASSERT_TRUE_MESSAGE(fz_timeouts > 200, "fuzz never timed out a wait");
    TEST_ASSERT_TRUE_MESSAGE(fz_gap_marks > 2000, "fuzz never saw a seq gap");
    TEST_ASSERT_TRUE_MESSAGE(fz_switch_marks > 200, "fuzz never switched codec");
    TEST_ASSERT_TRUE_MESSAGE(fz_texts > 500, "fuzz never sent a status text");
    TEST_ASSERT_TRUE_MESSAGE(fz_rollbacks > 500, "fuzz never rolled the clock back");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_new_fields_are_optional);

    RUN_TEST(test_connect_h264_p_is_not_delivered_until_idr);
    RUN_TEST(test_connect_h264_first_frame_idr_is_delivered_at_once_and_no_request);
    RUN_TEST(test_connect_without_frames_requests_once_at_end_of_first_tick);

    RUN_TEST(test_seq_gap_makes_need_key_until_next_idr);
    RUN_TEST(test_idr_with_seq_gap_is_delivered);
    RUN_TEST(test_gap_boundary_seq_plus_one_is_continuous_plus_two_is_not);
    RUN_TEST(test_backward_seq_is_a_discontinuity);
    RUN_TEST(test_seq_gap_after_idr_dropped_by_publisher_is_visible_only_as_gap_in_delivery);

    RUN_TEST(test_mjpeg_only_is_unchanged_and_never_waits_or_requests);
    RUN_TEST(test_mjpeg_delivered_regardless_of_need_key_and_clears_it);
    RUN_TEST(test_unknown_codec_value_is_not_gated);
    RUN_TEST(test_h264_flag_bits_other_than_key_do_not_count_as_key);

    RUN_TEST(test_mjpeg_to_h264_switch_sets_need_key);
    RUN_TEST(test_mjpeg_to_h264_switch_with_idr_first_is_delivered);
    RUN_TEST(test_h264_to_mjpeg_to_h264_sets_need_key_each_time);
    RUN_TEST(test_mjpeg_frame_ends_the_wait_during_h264_stream);

    RUN_TEST(test_requests_of_several_clients_are_merged_into_one);
    RUN_TEST(test_request_boundary_waits_idr_min_interval_then_one_call);
    RUN_TEST(test_request_boundary_one_ms_after_interval);
    RUN_TEST(test_pending_request_is_dropped_when_nobody_needs_a_key_any_more);
    RUN_TEST(test_pending_request_is_dropped_when_the_waiting_client_is_closed);
    RUN_TEST(test_interval_zero_does_not_merge_across_ticks_but_merges_within_a_tick);
    RUN_TEST(test_request_keyframe_null_still_gates_and_never_requests);
    RUN_TEST(test_request_callback_receives_user_data_and_is_not_called_inside_connect);
    RUN_TEST(test_need_key_set_again_after_idr_requests_again_after_interval);

    RUN_TEST(test_key_wait_timeout_boundary);
    RUN_TEST(test_key_wait_does_not_restart_on_each_withheld_frame);
    RUN_TEST(test_idr_before_timeout_prevents_close_and_new_wait_has_new_origin);
    RUN_TEST(test_key_wait_is_not_applied_after_the_wait_ended);
    RUN_TEST(test_key_wait_zero_never_closes);
    RUN_TEST(test_key_wait_does_not_count_while_no_frame_arrives);
    RUN_TEST(test_key_wait_closes_only_the_waiting_client);
    RUN_TEST(test_key_wait_closes_a_client_that_is_sending_text);
    RUN_TEST(test_key_wait_closes_even_while_a_send_is_in_flight);

    RUN_TEST(test_status_text_does_not_disturb_the_key_wait);
    RUN_TEST(test_text_send_done_does_not_release_a_frame_for_a_waiting_client);

    RUN_TEST(test_clients_are_independent);
    RUN_TEST(test_reconnect_on_the_same_slot_resets_the_wait_state);
    RUN_TEST(test_reconnect_after_timeout_close_has_a_new_wait);
    RUN_TEST(test_failed_begin_send_of_idr_closes_and_does_not_count_as_delivered);

    RUN_TEST(test_clock_rollback_clamps_last_request_time);
    RUN_TEST(test_clock_rollback_clamps_key_wait_origin);
    RUN_TEST(test_clock_rollback_to_zero_does_not_underflow);

    RUN_TEST(test_close_requested_client_is_not_served_and_not_counted_as_waiting);

    RUN_TEST(test_fuzz_keyframe_gating_matches_model_and_invariants);
    return UNITY_END();
}

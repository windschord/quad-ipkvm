/*
 * ws_video_core: H.264 の申告 (accepts_h264。docs/VIDEO_TRANSPORT.md 9.4、9.6) のテスト。
 *  不変条件: H.264 を申告していないクライアントには、H.264 のフレーム (IDR も P も) を配らない。MJPEG のフレームは配る。
 *  - 接続したときは申告なし。申告なしのクライアントは、need_key にも、request_keyframe にも、key_wait_timeout_ms にも関わらない
 *  - ws_video_core_set_accepts_h264: false -> true は接続時の規則 (need_key + 要求)、true -> false は待ちを捨てる、同じ値は何もしない
 *  - 未知の fd・NULL は何もしない。スロットの再利用 (on_close、再接続) で、前のクライアントの申告を引き継がない
 *  - 申告ありのクライアントは、従来どおり (test_ws_video_core_keyframe.c)
 *  - 不変条件ファズ (固定シード): 申告なしに H.264 が配られない、申告なしが閉じられない、要求は待っている申告ありがいるときだけ、
 *    H.264 の P は直前の配信が H.264 の seq - 1 のときだけ
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
#define MAXDEL 4096

static ws_video_core_t core;

typedef struct {
    bool used;
    int held;
    int64_t last_seen;
} hubc_t;
static hubc_t hubc[MAXID];
static video_frame_t hub_frames[MAXID];
static uint8_t hub_data[MAXID][16]; // [0] = codec, [1] = flags, [2..9] = seq
static bool lat_on;
static int64_t lat_seq = -1;
static uint8_t lat_codec, lat_flags;
static int rel_count, double_hold, bad_release, bad_unregister;

static bool sending[MAXFD];
static bool dead[MAXFD];
static int close_cnt[MAXFD];
static int frames_tick[MAXFD];
static int begin_calls;
static int viol_double, viol_dead;

typedef struct {
    int fd;
    int64_t seq;
    uint8_t codec, flags;
} del_t;
static del_t dels[MAXDEL];
static int del_n;

static int req_count;
static int req_user;
static int req_unjustified; // 要求の時点で、「待っている申告ありのクライアント」がいなかった回数 (0 でなければならない)
static uint64_t g_now;

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
    if (sending[fd]) viol_double++;
    frames_tick[fd]++;
    sending[fd] = true;
    if (del_n < MAXDEL) {
        dels[del_n].fd = (int)fd;
        dels[del_n].codec = d[0];
        dels[del_n].flags = d[1];
        memcpy(&dels[del_n].seq, &d[2], sizeof(int64_t));
        del_n++;
    }
    return 0;
}
static void m_close(void *u, ws_video_conn_t c) {
    (void)u;
    uintptr_t fd = (uintptr_t)c;
    if (fd < MAXFD) { close_cnt[fd]++; dead[fd] = true; }
}
static bool m_validate(void *u, const uint8_t t[32]) { (void)u; (void)t; return true; }
static void m_request_keyframe(void *u) {
    TEST_ASSERT_EQUAL_PTR(&req_user, u);
    req_count++;
    bool justified = false;
    for (int i = 0; i < core.config.max_clients; i++) {
        if (core.clients.clients[i].state == WS_CLIENT_ACTIVE && !core.close_requested[i] && core.accepts_h264[i] &&
            core.need_key[i]) {
            justified = true;
        }
    }
    if (!justified) req_unjustified++;
}

static void reset_all(void) {
    memset(hubc, 0, sizeof(hubc));
    memset(hub_frames, 0, sizeof(hub_frames));
    memset(hub_data, 0, sizeof(hub_data));
    memset(sending, 0, sizeof(sending));
    memset(dead, 0, sizeof(dead));
    memset(close_cnt, 0, sizeof(close_cnt));
    memset(frames_tick, 0, sizeof(frames_tick));
    lat_on = false;
    lat_seq = -1;
    lat_codec = VIDEO_CODEC_MJPEG;
    lat_flags = 0;
    rel_count = double_hold = bad_release = bad_unregister = 0;
    begin_calls = 0;
    viol_double = viol_dead = 0;
    del_n = 0;
    req_count = req_unjustified = 0;
    g_now = 0;
}

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

/* 接続する。申告は、呼び出し側が ws_video_core_set_accepts_h264 で行う (行わなければ、申告なし) */
static int connect_fd(int fd, uint64_t now) {
    uint8_t tok[32] = {1};
    close_cnt[fd] = 0;
    dead[fd] = false;
    sending[fd] = false;
    int slot = ws_video_core_on_connect(&core, (ws_video_conn_t)(uintptr_t)fd, fd, tok, now);
    TEST_ASSERT_TRUE(slot >= 0);
    return slot;
}

static void done(int fd, bool ok) {
    ws_video_core_on_send_done(&core, fd, ok);
    sending[fd] = false;
}

static void tick(uint64_t now) {
    memset(frames_tick, 0, sizeof(frames_tick));
    g_now = now;
    ws_video_core_tick(&core, now);
}

static void publish(uint8_t codec, uint8_t flags) {
    lat_seq++;
    lat_on = true;
    lat_codec = codec;
    lat_flags = flags;
}
#define PUB_P() publish(VIDEO_CODEC_H264, 0)
#define PUB_IDR() publish(VIDEO_CODEC_H264, VIDEO_FRAME_FLAG_KEY)
#define PUB_JPEG() publish(VIDEO_CODEC_MJPEG, VIDEO_FRAME_FLAG_KEY)

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
    TEST_ASSERT_EQUAL(0, req_unjustified);
}

void setUp(void) { memset(&core, 0, sizeof(core)); reset_all(); }
void tearDown(void) { ws_video_core_destroy(&core); }

/* ================= (a) 申告なし ================= */

void test_connect_default_is_not_accepting(void) {
    start();
    int slot = connect_fd(10, 0);
    TEST_ASSERT_FALSE(core.accepts_h264[slot]);
    TEST_ASSERT_FALSE_MESSAGE(core.need_key[slot], "申告なしは、接続しても need_key にならない (待つものがない)");
}

void test_no_accept_never_gets_h264_idr_or_p_but_gets_mjpeg(void) {
    start();
    connect_fd(10, 0);

    PUB_IDR(); // seq 0
    tick(0);
    TEST_ASSERT_EQUAL(0, begin_calls);
    TEST_ASSERT_EQUAL(1, rel_count);
    TEST_ASSERT_EQUAL(0, holds_total());

    PUB_P(); // seq 1
    tick(10);
    TEST_ASSERT_EQUAL(0, begin_calls);
    PUB_IDR(); // seq 2
    tick(20);
    TEST_ASSERT_EQUAL(0, begin_calls);
    TEST_ASSERT_EQUAL(3, rel_count);
    TEST_ASSERT_EQUAL(0, holds_total());

    PUB_JPEG(); // seq 3
    TEST_ASSERT_TRUE(tick_delivers(10, 30));
    TEST_ASSERT_EQUAL(1, del_n);
    TEST_ASSERT_EQUAL(3, (int)dels[0].seq);
    TEST_ASSERT_EQUAL(VIDEO_CODEC_MJPEG, dels[0].codec);

    PUB_IDR(); // seq 4: MJPEG の後でも、H.264 は配らない
    tick(40);
    TEST_ASSERT_EQUAL(1, begin_calls);
    PUB_JPEG(); // seq 5 (seq 4 は配っていない = 飛び。MJPEG は、飛びに関係なく配る)
    TEST_ASSERT_TRUE(tick_delivers(10, 50));
    TEST_ASSERT_EQUAL(5, (int)dels[1].seq);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    TEST_ASSERT_EQUAL(0, holds_total());
    expect_no_violations();
}

void test_no_accept_unknown_codec_value_is_not_h264_and_is_delivered(void) {
    start();
    connect_fd(10, 0);
    publish(2, 0); // 契約の外の値は、MJPEG と同じに配る (H.264 だけを止める)
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    TEST_ASSERT_EQUAL(2, dels[0].codec);
}

void test_no_accept_h264_frame_with_same_seq_as_last_is_still_released_once(void) {
    start();
    connect_fd(10, 0);
    PUB_P();
    tick(0);
    tick(10); // 同じフレームは、ハブが二度返さない。release は 1 回
    TEST_ASSERT_EQUAL(1, rel_count);
    TEST_ASSERT_EQUAL(0, holds_total());
    expect_no_violations();
}

/* ================= (b) 申告あり: 従来どおり ================= */

void test_accepting_client_is_unchanged(void) {
    start();
    connect_fd(10, 0);
    ws_video_core_set_accepts_h264(&core, 10, true);
    PUB_P();
    tick(0);
    TEST_ASSERT_EQUAL(0, begin_calls); // IDR まで P は配らない
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    PUB_P();
    TEST_ASSERT_TRUE(tick_delivers(10, 20));
    PUB_JPEG();
    TEST_ASSERT_TRUE(tick_delivers(10, 30));
    TEST_ASSERT_EQUAL(3, del_n);
    expect_no_violations();
}

void test_mixed_clients_same_frames(void) {
    start();
    connect_fd(10, 0);
    ws_video_core_set_accepts_h264(&core, 10, true);
    connect_fd(11, 0); // 申告なし
    PUB_IDR();
    tick(0);
    TEST_ASSERT_EQUAL(1, frames_tick[10]);
    TEST_ASSERT_EQUAL(0, frames_tick[11]);
    done(10, true);
    PUB_P();
    tick(10);
    TEST_ASSERT_EQUAL(1, frames_tick[10]);
    TEST_ASSERT_EQUAL(0, frames_tick[11]);
    done(10, true);
    PUB_JPEG();
    tick(20);
    TEST_ASSERT_EQUAL(1, frames_tick[10]);
    TEST_ASSERT_EQUAL(1, frames_tick[11]);
    for (int i = 0; i < del_n; i++) {
        if (dels[i].fd == 11) TEST_ASSERT_EQUAL(VIDEO_CODEC_MJPEG, dels[i].codec);
    }
    expect_no_violations();
}

/* ================= (c) 申告なしは、要求も切断も起こさない ================= */

void test_no_accept_never_requests_and_never_times_out(void) {
    start();
    connect_fd(10, 0);
    uint64_t t = 0;
    for (int i = 0; i < 2000; i++) { // 20 秒。key_wait_timeout_ms は 5000
        PUB_P();
        if (i % 7 == 0) PUB_IDR();
        if (i % 11 == 0) PUB_P(); // seq の飛び
        tick(t);
        t += 10;
    }
    TEST_ASSERT_EQUAL(0, req_count);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    TEST_ASSERT_EQUAL(0, begin_calls);
    TEST_ASSERT_EQUAL(0, holds_total());
    expect_no_violations();
}

void test_no_accept_mjpeg_seq_gap_does_not_request(void) {
    start();
    connect_fd(10, 0);
    PUB_JPEG();
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    PUB_JPEG();
    PUB_JPEG(); // seq の飛び
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    tick(2000);
    TEST_ASSERT_EQUAL(0, req_count);
    TEST_ASSERT_FALSE(core.need_key[0]);
}

void test_no_accept_connect_does_not_request(void) {
    start();
    connect_fd(10, 0);
    tick(0);
    tick(5000);
    TEST_ASSERT_EQUAL(0, req_count);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
}

void test_requests_come_only_from_accepting_clients(void) {
    start();
    connect_fd(10, 0); // 申告なし
    connect_fd(11, 0);
    ws_video_core_set_accepts_h264(&core, 11, true); // 申告あり: need_key
    PUB_P();
    tick(0);
    TEST_ASSERT_EQUAL(1, req_count);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    // 申告ありの待ちが 5 秒続けば、申告ありだけが閉じられる
    PUB_P();
    tick(1000);
    PUB_P();
    tick(5000);
    TEST_ASSERT_EQUAL(1, close_cnt[11]);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    expect_no_violations();
}

/* ================= (d) 途中で申告を変える ================= */

void test_accept_set_later_applies_connect_rule(void) {
    start();
    connect_fd(10, 0);
    PUB_P();
    tick(0);
    TEST_ASSERT_EQUAL(0, req_count);
    TEST_ASSERT_EQUAL(0, begin_calls);

    ws_video_core_set_accepts_h264(&core, 10, true);
    TEST_ASSERT_TRUE(core.need_key[0]);
    TEST_ASSERT_EQUAL_MESSAGE(0, req_count, "要求は set の中では出ない (tick の最後)");
    tick(10);
    TEST_ASSERT_EQUAL(1, req_count);
    PUB_P();
    tick(20);
    TEST_ASSERT_EQUAL(0, begin_calls); // IDR まで P は配らない
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 30));
    TEST_ASSERT_EQUAL(VIDEO_FRAME_FLAG_KEY, dels[0].flags);
    PUB_P();
    TEST_ASSERT_TRUE(tick_delivers(10, 40));
    expect_no_violations();
}

void test_accept_revoked_drops_the_wait_and_the_pending_request(void) {
    start();
    connect_fd(10, 0);
    ws_video_core_set_accepts_h264(&core, 10, true);
    PUB_P();
    tick(0); // 要求を 1 回出して、P を配らず、待ちの起点を持つ
    TEST_ASSERT_EQUAL(1, req_count);
    TEST_ASSERT_TRUE(core.key_waiting[0]);
    ws_video_core_set_accepts_h264(&core, 10, false);
    TEST_ASSERT_FALSE(core.need_key[0]);
    TEST_ASSERT_FALSE(core.key_waiting[0]);
    PUB_P();
    tick(20000); // 待ちが 5 秒を超えても、閉じない
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    TEST_ASSERT_EQUAL(1, req_count);
    PUB_JPEG();
    TEST_ASSERT_TRUE(tick_delivers(10, 20010));
    expect_no_violations();
}

void test_accept_revoked_drops_a_pending_request(void) {
    start();
    // 要求の間隔の中で溜まった要求 (idr_min_interval 1000) は、申告を外して誰も待たなくなれば捨てられる
    connect_fd(10, 0);
    ws_video_core_set_accepts_h264(&core, 10, true);
    PUB_P();
    tick(0);
    TEST_ASSERT_EQUAL(1, req_count);
    connect_fd(11, 100);
    ws_video_core_set_accepts_h264(&core, 11, true); // 新しい need_key = 溜まる (間隔の中)
    ws_video_core_set_accepts_h264(&core, 11, false);
    ws_video_core_set_accepts_h264(&core, 10, false);
    tick(1500);
    TEST_ASSERT_EQUAL(1, req_count);
}

void test_set_same_value_is_a_noop(void) {
    start();
    connect_fd(10, 0);
    ws_video_core_set_accepts_h264(&core, 10, true);
    PUB_IDR();
    TEST_ASSERT_TRUE(tick_delivers(10, 0));
    TEST_ASSERT_FALSE(core.need_key[0]);
    ws_video_core_set_accepts_h264(&core, 10, true); // 同じ値: need_key を立て直さない
    TEST_ASSERT_FALSE(core.need_key[0]);
    PUB_P();
    TEST_ASSERT_TRUE(tick_delivers(10, 10));
    ws_video_core_set_accepts_h264(&core, 10, false);
    ws_video_core_set_accepts_h264(&core, 10, false);
    TEST_ASSERT_FALSE(core.need_key[0]);
    TEST_ASSERT_EQUAL(0, req_count);
}

/* ================= (e) スロットの再利用 ================= */

void test_slot_reuse_after_close_does_not_inherit_accept(void) {
    start();
    int s1 = connect_fd(10, 0);
    ws_video_core_set_accepts_h264(&core, 10, true);
    TEST_ASSERT_TRUE(core.accepts_h264[s1]);
    ws_video_core_on_close(&core, 10);
    TEST_ASSERT_FALSE_MESSAGE(core.accepts_h264[s1], "on_close で戻す");
    int s2 = connect_fd(11, 0);
    TEST_ASSERT_EQUAL(s1, s2);
    TEST_ASSERT_FALSE(core.accepts_h264[s2]);
    PUB_IDR();
    tick(0);
    TEST_ASSERT_EQUAL(0, begin_calls);
    expect_no_violations();
}

void test_slot_reuse_without_close_notification_resets_on_connect(void) {
    start();
    int s1 = connect_fd(10, 0);
    ws_video_core_set_accepts_h264(&core, 10, true);
    ws_video_core_on_close(&core, 10);
    core.accepts_h264[s1] = true; // 何かの経路で値が残っていても、接続で戻す
    int s2 = connect_fd(11, 0);
    TEST_ASSERT_EQUAL(s1, s2);
    TEST_ASSERT_FALSE(core.accepts_h264[s2]);
}

void test_close_while_sending_resets_accept_and_new_client_waits_for_the_slot(void) {
    start();
    int s1 = connect_fd(10, 0);
    ws_video_core_set_accepts_h264(&core, 10, true);
    PUB_IDR();
    tick(0); // 送信中
    TEST_ASSERT_EQUAL(1, frames_tick[10]);
    ws_video_core_on_close(&core, 10);
    TEST_ASSERT_FALSE(core.accepts_h264[s1]);
    done(10, true); // CLOSING が解放される
    int s2 = connect_fd(11, 0);
    TEST_ASSERT_FALSE(core.accepts_h264[s2]);
    expect_no_violations();
}

/* ================= (f) 未知の fd・NULL ================= */

void test_unknown_fd_and_null_do_nothing(void) {
    start();
    int s = connect_fd(10, 0);
    ws_video_core_set_accepts_h264(NULL, 10, true);
    ws_video_core_set_accepts_h264(&core, 999, true);
    ws_video_core_set_accepts_h264(&core, -1, true);
    ws_video_core_set_accepts_h264(&core, 11, true); // 接続していない
    TEST_ASSERT_FALSE(core.accepts_h264[s]);
    for (int i = 0; i < WS_CLIENTS_MAX; i++) TEST_ASSERT_FALSE(core.accepts_h264[i]);
    ws_video_core_set_accepts_h264(&core, 10, true);
    ws_video_core_on_close(&core, 10);
    ws_video_core_set_accepts_h264(&core, 10, true); // 閉じた後 (スロットは解放済み)
    for (int i = 0; i < WS_CLIENTS_MAX; i++) TEST_ASSERT_FALSE(core.accepts_h264[i]);
    TEST_ASSERT_EQUAL(0, req_count);
}

void test_set_does_not_touch_other_clients(void) {
    start();
    int a = connect_fd(10, 0);
    int b = connect_fd(11, 0);
    ws_video_core_set_accepts_h264(&core, 11, true);
    TEST_ASSERT_FALSE(core.accepts_h264[a]);
    TEST_ASSERT_TRUE(core.accepts_h264[b]);
    TEST_ASSERT_FALSE(core.need_key[a]);
    TEST_ASSERT_TRUE(core.need_key[b]);
}

/* ================= (g) 不変条件ファズ ================= */

static uint32_t rng;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static long fz_delivered_h264, fz_delivered_mjpeg_noaccept, fz_requests, fz_toggles, fz_closes_accepting;

static void run_fuzz_seed(uint32_t seed) {
    rng = seed * 2654435761u + 12345u;
    ws_video_core_config_t cfg = base_cfg();
    cfg.max_clients = 1 + (int)(rnd() % 4);
    cfg.idr_min_interval_ms = (rnd() & 1) ? 1000 : 0;
    cfg.key_wait_timeout_ms = (rnd() & 1) ? 5000 : 0;
    if ((rnd() % 5) == 0) cfg.request_keyframe = NULL;
    start_cfg(&cfg);

    bool conn[MAXFD] = {0};
    bool acc[MAXFD] = {0};
    bool never_acc[MAXFD] = {0};   // 接続してから、一度も申告したことがない
    int64_t last_seq[MAXFD];
    int last_codec[MAXFD];
    for (int i = 0; i < MAXFD; i++) { last_seq[i] = -1; last_codec[i] = -1; }
    uint64_t now = 0;
    const int first_fd = 10;
    const int nfd = 5;
    int req_before = 0;

    for (int step = 0; step < 1500; step++) {
        int act = (int)(rnd() % 100);
        int fd = first_fd + (int)(rnd() % (uint32_t)nfd);
        int del_from = del_n;
        if (act < 8) {
            if (!conn[fd] && !sending[fd]) {
                uint8_t tok[32] = {1};
                dead[fd] = false;
                close_cnt[fd] = 0;
                int slot = ws_video_core_on_connect(&core, (ws_video_conn_t)(uintptr_t)fd, fd, tok, now);
                if (slot >= 0) {
                    conn[fd] = true;
                    acc[fd] = false;
                    never_acc[fd] = true;
                    last_seq[fd] = -1;
                    last_codec[fd] = -1;
                    if (rnd() % 100 < 60) {
                        ws_video_core_set_accepts_h264(&core, fd, true);
                        acc[fd] = true;
                        never_acc[fd] = false;
                    }
                }
            }
        } else if (act < 12) {
            if (conn[fd]) {
                ws_video_core_set_accepts_h264(&core, fd, !acc[fd]);
                acc[fd] = !acc[fd];
                if (acc[fd]) never_acc[fd] = false;
                fz_toggles++;
            }
        } else if (act < 16) {
            if (conn[fd]) {
                ws_video_core_on_close(&core, fd);
                conn[fd] = false;
                acc[fd] = false;
            }
        } else if (act < 45) {
            uint32_t r = rnd() % 100;
            if (r < 8) lat_seq++; // 配られない seq (飛び)
            if (r < 50) PUB_P();
            else if (r < 62) PUB_IDR();
            else PUB_JPEG();
        } else if (act < 75) {
            now += rnd() % 30;
            if (rnd() % 40 == 0) now += 1500 + rnd() % 4000; // 待ちの期限・要求の間隔を越える
            req_before = req_count;
            tick(now);
            fz_requests += req_count - req_before;
        } else {
            // 送信の完了
            for (int f = first_fd; f < first_fd + nfd; f++) {
                if (sending[f] && (rnd() & 1)) done(f, true);
            }
        }

        // 配られた分の検査
        for (int i = del_from; i < del_n; i++) {
            int f = dels[i].fd;
            if (dels[i].codec == VIDEO_CODEC_H264) {
                TEST_ASSERT_TRUE_MESSAGE(acc[f], "H.264 was delivered to a client that did not accept it");
                fz_delivered_h264++;
                if (!(dels[i].flags & VIDEO_FRAME_FLAG_KEY)) {
                    TEST_ASSERT_TRUE_MESSAGE(last_codec[f] == VIDEO_CODEC_H264 && last_seq[f] + 1 == dels[i].seq,
                                             "H.264 P frame delivered without the previous frame of the stream");
                }
            } else if (!acc[f]) {
                fz_delivered_mjpeg_noaccept++;
            }
            last_seq[f] = dels[i].seq;
            last_codec[f] = dels[i].codec;
        }
        for (int f = first_fd; f < first_fd + nfd; f++) {
            if (never_acc[f]) {
                TEST_ASSERT_EQUAL_MESSAGE(0, close_cnt[f], "a client that never accepted H.264 was closed");
            }
            int sl = ws_clients_find(&core.clients, f);
            if (conn[f] && sl >= 0) TEST_ASSERT_EQUAL_MESSAGE(acc[f], core.accepts_h264[sl], "accept flag differs from the model");
            if (conn[f] && sl >= 0 && !acc[f]) {
                TEST_ASSERT_FALSE_MESSAGE(core.need_key[sl], "a client that does not accept H.264 has need_key");
            }
        }
        TEST_ASSERT_EQUAL(0, viol_double);
        TEST_ASSERT_EQUAL(0, viol_dead);
        TEST_ASSERT_EQUAL(0, double_hold);
        TEST_ASSERT_EQUAL(0, bad_release);
        TEST_ASSERT_EQUAL(0, bad_unregister);
        TEST_ASSERT_EQUAL_MESSAGE(0, req_unjustified, "keyframe requested while no accepting client was waiting");

        // 閉じられたクライアントは、テスト側でも切断扱いにする (on_close を呼ぶ)
        for (int f = first_fd; f < first_fd + nfd; f++) {
            if (conn[f] && dead[f]) {
                if (acc[f]) fz_closes_accepting++;
                ws_video_core_on_close(&core, f);
                conn[f] = false;
                acc[f] = false;
                if (sending[f]) done(f, false);
            }
        }
    }
}

void test_fuzz_accepts_invariants(void) {
    for (uint32_t seed = 1; seed <= 300; seed++) {
        run_fuzz_seed(seed);
        ws_video_core_destroy(&core);
    }
    printf("fuzz: h264-deliveries=%ld mjpeg-to-noaccept=%ld requests=%ld toggles=%ld accepting-closes=%ld\n", fz_delivered_h264,
           fz_delivered_mjpeg_noaccept, fz_requests, fz_toggles, fz_closes_accepting);
    TEST_ASSERT_TRUE_MESSAGE(fz_delivered_h264 > 5000, "fuzz delivered too few H.264 frames");
    TEST_ASSERT_TRUE_MESSAGE(fz_delivered_mjpeg_noaccept > 5000, "fuzz delivered too few MJPEG frames to non-accepting clients");
    TEST_ASSERT_TRUE_MESSAGE(fz_requests > 500, "fuzz made too few requests");
    TEST_ASSERT_TRUE_MESSAGE(fz_toggles > 500, "fuzz toggled too few times");
    TEST_ASSERT_TRUE_MESSAGE(fz_closes_accepting > 50, "fuzz never timed out a waiting accepting client");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_connect_default_is_not_accepting);
    RUN_TEST(test_no_accept_never_gets_h264_idr_or_p_but_gets_mjpeg);
    RUN_TEST(test_no_accept_unknown_codec_value_is_not_h264_and_is_delivered);
    RUN_TEST(test_no_accept_h264_frame_with_same_seq_as_last_is_still_released_once);
    RUN_TEST(test_accepting_client_is_unchanged);
    RUN_TEST(test_mixed_clients_same_frames);
    RUN_TEST(test_no_accept_never_requests_and_never_times_out);
    RUN_TEST(test_no_accept_mjpeg_seq_gap_does_not_request);
    RUN_TEST(test_no_accept_connect_does_not_request);
    RUN_TEST(test_requests_come_only_from_accepting_clients);
    RUN_TEST(test_accept_set_later_applies_connect_rule);
    RUN_TEST(test_accept_revoked_drops_the_wait_and_the_pending_request);
    RUN_TEST(test_accept_revoked_drops_a_pending_request);
    RUN_TEST(test_set_same_value_is_a_noop);
    RUN_TEST(test_slot_reuse_after_close_does_not_inherit_accept);
    RUN_TEST(test_slot_reuse_without_close_notification_resets_on_connect);
    RUN_TEST(test_close_while_sending_resets_accept_and_new_client_waits_for_the_slot);
    RUN_TEST(test_unknown_fd_and_null_do_nothing);
    RUN_TEST(test_set_does_not_touch_other_clients);
    RUN_TEST(test_fuzz_accepts_invariants);
    return UNITY_END();
}

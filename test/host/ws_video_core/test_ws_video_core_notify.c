/*
 * ws_video_core: 映像の状態の通知 (C-6b。docs/VIDEO_TRANSPORT.md 8 章、docs/WEB_API.md 5 章) のテスト。
 *  - 接続直後に 1 回、同じ状態では送らない、状態・解像度の変化で 1 回
 *  - 送信中 (is_sending) は送らない。通知が先でフレームが後 (同じ tick では、通知を始めたらフレームは次の tick 以降)
 *  - テキストの送信失敗で閉じる。on_send_done はテキストとフレームを区別する (テキストの後にフレームの解放が起きない)
 *  - 最短間隔 (境界 ±1 ms、間隔の間の変化は最新の 1 回だけ、接続直後には適用しない、時刻の巻き戻り)
 *  - 期限 (テキスト): on_send_begin、queue_timeout_ms、send_timeout_ms、size = テキストの長さ
 *  - status_get == NULL なら従来どおり。複数のクライアントが独立。close_requested のクライアントには送らない
 *  - 不変条件ファズ (固定シード。独立したモデルで、tick ごとに「テキストを送るべきクライアント」を判定する)
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
#define MAXLOG 64

#define J_UNKNOWN "{\"t\":\"video\",\"state\":\"unknown\",\"w\":0,\"h\":0}"
#define J_NOSIG "{\"t\":\"video\",\"state\":\"no_signal\",\"w\":0,\"h\":0}"
#define J_720 "{\"t\":\"video\",\"state\":\"locked\",\"w\":1280,\"h\":720}"
#define J_1080 "{\"t\":\"video\",\"state\":\"locked\",\"w\":1920,\"h\":1080}"

enum { K_NONE = 0, K_FRAME = 1, K_TEXT = 2 };

static ws_video_core_t core;

/* ---- ハブのモック ---- */
static bool hub_used[MAXID];
static int hub_held[MAXID];
static video_frame_t hub_frames[MAXID];
static uint8_t dummy_data[8];
static int64_t g_seq = 1;
static bool hub_frame_on;     // false なら get_latest は NULL (フレームなし)
static size_t hub_frame_size; // 次のフレームの大きさ
static int hub_get_count, rel_count, unreg_count, reg_count;
static int double_hold, bad_release, bad_unregister;

/* ---- 送信のモック ---- */
static int kind[MAXFD];             // fd ごとの、送信中のもの (K_*)。テストの done() が K_NONE に戻す
static bool dead[MAXFD];            // 閉じる要求が出た / 切断された (以後、送ってはならない)
static int close_cnt[MAXFD];
static int frames_tick[MAXFD], texts_tick[MAXFD]; // 直近に数えを戻してからの開始の回数
static int text_count[MAXFD];       // fd ごとのテキストの開始の回数 (接続のたびに 0)
static int frame_begin_count;
static int text_begin_calls;        // io_begin_send_text の呼び出し回数 (失敗も数える)
static int begin_ret, text_ret;
static int viol_double, viol_dead, viol_release_text;
static int cur_done_fd = -1;

static char text_log[MAXLOG][160];
static int text_log_fd[MAXLOG];
static size_t text_log_len[MAXLOG];
static const uint8_t *text_log_ptr[MAXLOG];
static int text_log_n;
static char ev_log[MAXLOG * 2];
static int ev_n;
static void *last_user_data;
static int dummy_user;

static video_status_t g_status;
static int status_get_count;
static bool status_writes; // false なら status_get は out を書かない
static bool status_flip;   // true なら status_get は、呼ばれるたびに違う値 (幅が 1 増える LOCKED) を返す

static int m_register(void *u) {
    (void)u;
    for (int i = 0; i < MAXID; i++) {
        if (!hub_used[i]) {
            hub_used[i] = true;
            hub_held[i] = 0;
            reg_count++;
            return i;
        }
    }
    return -1;
}
static void m_unregister(void *u, int id) {
    (void)u;
    if (id < 0 || id >= MAXID || !hub_used[id]) { bad_unregister++; return; }
    hub_used[id] = false;
    hub_held[id] = 0;
    unreg_count++;
}
static const video_frame_t *m_get_latest(void *u, int id) {
    (void)u;
    hub_get_count++;
    if (!hub_frame_on) return NULL;
    if (hub_held[id] > 0) double_hold++;
    hub_held[id]++;
    hub_frames[id].data = dummy_data;
    hub_frames[id].size = hub_frame_size;
    hub_frames[id].seq = g_seq;
    return &hub_frames[id];
}
static void m_release(void *u, int id, const video_frame_t *f) {
    (void)u; (void)f;
    if (hub_held[id] <= 0) { bad_release++; return; }
    hub_held[id]--;
    rel_count++;
    /* テキストの送信の完了で、フレームの解放が起きてはならない */
    if (cur_done_fd >= 0 && kind[cur_done_fd] == K_TEXT) viol_release_text++;
}
static int m_begin_send(void *u, ws_video_conn_t c, const uint8_t *d, size_t l) {
    (void)u; (void)d; (void)l;
    uintptr_t fd = (uintptr_t)c;
    frame_begin_count++;
    frames_tick[fd]++;
    if (dead[fd]) viol_dead++;
    if (kind[fd] != K_NONE) viol_double++;
    if (ev_n < (int)sizeof(ev_log)) ev_log[ev_n++] = 'F';
    if (begin_ret != 0) return begin_ret;
    kind[fd] = K_FRAME;
    return 0;
}
static int m_begin_send_text(void *u, ws_video_conn_t c, const uint8_t *d, size_t l) {
    uintptr_t fd = (uintptr_t)c;
    last_user_data = u;
    text_begin_calls++;
    texts_tick[fd]++;
    if (dead[fd]) viol_dead++;
    if (kind[fd] != K_NONE) viol_double++;
    if (ev_n < (int)sizeof(ev_log)) ev_log[ev_n++] = 'T';
    if (text_log_n < MAXLOG) {
        size_t n = l < sizeof(text_log[0]) - 1 ? l : sizeof(text_log[0]) - 1;
        memcpy(text_log[text_log_n], d, n);
        text_log[text_log_n][n] = '\0';
        text_log_fd[text_log_n] = (int)fd;
        text_log_len[text_log_n] = l;
        text_log_ptr[text_log_n] = d;
        text_log_n++;
    }
    if (text_ret != 0) return text_ret;
    text_count[fd]++;
    kind[fd] = K_TEXT;
    return 0;
}
static void m_close(void *u, ws_video_conn_t c) {
    (void)u;
    uintptr_t fd = (uintptr_t)c;
    if (fd < MAXFD) { close_cnt[fd]++; dead[fd] = true; }
}
static bool val_ret = true;
static bool m_validate(void *u, const uint8_t t[32]) { (void)u; (void)t; return val_ret; }
static void m_status_get(void *u, video_status_t *out) {
    (void)u;
    status_get_count++;
    if (!status_writes) return;
    *out = g_status;
    if (status_flip) {
        out->state = KVM_VIDEO_SYNC_LOCKED;
        out->width = 100u + (uint32_t)status_get_count;
        out->height = 50;
    }
}

static void reset_all(void) {
    memset(hub_used, 0, sizeof(hub_used));
    memset(hub_held, 0, sizeof(hub_held));
    memset(kind, 0, sizeof(kind));
    memset(dead, 0, sizeof(dead));
    memset(close_cnt, 0, sizeof(close_cnt));
    memset(frames_tick, 0, sizeof(frames_tick));
    memset(texts_tick, 0, sizeof(texts_tick));
    memset(text_count, 0, sizeof(text_count));
    hub_frame_on = false;
    hub_frame_size = 4;
    g_seq = 1;
    hub_get_count = rel_count = unreg_count = reg_count = 0;
    double_hold = bad_release = bad_unregister = 0;
    frame_begin_count = text_begin_calls = 0;
    begin_ret = text_ret = 0;
    viol_double = viol_dead = viol_release_text = 0;
    cur_done_fd = -1;
    text_log_n = 0;
    ev_n = 0;
    last_user_data = NULL;
    g_status.state = KVM_VIDEO_SYNC_UNKNOWN;
    g_status.width = 0;
    g_status.height = 0;
    status_get_count = 0;
    status_writes = true;
    status_flip = false;
    val_ret = true;
}

/* 通知ありの既定の設定。テストが、必要な項目を書き換えて start_cfg に渡す */
static ws_video_core_config_t base_cfg(uint64_t min_interval) {
    ws_video_core_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_clients = 4;
    cfg.session_check_interval_ms = 1000000000ull;
    cfg.status_get = m_status_get;
    cfg.io_begin_send_text = m_begin_send_text;
    cfg.status_min_interval_ms = min_interval;
    cfg.hub_register = m_register;
    cfg.hub_unregister = m_unregister;
    cfg.hub_get_latest = m_get_latest;
    cfg.hub_release = m_release;
    cfg.io_begin_send = m_begin_send;
    cfg.io_request_close = m_close;
    cfg.io_validate_session = m_validate;
    cfg.user_data = &dummy_user;
    return cfg;
}

static void start_cfg(const ws_video_core_config_t *cfg) {
    ws_video_core_destroy(&core);
    reset_all();
    TEST_ASSERT_EQUAL(0, ws_video_core_init(&core, cfg));
}

static void start(uint64_t min_interval) {
    ws_video_core_config_t cfg = base_cfg(min_interval);
    start_cfg(&cfg);
}

static int connect_fd(int fd, uint64_t now) {
    uint8_t tok[32] = {1};
    close_cnt[fd] = 0;
    dead[fd] = false;
    kind[fd] = K_NONE;
    text_count[fd] = 0;
    int slot = ws_video_core_on_connect(&core, (ws_video_conn_t)(uintptr_t)fd, fd, tok, now);
    TEST_ASSERT_TRUE(slot >= 0);
    return slot;
}

static void done(int fd, bool ok) {
    cur_done_fd = fd;
    ws_video_core_on_send_done(&core, fd, ok);
    cur_done_fd = -1;
    kind[fd] = K_NONE;
}

static void status_set(kvm_video_sync_state_t s, uint32_t w, uint32_t h) {
    g_status.state = s;
    g_status.width = w;
    g_status.height = h;
}

static void tick(uint64_t now) {
    memset(frames_tick, 0, sizeof(frames_tick));
    memset(texts_tick, 0, sizeof(texts_tick));
    ws_video_core_tick(&core, now);
}

static void expect_no_violations(void) {
    TEST_ASSERT_EQUAL(0, viol_double);
    TEST_ASSERT_EQUAL(0, viol_dead);
    TEST_ASSERT_EQUAL(0, viol_release_text);
    TEST_ASSERT_EQUAL(0, double_hold);
    TEST_ASSERT_EQUAL(0, bad_release);
    TEST_ASSERT_EQUAL(0, bad_unregister);
}

void setUp(void) { memset(&core, 0, sizeof(core)); reset_all(); }
void tearDown(void) { ws_video_core_destroy(&core); }

/* ================= init ================= */

void test_init_status_get_requires_io_begin_send_text(void) {
    ws_video_core_t c;
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.io_begin_send_text = NULL;
    TEST_ASSERT_EQUAL(-1, ws_video_core_init(&c, &cfg));
    cfg.io_begin_send_text = m_begin_send_text;
    TEST_ASSERT_EQUAL(0, ws_video_core_init(&c, &cfg));
    ws_video_core_destroy(&c);
}

void test_init_without_status_get_does_not_need_text_callback(void) {
    ws_video_core_t c;
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.status_get = NULL;
    cfg.io_begin_send_text = NULL;
    TEST_ASSERT_EQUAL(0, ws_video_core_init(&c, &cfg));
    ws_video_core_destroy(&c);
    cfg.io_begin_send_text = m_begin_send_text; // status_get なしで text だけあってもよい
    TEST_ASSERT_EQUAL(0, ws_video_core_init(&c, &cfg));
    ws_video_core_destroy(&c);
}

/* ================= 接続直後に 1 回 ================= */

void test_first_tick_after_connect_sends_status_once(void) {
    start(0);
    int slot = connect_fd(10, 0);
    TEST_ASSERT_EQUAL(0, text_begin_calls);
    tick(0);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    TEST_ASSERT_EQUAL(1, text_log_n);
    TEST_ASSERT_EQUAL(10, text_log_fd[0]);
    TEST_ASSERT_EQUAL_STRING(J_UNKNOWN, text_log[0]);
    TEST_ASSERT_EQUAL_UINT((unsigned)strlen(J_UNKNOWN), (unsigned)text_log_len[0]);
    TEST_ASSERT_TRUE(core.clients.clients[slot].is_sending);
    TEST_ASSERT_TRUE(core.sending_text[slot]);
    TEST_ASSERT_NULL(core.current_frames[slot]);
    TEST_ASSERT_EQUAL_UINT((unsigned)strlen(J_UNKNOWN), (unsigned)core.text_len[slot]);
    TEST_ASSERT_EQUAL_PTR(&dummy_user, last_user_data);
    expect_no_violations();
}

void test_first_status_is_the_current_status_not_a_fixed_unknown(void) {
    start(0);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    connect_fd(10, 0);
    tick(0);
    TEST_ASSERT_EQUAL(1, text_log_n);
    TEST_ASSERT_EQUAL_STRING(J_720, text_log[0]);
}

void test_no_text_before_first_tick_and_not_for_unconnected(void) {
    start(0);
    tick(0);
    TEST_ASSERT_EQUAL(0, text_begin_calls);
}

/* ================= 同じ状態では送らない / 変化で 1 回 ================= */

void test_same_status_is_not_resent(void) {
    start(0);
    connect_fd(10, 0);
    tick(0);
    done(10, true);
    for (uint64_t t = 1; t <= 20; t++) tick(t * 100);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    TEST_ASSERT_EQUAL(1, text_count[10]);
}

void test_state_change_sends_once(void) {
    start(0);
    connect_fd(10, 0);
    tick(0);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    tick(1);
    TEST_ASSERT_EQUAL(2, text_log_n);
    TEST_ASSERT_EQUAL_STRING(J_NOSIG, text_log[1]);
    done(10, true);
    tick(2);
    tick(3);
    TEST_ASSERT_EQUAL(2, text_begin_calls);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    tick(4);
    TEST_ASSERT_EQUAL(3, text_log_n);
    TEST_ASSERT_EQUAL_STRING(J_720, text_log[2]);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_UNKNOWN, 0, 0);
    tick(5);
    TEST_ASSERT_EQUAL(4, text_log_n);
    TEST_ASSERT_EQUAL_STRING(J_UNKNOWN, text_log[3]);
    expect_no_violations();
}

void test_resolution_change_sends_once(void) {
    start(0);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    connect_fd(10, 0);
    tick(0);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    tick(1);
    TEST_ASSERT_EQUAL(2, text_log_n);
    TEST_ASSERT_EQUAL_STRING(J_1080, text_log[1]);
    done(10, true);
    tick(2);
    TEST_ASSERT_EQUAL(2, text_begin_calls);
    // 幅だけ・高さだけの変化も、それぞれ 1 回
    status_set(KVM_VIDEO_SYNC_LOCKED, 1920, 1081);
    tick(3);
    TEST_ASSERT_EQUAL(3, text_begin_calls);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1921, 1081);
    tick(4);
    TEST_ASSERT_EQUAL(4, text_begin_calls);
    done(10, true);
    tick(5);
    TEST_ASSERT_EQUAL(4, text_begin_calls);
}

void test_change_and_change_back_before_tick_sends_nothing(void) {
    start(0);
    connect_fd(10, 0);
    tick(0);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_LOCKED, 640, 480);
    status_set(KVM_VIDEO_SYNC_UNKNOWN, 0, 0); // tick の前に戻る = 最後に送ったものと同じ
    tick(1);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
}

void test_status_get_that_writes_nothing_is_unknown(void) {
    start(0);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    status_writes = false;
    connect_fd(10, 0);
    tick(0);
    TEST_ASSERT_EQUAL(1, text_log_n);
    TEST_ASSERT_EQUAL_STRING(J_UNKNOWN, text_log[0]);
}

void test_out_of_range_status_does_not_crash_and_is_unknown_json(void) {
    start(0);
    status_set((kvm_video_sync_state_t)99, 5, 6);
    connect_fd(10, 0);
    tick(0);
    TEST_ASSERT_EQUAL_STRING(J_UNKNOWN, text_log[0]);
    done(10, true);
    tick(1);
    TEST_ASSERT_EQUAL(1, text_begin_calls); // 同じ値は、再送しない
    status_set(KVM_VIDEO_SYNC_LOCKED, 0, 0); // 不整合 (LOCKED で 0)
    tick(2);
    TEST_ASSERT_EQUAL_STRING(J_UNKNOWN, text_log[text_log_n - 1]);
    done(10, true);
    status_set((kvm_video_sync_state_t)-5, UINT32_MAX, UINT32_MAX);
    tick(3);
    TEST_ASSERT_EQUAL_STRING(J_UNKNOWN, text_log[text_log_n - 1]);
    expect_no_violations();
}

/* ================= 送信中は送らない ================= */

void test_no_text_while_text_in_flight_then_latest_after_done(void) {
    start(0);
    connect_fd(10, 0);
    tick(0);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    for (uint64_t t = 1; t < 50; t++) tick(t); // テキストの送信中
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    done(10, true);
    tick(50);
    TEST_ASSERT_EQUAL(2, text_begin_calls);
    TEST_ASSERT_EQUAL_STRING(J_720, text_log[1]);
    expect_no_violations();
}

void test_no_text_while_frame_in_flight_and_text_first_after(void) {
    start(0);
    hub_frame_on = true;
    connect_fd(10, 0);
    tick(0);              // テキスト (フレームはまだ)
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    TEST_ASSERT_EQUAL(0, frame_begin_count);
    done(10, true);
    tick(1);              // フレーム
    TEST_ASSERT_EQUAL(1, frame_begin_count);
    TEST_ASSERT_EQUAL(K_FRAME, kind[10]);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    for (uint64_t t = 2; t < 30; t++) tick(t); // フレームの送信中は、通知しない
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    TEST_ASSERT_EQUAL(1, frame_begin_count);
    done(10, true);
    g_seq++;              // 新しいフレームもある
    int gets = hub_get_count;
    tick(30);             // 通知が先。このクライアントのフレームは、この tick では送らない
    TEST_ASSERT_EQUAL(2, text_begin_calls);
    TEST_ASSERT_EQUAL(1, frame_begin_count);
    TEST_ASSERT_EQUAL(gets, hub_get_count); // フレームの取得もしていない
    TEST_ASSERT_EQUAL(K_TEXT, kind[10]);
    tick(31);             // テキストの送信中は、フレームも送らない
    TEST_ASSERT_EQUAL(1, frame_begin_count);
    done(10, true);
    tick(32);             // 次の tick 以降: フレーム
    TEST_ASSERT_EQUAL(2, frame_begin_count);
    TEST_ASSERT_EQUAL(K_FRAME, kind[10]);
    TEST_ASSERT_EQUAL(4, ev_n);
    TEST_ASSERT_EQUAL_MEMORY("TFTF", ev_log, 4);
    expect_no_violations();
}

void test_new_client_gets_text_then_frame_in_order(void) {
    start(0);
    hub_frame_on = true;
    connect_fd(10, 0);
    tick(0);
    done(10, true);
    tick(1);
    TEST_ASSERT_EQUAL_MEMORY("TF", ev_log, 2);
    TEST_ASSERT_EQUAL(2, ev_n);
}

void test_frames_continue_when_status_unchanged(void) {
    start(0);
    hub_frame_on = true;
    connect_fd(10, 0);
    tick(0);
    done(10, true);
    for (int i = 0; i < 5; i++) {
        tick(1 + (uint64_t)i * 2);
        TEST_ASSERT_EQUAL(K_FRAME, kind[10]);
        done(10, true);
        g_seq++;
    }
    TEST_ASSERT_EQUAL(5, frame_begin_count);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    TEST_ASSERT_EQUAL(5, rel_count);
}

/* ================= テキストの送信の失敗 ================= */

void test_text_send_failure_closes_the_client(void) {
    start(0);
    hub_frame_on = true;
    connect_fd(10, 0);
    text_ret = -1;
    tick(0);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_FALSE(core.clients.clients[0].is_sending);
    TEST_ASSERT_FALSE(core.sending_text[0]);
    TEST_ASSERT_EQUAL(0, frame_begin_count);   // このクライアントのフレームは、同じ tick でも送らない
    TEST_ASSERT_EQUAL(0, hub_get_count);
    TEST_ASSERT_EQUAL(0, rel_count);
    // 以後、閉じる要求の済んだクライアントには送らない (close_requested)
    text_ret = 0;
    tick(1);
    tick(2);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    TEST_ASSERT_EQUAL(0, frame_begin_count);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    ws_video_core_on_close(&core, 10);
    TEST_ASSERT_EQUAL(1, unreg_count);
    expect_no_violations();
}

void test_text_send_failure_with_positive_return_also_closes(void) {
    start(0);
    connect_fd(10, 0);
    text_ret = 7; // 0 以外はすべて失敗
    tick(0);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_FALSE(core.clients.clients[0].is_sending);
}

void test_text_failure_does_not_affect_other_clients(void) {
    start(0);
    connect_fd(10, 0);
    connect_fd(11, 0);
    text_ret = -1;
    tick(0); // 両方失敗
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_EQUAL(1, close_cnt[11]);
    // 1 つだけ失敗させる
    start(0);
    connect_fd(10, 0);
    tick(0); // fd 10: 成功
    connect_fd(11, 0);
    text_ret = -1;
    tick(1); // fd 11 だけ新規 -> 失敗。fd 10 は送信中
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    TEST_ASSERT_EQUAL(1, close_cnt[11]);
    TEST_ASSERT_TRUE(core.clients.clients[0].is_sending);
}

void test_failed_text_is_not_counted_as_sent_and_not_retried(void) {
    start(0);
    connect_fd(10, 0);
    text_ret = -1;
    tick(0);
    TEST_ASSERT_FALSE(core.status_sent[0]);
    TEST_ASSERT_EQUAL(0, text_count[10]);
}

/* ================= on_send_done の区別 ================= */

void test_send_done_after_text_does_not_release_a_frame(void) {
    start(0);
    hub_frame_on = true;
    connect_fd(10, 0);
    tick(0);
    done(10, true);
    TEST_ASSERT_EQUAL(0, rel_count);
    TEST_ASSERT_FALSE(core.sending_text[0]);
    TEST_ASSERT_EQUAL(0, (int)core.text_len[0]);
    tick(1); // フレーム
    TEST_ASSERT_FALSE(core.sending_text[0]);
    TEST_ASSERT_NOT_NULL(core.current_frames[0]);
    done(10, true);
    TEST_ASSERT_EQUAL(1, rel_count); // フレームの完了では、解放する
    TEST_ASSERT_NULL(core.current_frames[0]);
    expect_no_violations();
}

void test_send_done_failure_after_text_closes_without_release(void) {
    start(0);
    hub_frame_on = true;
    connect_fd(10, 0);
    tick(0);
    done(10, false);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_EQUAL(0, rel_count);
    TEST_ASSERT_FALSE(core.clients.clients[0].is_sending);
    tick(1);
    TEST_ASSERT_EQUAL(0, frame_begin_count);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    expect_no_violations();
}

void test_send_done_failure_after_frame_still_closes(void) {
    start(0);
    hub_frame_on = true;
    connect_fd(10, 0);
    tick(0);
    done(10, true);
    tick(1);
    done(10, false);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_EQUAL(1, rel_count);
}

void test_close_during_text_send_then_done_unregisters_once_without_release(void) {
    start(0);
    hub_frame_on = true;
    connect_fd(10, 0);
    tick(0);
    ws_video_core_on_close(&core, 10); // CLOSING (送信中)
    TEST_ASSERT_EQUAL(0, unreg_count);
    done(10, true);
    TEST_ASSERT_EQUAL(1, unreg_count);
    TEST_ASSERT_EQUAL(0, rel_count);
    TEST_ASSERT_EQUAL(-1, ws_clients_find(&core.clients, 10));
    expect_no_violations();
}

void test_text_flag_does_not_leak_to_next_connection_in_same_slot(void) {
    start(0);
    hub_frame_on = true;
    connect_fd(10, 0);
    tick(0);
    ws_video_core_on_close(&core, 10);
    done(10, true);
    int slot = connect_fd(11, 5);
    TEST_ASSERT_FALSE(core.sending_text[slot]);
    TEST_ASSERT_EQUAL(0, (int)core.text_len[slot]);
    tick(5);
    done(11, true);
    tick(6); // フレーム。sending_text が残っていれば、期限の size がテキストの長さになる
    TEST_ASSERT_EQUAL(1, frame_begin_count);
    TEST_ASSERT_FALSE(core.sending_text[slot]);
}

/* ================= 最短間隔 ================= */

void test_min_interval_boundary_plus_minus_one(void) {
    start(200);
    connect_fd(10, 1000);
    tick(1000);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    tick(1001);
    tick(1100);
    tick(1199); // 199 ms
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    tick(1200); // ちょうど 200 ms
    TEST_ASSERT_EQUAL(2, text_begin_calls);
    TEST_ASSERT_EQUAL_STRING(J_720, text_log[1]);
    done(10, true);
    // 次の間隔は、1200 から数える
    status_set(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    tick(1399);
    TEST_ASSERT_EQUAL(2, text_begin_calls);
    tick(1400);
    TEST_ASSERT_EQUAL(3, text_begin_calls);
    TEST_ASSERT_EQUAL_STRING(J_NOSIG, text_log[2]);
    expect_no_violations();
}

void test_min_interval_one_ms(void) {
    start(1);
    connect_fd(10, 100);
    tick(100);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    tick(100); // 0 ms
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    tick(101); // 1 ms
    TEST_ASSERT_EQUAL(2, text_begin_calls);
}

void test_changes_inside_the_interval_send_only_the_latest_once(void) {
    start(200);
    connect_fd(10, 1000);
    tick(1000);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_LOCKED, 640, 480);
    tick(1050);
    status_set(KVM_VIDEO_SYNC_LOCKED, 800, 600);
    tick(1100);
    status_set(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    tick(1150);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    tick(1200);
    TEST_ASSERT_EQUAL(2, text_begin_calls);
    TEST_ASSERT_EQUAL_STRING(J_NOSIG, text_log[1]); // 最新の状態だけ。途中の状態は送らない
    done(10, true);
    for (uint64_t t = 1201; t < 2000; t += 50) tick(t);
    TEST_ASSERT_EQUAL(2, text_begin_calls);         // その後は、1 回だけ
}

void test_change_and_revert_inside_the_interval_sends_nothing(void) {
    start(200);
    connect_fd(10, 1000);
    tick(1000);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_LOCKED, 640, 480);
    tick(1050);
    status_set(KVM_VIDEO_SYNC_UNKNOWN, 0, 0); // 最後に送ったものに戻る
    for (uint64_t t = 1100; t < 1600; t += 50) tick(t);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
}

void test_change_after_a_long_quiet_period_is_sent_immediately(void) {
    start(200);
    connect_fd(10, 0);
    tick(0);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    tick(5000); // 最後に送ってから 5000 ms 経っている
    TEST_ASSERT_EQUAL(2, text_begin_calls);
}

void test_interval_counts_from_begin_not_from_done(void) {
    start(200);
    connect_fd(10, 1000);
    tick(1000);                  // 送り始めた (1000)
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    tick(1150);                  // まだ送信中
    done(10, true);              // 完了が遅い
    tick(1199);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    tick(1200);                  // 送り始めた時刻から 200 ms
    TEST_ASSERT_EQUAL(2, text_begin_calls);
}

void test_first_status_after_connect_ignores_the_interval(void) {
    start(1000000);
    connect_fd(10, 7);
    tick(7);
    TEST_ASSERT_EQUAL(1, text_count[10]);
    done(10, true);
    connect_fd(11, 8); // 別のクライアントの最初の 1 回は、10 の間隔に関わらず送る
    tick(8);
    TEST_ASSERT_EQUAL(1, text_count[11]);
    done(11, true);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    tick(9);
    tick(500000);
    TEST_ASSERT_EQUAL(1, text_count[10]);
    TEST_ASSERT_EQUAL(1, text_count[11]);
    tick(1000006);              // 10 は 999999 ms
    TEST_ASSERT_EQUAL(1, text_count[10]);
    tick(1000007);              // 10 は 1000000 ms
    TEST_ASSERT_EQUAL(2, text_count[10]);
    TEST_ASSERT_EQUAL(1, text_count[11]);
    tick(1000008);              // 11 は 1000000 ms
    TEST_ASSERT_EQUAL(2, text_count[11]);
}

void test_reconnect_resets_interval_and_sent_flag(void) {
    start(1000000);
    connect_fd(10, 0);
    tick(0);
    done(10, true);
    ws_video_core_on_close(&core, 10);
    connect_fd(11, 1); // 同じスロット。同じ状態でも、接続直後は必ず 1 回
    tick(1);
    TEST_ASSERT_EQUAL(1, text_count[11]);
    TEST_ASSERT_EQUAL_STRING(J_UNKNOWN, text_log[text_log_n - 1]);
}

void test_interval_zero_sends_every_change_immediately(void) {
    start(0);
    connect_fd(10, 0);
    tick(0);
    done(10, true);
    for (int i = 1; i <= 5; i++) {
        status_set(KVM_VIDEO_SYNC_LOCKED, (uint32_t)(100 + i), 50);
        tick(0); // 時刻が進まなくても (同じ時刻でも) 送る
        TEST_ASSERT_EQUAL(1 + i, text_begin_calls);
        done(10, true);
    }
}

/* ================= 時刻の巻き戻り ================= */

void test_clock_rollback_clamps_last_sent_time(void) {
    start(200);
    connect_fd(10, 5000);
    tick(5000);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    tick(1000);                              // 巻き戻り: 最後に送った時刻を 1000 に合わせる
    TEST_ASSERT_EQUAL_UINT64(1000, core.status_last_ms[0]);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    tick(1199);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    tick(1200);
    TEST_ASSERT_EQUAL(2, text_begin_calls);
}

void test_clock_rollback_without_change_then_change(void) {
    start(200);
    connect_fd(10, 5000);
    tick(5000);
    done(10, true);
    tick(1000);                              // 変化なしの tick でも、時刻は合わせる
    TEST_ASSERT_EQUAL_UINT64(1000, core.status_last_ms[0]);
    status_set(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    tick(1199);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    tick(1200);
    TEST_ASSERT_EQUAL(2, text_begin_calls);
}

void test_clock_rollback_to_zero_does_not_underflow(void) {
    start(200);
    connect_fd(10, 5000);
    tick(5000);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    tick(0);                                 // 巻き戻して 0: 差が巨大な値にならない
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    tick(199);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    tick(200);
    TEST_ASSERT_EQUAL(2, text_begin_calls);
}

/* ================= 期限 (テキスト) ================= */

void test_text_deadline_uses_text_length(void) {
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.frame_deadline_base_ms = 100;
    cfg.frame_min_rate_bytes_per_s = 100; // 100 B/s: 1 バイトにつき 10 ms
    start_cfg(&cfg);
    hub_frame_size = 4;
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    connect_fd(10, 0);
    tick(0);
    size_t len = text_log_len[0];
    TEST_ASSERT_TRUE(len > 40);
    uint64_t dl = ws_video_frame_deadline_ms(len, 100, 100);
    TEST_ASSERT_EQUAL_UINT64(100 + len * 10, dl);
    ws_video_core_on_send_begin(&core, 10, 0);
    tick(dl - 1);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(dl);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_text_deadline_is_not_the_base_only_nor_the_frame_size(void) {
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.frame_deadline_base_ms = 100;
    cfg.frame_min_rate_bytes_per_s = 100;
    start_cfg(&cfg);
    hub_frame_size = 100000;               // フレームの大きさは別 (期限 1000100 ms)
    hub_frame_on = true;
    connect_fd(10, 0);
    tick(0);
    size_t len = text_log_len[0];
    ws_video_core_on_send_begin(&core, 10, 0);
    tick(100 + len * 10 - 1);              // base だけ (size = 0) の期限 100 ms は、とうに過ぎている
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(100 + len * 10);                  // フレームの大きさ (100000) ではなく、テキストの長さで切れる
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_text_deadline_closes_only_that_client(void) {
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.frame_deadline_base_ms = 100;
    cfg.frame_min_rate_bytes_per_s = 100;
    start_cfg(&cfg);
    connect_fd(10, 0);
    connect_fd(11, 0);
    tick(0);
    size_t len = text_log_len[0];
    ws_video_core_on_send_begin(&core, 10, 0);
    ws_video_core_on_send_begin(&core, 11, 0);
    done(11, true);                        // 11 は、期限の前に送り終える
    tick(100 + len * 10);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_EQUAL(0, close_cnt[11]);
}

void test_text_deadline_uses_each_clients_own_text_length(void) {
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.frame_deadline_base_ms = 100;
    cfg.frame_min_rate_bytes_per_s = 100;
    start_cfg(&cfg);
    connect_fd(10, 0);
    tick(0);                               // スロット 0: unknown (短いテキスト)
    size_t len10 = text_log_len[0];
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    connect_fd(11, 0);
    tick(0);                               // スロット 1: locked (長いテキスト)
    size_t len11 = text_log_len[1];
    TEST_ASSERT_TRUE(len11 > len10);
    ws_video_core_on_send_begin(&core, 10, 0);
    ws_video_core_on_send_begin(&core, 11, 0);
    uint64_t dl10 = 100 + len10 * 10;
    uint64_t dl11 = 100 + len11 * 10;
    tick(dl10 - 1);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    TEST_ASSERT_EQUAL(0, close_cnt[11]);
    tick(dl10);                            // 10 の期限だけが来た
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_EQUAL(0, close_cnt[11]);
    tick(dl11 - 1);
    TEST_ASSERT_EQUAL(0, close_cnt[11]);
    tick(dl11);
    TEST_ASSERT_EQUAL(1, close_cnt[11]);
}

void test_text_deadline_not_applied_before_send_begin_but_queue_timeout_is(void) {
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.frame_deadline_base_ms = 100;
    cfg.frame_min_rate_bytes_per_s = 100;
    cfg.queue_timeout_ms = 500;
    start_cfg(&cfg);
    connect_fd(10, 1000);
    tick(1000);                            // キューに入れた (on_send_begin はまだ)。時刻は 0 でない
    tick(1499);                            // 期限は、まだ数えない
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(1500);                            // キュー待ちの上限 (キューに入れた時刻から 500 ms)
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_text_send_timeout_counts_from_send_begin(void) {
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.send_timeout_ms = 300;
    cfg.queue_timeout_ms = 10000;
    start_cfg(&cfg);
    connect_fd(10, 0);
    tick(0);
    tick(5000);                            // キュー待ち。送信の停止には数えない
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    ws_video_core_on_send_begin(&core, 10, 5000);
    tick(5299);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(5300);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_text_queue_timeout_legacy_mode_counts_from_queued(void) {
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.send_timeout_ms = 300;
    cfg.queue_timeout_ms = 0; // 互換: キューに入れた時刻から数える
    start_cfg(&cfg);
    connect_fd(10, 1000);
    tick(1000);
    tick(1299);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(1300);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_deadline_for_next_frame_uses_the_frame_size_after_text(void) {
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.frame_deadline_base_ms = 200;
    cfg.frame_min_rate_bytes_per_s = 1000000;
    start_cfg(&cfg);
    hub_frame_on = true;
    hub_frame_size = 114688; // 期限 314 ms
    connect_fd(10, 0);
    tick(0);
    ws_video_core_on_send_begin(&core, 10, 0);
    done(10, true);
    tick(1);                 // フレーム
    TEST_ASSERT_EQUAL(K_FRAME, kind[10]);
    ws_video_core_on_send_begin(&core, 10, 1);
    tick(1 + 313);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(1 + 314);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_text_deadline_is_per_message_not_cumulative(void) {
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.frame_deadline_base_ms = 100;
    cfg.frame_min_rate_bytes_per_s = 100;
    start_cfg(&cfg);
    connect_fd(10, 0);
    tick(0);
    size_t len = text_log_len[0];
    uint64_t dl = 100 + len * 10;
    ws_video_core_on_send_begin(&core, 10, 0);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    tick(dl - 5);                          // 2 つ目のテキスト (前のテキストの期限は、もう関係ない)
    ws_video_core_on_send_begin(&core, 10, dl - 5);
    size_t len2 = text_log_len[1];
    uint64_t dl2 = 100 + len2 * 10;
    tick(dl - 5 + dl2 - 1);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    tick(dl - 5 + dl2);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

/* ================= status_get == NULL: 従来どおり ================= */

void test_without_status_get_behaves_as_before(void) {
    ws_video_core_config_t cfg = base_cfg(200);
    cfg.status_get = NULL; // io_begin_send_text と status_min_interval_ms があっても、通知しない
    start_cfg(&cfg);
    hub_frame_on = true;
    connect_fd(10, 0);
    tick(0);                               // 最初の tick で、フレームを送る
    TEST_ASSERT_EQUAL(1, frame_begin_count);
    TEST_ASSERT_EQUAL(0, text_begin_calls);
    TEST_ASSERT_EQUAL(0, status_get_count);
    TEST_ASSERT_EQUAL(K_FRAME, kind[10]);
    done(10, true);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    g_seq++;
    tick(1);
    TEST_ASSERT_EQUAL(2, frame_begin_count);
    TEST_ASSERT_EQUAL(0, text_begin_calls);
    TEST_ASSERT_FALSE(core.sending_text[0]);
    TEST_ASSERT_FALSE(core.status_sent[0]);
}

void test_without_status_get_and_without_text_callback(void) {
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.status_get = NULL;
    cfg.io_begin_send_text = NULL;
    start_cfg(&cfg);
    hub_frame_on = true;
    connect_fd(10, 0);
    tick(0);
    TEST_ASSERT_EQUAL(1, frame_begin_count);
    done(10, true);
    TEST_ASSERT_EQUAL(1, rel_count);
}

/* ================= 複数のクライアント ================= */

void test_multiple_clients_are_independent(void) {
    start(0);
    connect_fd(10, 0);
    connect_fd(11, 0);
    connect_fd(12, 0);
    tick(0);
    TEST_ASSERT_EQUAL(1, text_count[10]);
    TEST_ASSERT_EQUAL(1, text_count[11]);
    TEST_ASSERT_EQUAL(1, text_count[12]);
    done(10, true);
    done(12, true); // 11 は送信中のまま
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    tick(10);
    TEST_ASSERT_EQUAL(2, text_count[10]);
    TEST_ASSERT_EQUAL(1, text_count[11]); // 送信中
    TEST_ASSERT_EQUAL(2, text_count[12]);
    done(11, true);
    tick(11);
    TEST_ASSERT_EQUAL(2, text_count[11]);
    done(10, true);
    done(12, true);
    done(11, true);
    for (uint64_t t = 12; t < 30; t++) tick(t);
    TEST_ASSERT_EQUAL(2, text_count[10]);
    TEST_ASSERT_EQUAL(2, text_count[11]);
    TEST_ASSERT_EQUAL(2, text_count[12]);
    expect_no_violations();
}

void test_multiple_clients_have_independent_intervals(void) {
    start(200);
    connect_fd(10, 1000);
    tick(1000);
    done(10, true);
    connect_fd(11, 1100);                 // 11 は、あとで接続して、あとで最初の 1 回を送る
    tick(1100);
    done(11, true);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    tick(1199);
    TEST_ASSERT_EQUAL(1, text_count[10]);
    TEST_ASSERT_EQUAL(1, text_count[11]);
    tick(1200);                           // 10 だけ、200 ms 経った
    TEST_ASSERT_EQUAL(2, text_count[10]);
    TEST_ASSERT_EQUAL(1, text_count[11]);
    tick(1299);
    TEST_ASSERT_EQUAL(1, text_count[11]);
    tick(1300);
    TEST_ASSERT_EQUAL(2, text_count[11]);
    TEST_ASSERT_EQUAL_STRING(J_720, text_log[text_log_n - 1]);
}

void test_all_clients_in_one_tick_see_the_same_status_snapshot(void) {
    start(0);
    status_flip = true; // status_get を呼ぶたびに値が変わる。tick の中で何回呼んでも、全員に同じ値を送ること
    connect_fd(10, 0);
    connect_fd(11, 0);
    connect_fd(12, 0);
    tick(0);
    TEST_ASSERT_EQUAL(3, text_log_n);
    TEST_ASSERT_EQUAL_STRING(text_log[0], text_log[1]);
    TEST_ASSERT_EQUAL_STRING(text_log[0], text_log[2]);
    TEST_ASSERT_TRUE(status_get_count >= 1);
}

void test_each_client_gets_text_on_its_own_connection(void) {
    start(0);
    connect_fd(10, 0);
    connect_fd(11, 0);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    tick(0);
    TEST_ASSERT_EQUAL(2, text_log_n);
    TEST_ASSERT_EQUAL(10, text_log_fd[0]);
    TEST_ASSERT_EQUAL(11, text_log_fd[1]);
    TEST_ASSERT_TRUE(text_log_ptr[0] != text_log_ptr[1]); // バッファは、クライアントごと
}

void test_text_buffer_stays_valid_and_unchanged_until_send_done(void) {
    start(0);
    connect_fd(10, 0);
    connect_fd(11, 0);
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    tick(0);
    const uint8_t *p10 = text_log_ptr[0];
    const uint8_t *p11 = text_log_ptr[1];
    // 状態が変わり、ほかのクライアントの送信が進んでも、送信中のバッファは変わらない
    status_set(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    done(11, true);
    tick(1);                              // 11 に新しいテキスト
    TEST_ASSERT_EQUAL_STRING(J_720, (const char *)p10);
    TEST_ASSERT_EQUAL_STRING(J_NOSIG, (const char *)text_log_ptr[2]);
    (void)p11;
    done(10, true);
}

void test_client_with_close_requested_gets_no_text(void) {
    start(0);
    connect_fd(10, 0);
    ws_video_core_on_client_data(&core, 10); // 受信した = 閉じる
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    tick(0);
    tick(1);
    TEST_ASSERT_EQUAL(0, text_begin_calls);
    // 別のクライアントは、影響を受けない
    connect_fd(11, 2);
    tick(2);
    TEST_ASSERT_EQUAL(1, text_count[11]);
    TEST_ASSERT_EQUAL(0, text_count[10]);
}

void test_client_closing_after_transport_close_gets_no_text(void) {
    start(0);
    connect_fd(10, 0);
    tick(0);
    ws_video_core_on_close(&core, 10); // 送信中 -> CLOSING
    status_set(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    tick(1);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
    done(10, true);                    // FREE
    tick(2);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
}

void test_session_invalid_client_gets_no_text(void) {
    // 無効なセッション: tick の最初で閉じる。テキストは送らない
    ws_video_core_config_t cfg = base_cfg(0);
    cfg.session_check_interval_ms = 10;
    start_cfg(&cfg);
    connect_fd(10, 0);
    connect_fd(11, 0);
    val_ret = false;
    tick(100);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_EQUAL(1, close_cnt[11]);
    TEST_ASSERT_EQUAL(0, text_begin_calls);
    // 有効なセッションなら、間隔の検査が通ったうえで、テキストを送る
    start_cfg(&cfg);
    connect_fd(10, 0);
    tick(100);
    TEST_ASSERT_EQUAL(0, close_cnt[10]);
    TEST_ASSERT_EQUAL(1, text_begin_calls);
}

/* ================= ファズ ================= */

static uint32_t rng_state;
static uint32_t rnd(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}

typedef struct {
    bool connected;
    bool closed;     // 何らかの理由で、close 要求済み / CLOSING
    bool inflight;   // 送信中 (フレームまたはテキスト)
    bool sent;       // モデルの status_sent
    video_status_t last;
    uint64_t last_ms;
} fm_t;

static fm_t fm[MAXFD];
static uint64_t g_now;
static long fz_texts, fz_blocked, fz_text_fail, fz_frames, fz_rollbacks;

static const video_status_t fz_palette[] = {
    {KVM_VIDEO_SYNC_UNKNOWN, 0, 0},
    {KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0},
    {KVM_VIDEO_SYNC_LOCKED, 1280, 720},
    {KVM_VIDEO_SYNC_LOCKED, 1920, 1080},
    {KVM_VIDEO_SYNC_LOCKED, 640, 480},
    {KVM_VIDEO_SYNC_LOCKED, 65535, 65535},
    {(kvm_video_sync_state_t)99, 7, 7},        /* 範囲外 (生の値で比べる) */
    {KVM_VIDEO_SYNC_LOCKED, 0, 0},              /* 不整合 */
};
#define FZ_PAL (sizeof(fz_palette) / sizeof(fz_palette[0]))

static bool st_same(const video_status_t *a, const video_status_t *b) {
    return a->state == b->state && a->width == b->width && a->height == b->height;
}

static int fz_one(uint32_t seed, int steps) {
    rng_state = seed ? seed : 1;
    static const uint64_t intervals[] = {0, 50, 200, 1000};
    uint64_t interval = intervals[rnd() % 4];
    bool timers = (rnd() % 2) == 0;
    bool rollback = (rnd() % 2) == 0;

    ws_video_core_config_t cfg = base_cfg(interval);
    cfg.max_clients = 5;
    if (timers) {
        cfg.send_timeout_ms = 300;
        cfg.queue_timeout_ms = 500;
        cfg.frame_deadline_base_ms = 200;
        cfg.frame_min_rate_bytes_per_s = 40000;
    }
    ws_video_core_destroy(&core);
    reset_all();
    memset(fm, 0, sizeof(fm));
    if (ws_video_core_init(&core, &cfg) != 0) return -1;
    hub_frame_on = true;
    hub_frame_size = 100;
    g_now = 1000;
    g_status = fz_palette[0];

    for (int step = 0; step < steps; step++) {
        int fd = 10 + (int)(rnd() % 5);
        fm_t *m = &fm[fd];
        uint32_t r = rnd() % 100;
        if (r < 10) { // connect
            if (!m->connected) {
                memset(m, 0, sizeof(*m));
                uint8_t tok[32] = {0};
                close_cnt[fd] = 0;
                dead[fd] = false;
                kind[fd] = K_NONE;
                text_count[fd] = 0;
                if (ws_video_core_on_connect(&core, (ws_video_conn_t)(uintptr_t)fd, fd, tok, g_now) >= 0) {
                    m->connected = true;
                }
            }
        } else if (r < 15) { // 切断 (トランスポート)
            if (m->connected) {
                int slot = ws_clients_find(&core.clients, fd);
                ws_video_core_on_close(&core, fd);
                dead[fd] = true;
                m->closed = true;
                if (slot >= 0 && core.clients.clients[slot].state == WS_CLIENT_FREE) m->connected = false;
            }
        } else if (r < 25) { // on_send_begin
            if (m->connected && m->inflight) ws_video_core_on_send_begin(&core, fd, g_now);
        } else if (r < 42) { // on_send_done
            if (m->connected && m->inflight) {
                bool ok = (rnd() % 5) != 0;
                done(fd, ok);
                m->inflight = false;
                if (!ok) m->closed = true;
                if (ws_clients_find(&core.clients, fd) < 0) m->connected = false;
            }
        } else if (r < 44) { // クライアントからの受信
            if (m->connected) {
                ws_video_core_on_client_data(&core, fd);
                dead[fd] = true;
                m->closed = true;
            }
        } else if (r < 58) { // 状態の変化
            g_status = fz_palette[rnd() % FZ_PAL];
        } else if (r < 63) { // 新しいフレーム
            g_seq++;
        } else { // tick
            if (rollback && (rnd() % 12) == 0) {
                uint64_t back = rnd() % 1500;
                g_now = (back < g_now) ? g_now - back : 0;
                fz_rollbacks++;
            } else {
                g_now += rnd() % 300;
            }
            text_ret = ((rnd() % 9) == 0) ? -1 : 0;
            begin_ret = ((rnd() % 25) == 0) ? -1 : 0;

            bool alive_idle[MAXFD] = {0};
            bool expect_text[MAXFD] = {0};
            for (int f = 10; f < 15; f++) {
                fm_t *q = &fm[f];
                if (!q->connected || q->closed || q->inflight) continue;
                alive_idle[f] = true;
                if (!q->sent) {
                    expect_text[f] = true;
                } else {
                    if (g_now < q->last_ms) q->last_ms = g_now; // 時刻の巻き戻り
                    if (!st_same(&g_status, &q->last)) {
                        if (interval == 0 || g_now - q->last_ms >= interval) expect_text[f] = true;
                        else fz_blocked++;
                    }
                }
            }
            tick(g_now);
            for (int f = 10; f < 15; f++) {
                fm_t *q = &fm[f];
                if (alive_idle[f]) {
                    if ((texts_tick[f] > 0) != expect_text[f]) {
                        printf("seed=%u step=%d fd=%d text=%d expect=%d now=%llu last=%llu\n", (unsigned)seed, step, f,
                               texts_tick[f], expect_text[f], (unsigned long long)g_now, (unsigned long long)q->last_ms);
                        return -2;
                    }
                    if (expect_text[f]) {
                        if (frames_tick[f] != 0) {
                            printf("seed=%u step=%d fd=%d frame in the same tick as text\n", (unsigned)seed, step, f);
                            return -3;
                        }
                        if (text_ret == 0) {
                            q->sent = true;
                            q->last = g_status;
                            q->last_ms = g_now;
                            fz_texts++;
                        } else {
                            fz_text_fail++;
                        }
                    } else if (frames_tick[f] > 0 && begin_ret == 0) {
                        fz_frames++;
                    }
                } else if (texts_tick[f] != 0 || frames_tick[f] != 0) {
                    printf("seed=%u step=%d fd=%d sent while not idle/alive\n", (unsigned)seed, step, f);
                    return -4;
                }
                if (kind[f] != K_NONE) q->inflight = true;
                if (close_cnt[f] > 0) q->closed = true;
            }
        }
        for (int f = 10; f < 15; f++) {
            if (close_cnt[f] > 1) { printf("seed=%u step=%d fd=%d double close\n", (unsigned)seed, step, f); return -5; }
        }
        if (viol_double || viol_dead || viol_release_text || double_hold || bad_release || bad_unregister) {
            printf("seed=%u step=%d violations dbl=%d dead=%d reltext=%d hold=%d rel=%d unreg=%d\n", (unsigned)seed, step,
                   viol_double, viol_dead, viol_release_text, double_hold, bad_release, bad_unregister);
            return -6;
        }
    }

    /* 収束: 最後に、どの値とも違う状態にして、生きている全員が、最新の状態を 1 回だけ受け取ること */
    text_ret = 0;
    begin_ret = 0;
    for (int f = 10; f < 15; f++) {
        if (fm[f].connected && fm[f].inflight) {
            done(f, true);
            fm[f].inflight = false;
        }
    }
    const video_status_t final_st = {KVM_VIDEO_SYNC_LOCKED, 4321, 1234};
    const char *final_json = "{\"t\":\"video\",\"state\":\"locked\",\"w\":4321,\"h\":1234}";
    g_status = final_st;
    int since_final[MAXFD];
    memset(since_final, 0, sizeof(since_final));
    char last_json[MAXFD][160];
    memset(last_json, 0, sizeof(last_json));
    int log_from = text_log_n; /* text_log は先頭 MAXLOG 件だけなので、最終の文字列は別に取る */
    (void)log_from;
    for (int round = 0; round < 8; round++) {
        g_now += 5000; // どの間隔よりも長い
        tick(g_now);
        for (int f = 10; f < 15; f++) {
            if (texts_tick[f] > 0) {
                since_final[f] += texts_tick[f];
                /* 直近の本文: tick 中に、モックが最後に書いた内容は、ログに残っていないので、ポインタから読む */
            }
        }
        for (int f = 10; f < 15; f++) {
            fm_t *q = &fm[f];
            if (!q->connected) continue;
            int slot = ws_clients_find(&core.clients, f);
            if (slot >= 0 && core.sending_text[slot] && core.clients.clients[slot].is_sending) {
                memcpy(last_json[f], core.text_buf[slot], core.text_len[slot]);
                last_json[f][core.text_len[slot]] = '\0';
            }
            if (kind[f] != K_NONE) {
                done(f, true);
                q->inflight = false;
            }
        }
    }
    for (int f = 10; f < 15; f++) {
        fm_t *q = &fm[f];
        if (!q->connected || q->closed) continue;
        if (since_final[f] != 1) {
            printf("seed=%u fd=%d final texts=%d (expected exactly 1)\n", (unsigned)seed, f, since_final[f]);
            return -7;
        }
        if (strcmp(last_json[f], final_json) != 0) {
            printf("seed=%u fd=%d final json=%s\n", (unsigned)seed, f, last_json[f]);
            return -8;
        }
    }

    /* 後始末: フレームの保持が残らない */
    for (int f = 10; f < 15; f++) {
        if (fm[f].connected) {
            int slot = ws_clients_find(&core.clients, f);
            if (slot >= 0 && core.clients.clients[slot].is_sending) done(f, true);
            ws_video_core_on_close(&core, f);
            slot = ws_clients_find(&core.clients, f);
            if (slot >= 0 && core.clients.clients[slot].is_sending) done(f, true);
        }
    }
    for (int i = 0; i < MAXID; i++) {
        if (hub_held[i] != 0) { printf("seed=%u leftover hold id=%d\n", (unsigned)seed, i); return -9; }
        if (hub_used[i]) { printf("seed=%u leftover registration id=%d\n", (unsigned)seed, i); return -10; }
    }
    return 0;
}

void test_fuzz_status_notify_matches_model(void) {
    fz_texts = fz_blocked = fz_text_fail = fz_frames = fz_rollbacks = 0;
    for (uint32_t seed = 1; seed <= 300; seed++) {
        int r = fz_one(seed * 2654435761u + 11u, 3000);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, r, "model mismatch or invariant violation (see printed seed)");
    }
    printf("fuzz: texts=%ld blocked-by-interval=%ld text-failures=%ld frames=%ld rollbacks=%ld\n", fz_texts, fz_blocked,
           fz_text_fail, fz_frames, fz_rollbacks);
    /* 空振りでないこと (固定シードなので、毎回同じ値) */
    TEST_ASSERT_TRUE_MESSAGE(fz_texts > 20000, "fuzz sent too few texts");
    TEST_ASSERT_TRUE_MESSAGE(fz_blocked > 2000, "fuzz never hit the interval");
    TEST_ASSERT_TRUE_MESSAGE(fz_text_fail > 500, "fuzz never failed a text");
    TEST_ASSERT_TRUE_MESSAGE(fz_frames > 5000, "fuzz sent too few frames");
    TEST_ASSERT_TRUE_MESSAGE(fz_rollbacks > 1000, "fuzz never rolled the clock back");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_status_get_requires_io_begin_send_text);
    RUN_TEST(test_init_without_status_get_does_not_need_text_callback);

    RUN_TEST(test_first_tick_after_connect_sends_status_once);
    RUN_TEST(test_first_status_is_the_current_status_not_a_fixed_unknown);
    RUN_TEST(test_no_text_before_first_tick_and_not_for_unconnected);

    RUN_TEST(test_same_status_is_not_resent);
    RUN_TEST(test_state_change_sends_once);
    RUN_TEST(test_resolution_change_sends_once);
    RUN_TEST(test_change_and_change_back_before_tick_sends_nothing);
    RUN_TEST(test_status_get_that_writes_nothing_is_unknown);
    RUN_TEST(test_out_of_range_status_does_not_crash_and_is_unknown_json);

    RUN_TEST(test_no_text_while_text_in_flight_then_latest_after_done);
    RUN_TEST(test_no_text_while_frame_in_flight_and_text_first_after);
    RUN_TEST(test_new_client_gets_text_then_frame_in_order);
    RUN_TEST(test_frames_continue_when_status_unchanged);

    RUN_TEST(test_text_send_failure_closes_the_client);
    RUN_TEST(test_text_send_failure_with_positive_return_also_closes);
    RUN_TEST(test_text_failure_does_not_affect_other_clients);
    RUN_TEST(test_failed_text_is_not_counted_as_sent_and_not_retried);

    RUN_TEST(test_send_done_after_text_does_not_release_a_frame);
    RUN_TEST(test_send_done_failure_after_text_closes_without_release);
    RUN_TEST(test_send_done_failure_after_frame_still_closes);
    RUN_TEST(test_close_during_text_send_then_done_unregisters_once_without_release);
    RUN_TEST(test_text_flag_does_not_leak_to_next_connection_in_same_slot);

    RUN_TEST(test_min_interval_boundary_plus_minus_one);
    RUN_TEST(test_min_interval_one_ms);
    RUN_TEST(test_changes_inside_the_interval_send_only_the_latest_once);
    RUN_TEST(test_change_and_revert_inside_the_interval_sends_nothing);
    RUN_TEST(test_change_after_a_long_quiet_period_is_sent_immediately);
    RUN_TEST(test_interval_counts_from_begin_not_from_done);
    RUN_TEST(test_first_status_after_connect_ignores_the_interval);
    RUN_TEST(test_reconnect_resets_interval_and_sent_flag);
    RUN_TEST(test_interval_zero_sends_every_change_immediately);

    RUN_TEST(test_clock_rollback_clamps_last_sent_time);
    RUN_TEST(test_clock_rollback_without_change_then_change);
    RUN_TEST(test_clock_rollback_to_zero_does_not_underflow);

    RUN_TEST(test_text_deadline_uses_text_length);
    RUN_TEST(test_text_deadline_is_not_the_base_only_nor_the_frame_size);
    RUN_TEST(test_text_deadline_closes_only_that_client);
    RUN_TEST(test_text_deadline_uses_each_clients_own_text_length);
    RUN_TEST(test_text_deadline_not_applied_before_send_begin_but_queue_timeout_is);
    RUN_TEST(test_text_send_timeout_counts_from_send_begin);
    RUN_TEST(test_text_queue_timeout_legacy_mode_counts_from_queued);
    RUN_TEST(test_deadline_for_next_frame_uses_the_frame_size_after_text);
    RUN_TEST(test_text_deadline_is_per_message_not_cumulative);

    RUN_TEST(test_without_status_get_behaves_as_before);
    RUN_TEST(test_without_status_get_and_without_text_callback);

    RUN_TEST(test_multiple_clients_are_independent);
    RUN_TEST(test_multiple_clients_have_independent_intervals);
    RUN_TEST(test_all_clients_in_one_tick_see_the_same_status_snapshot);
    RUN_TEST(test_each_client_gets_text_on_its_own_connection);
    RUN_TEST(test_text_buffer_stays_valid_and_unchanged_until_send_done);
    RUN_TEST(test_client_with_close_requested_gets_no_text);
    RUN_TEST(test_client_closing_after_transport_close_gets_no_text);
    RUN_TEST(test_session_invalid_client_gets_no_text);

    RUN_TEST(test_fuzz_status_notify_matches_model);
    return UNITY_END();
}

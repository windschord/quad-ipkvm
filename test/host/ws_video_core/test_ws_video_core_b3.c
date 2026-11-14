/*
 * ws_video_core: B-3 (a) の挙動のテスト (docs/VIDEO_TRANSPORT.md 3 章)。
 *  - ws_video_frame_deadline_ms (純 C の期限の関数)
 *  - on_send_begin: キュー待ちと実際の送信の分離
 *  - queue_timeout_ms: キュー待ちの上限
 *  - 1 フレーム全体の期限 (フレームの大きさごと。そのクライアントだけを閉じる)
 *  - 時刻の巻き戻り、on_send_begin なしの on_send_done
 *  - 不変条件ファズ (固定シード。独立したモデルで、閉じるべきクライアントを判定する)
 * 既存の test_ws_video_core.c は、キュー待ちの分離を使わない呼び出し側 (queue_timeout_ms == 0) の互換を見ている。
 */
#include "unity.h"
#include "ws_video_core.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define MAXFD 16
#define MAXID 16

static ws_video_core_t core;

/* ---- モック ---- */
static int hub_next_size = 4;              // 次に register されるクライアントのフレームの大きさ
static size_t hub_size[MAXID];
static bool hub_used[MAXID];
static int hub_held[MAXID];                // 保持中のフレーム数 (0 または 1)
static video_frame_t hub_frames[MAXID];
static int64_t g_seq = 1;
static uint8_t dummy_data[8];
static int double_hold_violations, bad_release_violations, bad_unregister_violations;
static int unreg_count, rel_count, begin_count;
static int begin_ret = 0;
static int close_cnt[MAXFD];               // fd ごとの、io_request_close の回数 (接続のたびに 0 に戻す)
static int total_closes;

static int m_register(void *u) {
    (void)u;
    for (int i = 0; i < MAXID; i++) {
        if (!hub_used[i]) {
            hub_used[i] = true;
            hub_held[i] = 0;
            hub_size[i] = (size_t)hub_next_size;
            return i;
        }
    }
    return -1;
}
static void m_unregister(void *u, int id) {
    (void)u;
    if (id < 0 || id >= MAXID || !hub_used[id]) { bad_unregister_violations++; return; }
    hub_used[id] = false;
    hub_held[id] = 0;
    unreg_count++;
}
static const video_frame_t *m_get_latest(void *u, int id) {
    (void)u;
    if (hub_held[id] > 0) double_hold_violations++;
    hub_held[id]++;
    hub_frames[id].data = dummy_data;
    hub_frames[id].size = hub_size[id];
    hub_frames[id].seq = g_seq;
    return &hub_frames[id];
}
static void m_release(void *u, int id, const video_frame_t *f) {
    (void)u; (void)f;
    if (hub_held[id] <= 0) { bad_release_violations++; return; }
    hub_held[id]--;
    rel_count++;
}
static int m_begin_send(void *u, ws_video_conn_t c, const uint8_t *d, size_t l) {
    (void)u; (void)c; (void)d; (void)l;
    begin_count++;
    return begin_ret;
}
static void m_close(void *u, ws_video_conn_t c) {
    (void)u;
    uintptr_t fd = (uintptr_t)c;
    if (fd < MAXFD) close_cnt[fd]++;
    total_closes++;
}
static bool m_validate(void *u, const uint8_t t[32]) { (void)u; (void)t; return true; }

static void reset_all(void) {
    memset(hub_used, 0, sizeof(hub_used));
    memset(hub_held, 0, sizeof(hub_held));
    memset(close_cnt, 0, sizeof(close_cnt));
    hub_next_size = 4;
    g_seq = 1;
    double_hold_violations = bad_release_violations = bad_unregister_violations = 0;
    unreg_count = rel_count = begin_count = total_closes = 0;
    begin_ret = 0;
}

static void start(int max_clients, uint64_t send_to, uint64_t queue_to, uint32_t base, uint32_t rate) {
    ws_video_core_destroy(&core);
    reset_all();
    ws_video_core_config_t cfg = {
        .max_clients = max_clients,
        .session_check_interval_ms = 1000000000ull,
        .send_timeout_ms = send_to,
        .queue_timeout_ms = queue_to,
        .frame_deadline_base_ms = base,
        .frame_min_rate_bytes_per_s = rate,
        .hub_register = m_register,
        .hub_unregister = m_unregister,
        .hub_get_latest = m_get_latest,
        .hub_release = m_release,
        .io_begin_send = m_begin_send,
        .io_request_close = m_close,
        .io_validate_session = m_validate,
    };
    TEST_ASSERT_EQUAL(0, ws_video_core_init(&core, &cfg));
}

static int connect_fd(int fd, size_t frame_size, uint64_t now) {
    uint8_t tok[32] = {1};
    hub_next_size = (int)frame_size;
    close_cnt[fd] = 0;
    int slot = ws_video_core_on_connect(&core, (ws_video_conn_t)(uintptr_t)fd, fd, tok, now);
    TEST_ASSERT_TRUE(slot >= 0);
    return slot;
}

static void next_frame(void) { g_seq++; }

void setUp(void) { memset(&core, 0, sizeof(core)); reset_all(); }
void tearDown(void) { ws_video_core_destroy(&core); }

/* ---- ws_video_frame_deadline_ms ---- */

void test_deadline_rate_zero_means_no_deadline(void) {
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ws_video_frame_deadline_ms(0, 0, 0));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ws_video_frame_deadline_ms(123456, 200, 0));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ws_video_frame_deadline_ms(SIZE_MAX, UINT32_MAX, 0));
}

void test_deadline_values(void) {
    // size 0 -> base だけ
    TEST_ASSERT_EQUAL_UINT64(200, ws_video_frame_deadline_ms(0, 200, 1000000));
    TEST_ASSERT_EQUAL_UINT64(0, ws_video_frame_deadline_ms(0, 0, 1));
    // 契約の例: 112 KiB (114688 B) は約 315 ms (切り捨てで 314)、600000 B は 800 ms
    TEST_ASSERT_EQUAL_UINT64(314, ws_video_frame_deadline_ms(114688, 200, 1000000));
    TEST_ASSERT_EQUAL_UINT64(800, ws_video_frame_deadline_ms(600000, 200, 1000000));
    // 768 KiB (上限のフレーム)
    TEST_ASSERT_EQUAL_UINT64(200 + 786432u * 1000ull / 1000000u, ws_video_frame_deadline_ms(786432, 200, 1000000));
    // 切り捨て: 1 バイトを 3 B/s で送る = 333.33 ms
    TEST_ASSERT_EQUAL_UINT64(333, ws_video_frame_deadline_ms(1, 0, 3));
    TEST_ASSERT_EQUAL_UINT64(1333, ws_video_frame_deadline_ms(1, 1000, 3));
    // 2 バイト / 3 B/s = 666.67 ms
    TEST_ASSERT_EQUAL_UINT64(666, ws_video_frame_deadline_ms(2, 0, 3));
    // ちょうど 1 秒分
    TEST_ASSERT_EQUAL_UINT64(1000, ws_video_frame_deadline_ms(1000000, 0, 1000000));
    TEST_ASSERT_EQUAL_UINT64(999, ws_video_frame_deadline_ms(999999, 0, 1000000)); // 999.999 -> 999
    // 速度が 1 B/s: size * 1000 ms
    TEST_ASSERT_EQUAL_UINT64(5000, ws_video_frame_deadline_ms(5, 0, 1));
    // 速度の最大 (uint32)
    TEST_ASSERT_EQUAL_UINT64(0, ws_video_frame_deadline_ms(1, 0, UINT32_MAX));      // 1000 / 4294967295 = 0
    TEST_ASSERT_EQUAL_UINT64(1, ws_video_frame_deadline_ms(4294968, 0, UINT32_MAX)); // 4294968000 / 4294967295 = 1
    // base は uint32 の最大でも、そのまま足す
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, ws_video_frame_deadline_ms(0, UINT32_MAX, 1));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)UINT32_MAX + 1000, ws_video_frame_deadline_ms(1, UINT32_MAX, 1));
}

void test_deadline_monotonic_in_size_base_and_inverse_in_rate(void) {
    uint64_t prev = 0;
    for (size_t s = 0; s < 5000000; s += 7919) {
        uint64_t d = ws_video_frame_deadline_ms(s, 200, 1000000);
        TEST_ASSERT_TRUE(d >= prev);
        prev = d;
    }
    TEST_ASSERT_TRUE(ws_video_frame_deadline_ms(100000, 201, 1000000) == ws_video_frame_deadline_ms(100000, 200, 1000000) + 1);
    TEST_ASSERT_TRUE(ws_video_frame_deadline_ms(100000, 200, 500000) > ws_video_frame_deadline_ms(100000, 200, 1000000));
}

void test_deadline_saturates(void) {
#if SIZE_MAX >= UINT64_MAX
    // size * 1000 が 64 ビットを超える (rate = 1)
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ws_video_frame_deadline_ms((size_t)UINT64_MAX, 0, 1));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ws_video_frame_deadline_ms((size_t)UINT64_MAX, 1000, 1));
    // 結果がちょうど 64 ビットに収まる側と、超える側の境界 (rate = 1)
    const uint64_t q = UINT64_MAX / 1000u; // q * 1000 = UINT64_MAX - 615
    TEST_ASSERT_EQUAL_UINT64(q * 1000u, ws_video_frame_deadline_ms((size_t)q, 0, 1));
    TEST_ASSERT_EQUAL_UINT64(q * 1000u + 614u, ws_video_frame_deadline_ms((size_t)q, 614, 1));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ws_video_frame_deadline_ms((size_t)q, 615, 1));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ws_video_frame_deadline_ms((size_t)q, 616, 1));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ws_video_frame_deadline_ms((size_t)q, 1000000, 1));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ws_video_frame_deadline_ms((size_t)(q + 1), 0, 1)); // q+1 の積は溢れる
    // 厳密な値が 64 ビットに収まる大きな size (size * 1000 は溢れるが、結果は収まる) は、飽和させず正確に返す
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX / 1000u * 1000u + 0u, ws_video_frame_deadline_ms((size_t)(UINT64_MAX / 1000u), 0, 1) );
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ws_video_frame_deadline_ms((size_t)UINT64_MAX, 0, 1000)); // = UINT64_MAX ちょうど (切り捨て)
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ws_video_frame_deadline_ms((size_t)UINT64_MAX, 1, 1000)); // 超える -> 飽和
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX / 1000u * 1000u + 615u - 1u, ws_video_frame_deadline_ms((size_t)UINT64_MAX - 1u, 0, 1000));
    // 乗算の部分は収まるが、(余りの分 + q * 1000) の加算で溢れる
    const uint64_t qq = UINT64_MAX / 1000u;
    const size_t big = (size_t)(qq * 999u + 998u);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ws_video_frame_deadline_ms(big, 0, 999));
    // 余りが小さい側 (溢れない) は、正確な値
    const size_t ok = (size_t)(qq * 999u + 1u);
    TEST_ASSERT_EQUAL_UINT64(qq * 1000u + 1u, ws_video_frame_deadline_ms(ok, 0, 999));
#endif
    // 32 ビットの size_t でも、最大値で溢れない
    TEST_ASSERT_TRUE(ws_video_frame_deadline_ms((size_t)0xFFFFFFFFu, UINT32_MAX, 1) >= 4294967295000ull);
    TEST_ASSERT_EQUAL_UINT64(4294967295000ull + 5, ws_video_frame_deadline_ms((size_t)0xFFFFFFFFu, 5, 1));
}

/* ---- on_send_begin: キュー待ちと送信の分離 ---- */

void test_send_started_set_by_on_send_begin_not_by_begin_send(void) {
    start(2, 3000, 10000, 200, 1000000);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 777);
    TEST_ASSERT_EQUAL(1, begin_count);
    TEST_ASSERT_FALSE(core.send_begun[0]);
    TEST_ASSERT_EQUAL_UINT64(0, core.send_started_ms[0]);   // キューに入れた時点では設定しない
    TEST_ASSERT_EQUAL_UINT64(777, core.queued_ms[0]);       // 代わりにキューに入れた時刻を記録

    ws_video_core_on_send_begin(&core, 10, 1234);
    TEST_ASSERT_TRUE(core.send_begun[0]);
    TEST_ASSERT_EQUAL_UINT64(1234, core.send_started_ms[0]);

    ws_video_core_on_send_done(&core, 10, true);
    TEST_ASSERT_FALSE(core.send_begun[0]);
    TEST_ASSERT_EQUAL_UINT64(0, core.queued_ms[0]);
}

void test_queue_wait_not_counted_in_send_timeout(void) {
    start(2, 3000, 10000, 0, 0);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 0);                  // キューに入れる
    ws_video_core_tick(&core, 5000);               // キュー待ち 5000 ms: send_timeout (3000) を超えるが、数えない
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_on_send_begin(&core, 10, 5000);  // 実際に送り始める
    ws_video_core_tick(&core, 7999);               // 2999 ms
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 8000);               // 3000 ms: ちょうど
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_queue_timeout_boundary(void) {
    start(2, 3000, 10000, 200, 1000000);
    connect_fd(10, 4, 100);
    ws_video_core_tick(&core, 100);
    ws_video_core_tick(&core, 10099);   // 9999 ms
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 10100);   // 10000 ms
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    ws_video_core_tick(&core, 20000);   // 二重に要求しない
    TEST_ASSERT_EQUAL(1, total_closes);
    // フレームは on_send_done まで保持
    TEST_ASSERT_EQUAL(1, hub_held[0]);
    ws_video_core_on_send_done(&core, 10, false);
    TEST_ASSERT_EQUAL(0, hub_held[0]);
    TEST_ASSERT_EQUAL(1, total_closes);
}

void test_queue_timeout_zero_is_disabled_when_send_timeout_zero(void) {
    start(2, 0, 0, 0, 0);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_tick(&core, 4000000000ull);
    TEST_ASSERT_EQUAL(0, total_closes);
}

void test_legacy_mode_queue_timeout_zero_counts_from_queued_time(void) {
    // queue_timeout_ms == 0 (分離しない設定) では、on_send_begin を呼ばない呼び出し側は、従来どおり
    // キューに入れた時刻から send_timeout_ms で停止を数える。
    start(2, 3000, 0, 0, 0);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_tick(&core, 2999);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 3000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_legacy_mode_on_send_begin_restarts_the_clock(void) {
    start(2, 3000, 0, 0, 0);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_on_send_begin(&core, 10, 2000);
    ws_video_core_tick(&core, 4999);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 5000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_on_send_begin_ignored_for_unknown_or_idle_fd(void) {
    start(2, 3000, 10000, 200, 1000000);
    ws_video_core_on_send_begin(&core, 99, 5);        // 未知の fd
    ws_video_core_on_send_begin(NULL, 10, 5);         // NULL
    connect_fd(10, 4, 0);
    ws_video_core_on_send_begin(&core, 10, 5);        // 送信中でない (キューに入れていない)
    TEST_ASSERT_FALSE(core.send_begun[0]);
    TEST_ASSERT_EQUAL_UINT64(0, core.send_started_ms[0]);
    ws_video_core_tick(&core, 100);
    TEST_ASSERT_FALSE(core.send_begun[0]);            // 以前の呼び出しが、次の送信に残らない
}

void test_second_on_send_begin_does_not_extend_the_start(void) {
    start(2, 3000, 10000, 0, 0);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_on_send_begin(&core, 10, 1000);
    ws_video_core_on_send_begin(&core, 10, 2000);     // 無視される
    TEST_ASSERT_EQUAL_UINT64(1000, core.send_started_ms[0]);
    ws_video_core_tick(&core, 3999);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 4000);                  // 1000 + 3000
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_send_done_without_begin_cleans_up(void) {
    start(2, 3000, 10000, 200, 1000000);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 0);
    TEST_ASSERT_EQUAL(1, hub_held[0]);
    ws_video_core_on_send_done(&core, 10, true);      // on_send_begin なし (例えば、送る前に別の理由で終わった)
    TEST_ASSERT_EQUAL(0, hub_held[0]);
    TEST_ASSERT_EQUAL(1, rel_count);
    TEST_ASSERT_EQUAL(0, total_closes);
    TEST_ASSERT_FALSE(core.clients.clients[0].is_sending);
    TEST_ASSERT_FALSE(core.send_begun[0]);
    TEST_ASSERT_EQUAL_UINT64(0, core.queued_ms[0]);

    // 次のフレームは、新しい時刻から数える。古い時刻が残らない。
    next_frame();
    ws_video_core_tick(&core, 50000);
    TEST_ASSERT_EQUAL(2, begin_count);
    ws_video_core_tick(&core, 59999);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 60000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_send_done_failure_without_begin_closes_once(void) {
    start(2, 3000, 10000, 200, 1000000);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_on_send_done(&core, 10, false);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_EQUAL(0, hub_held[0]);
    ws_video_core_tick(&core, 100000);
    TEST_ASSERT_EQUAL(1, total_closes);
}

void test_begun_flag_does_not_leak_to_next_frame(void) {
    start(2, 3000, 10000, 0, 0);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_on_send_begin(&core, 10, 0);
    ws_video_core_on_send_done(&core, 10, true);
    next_frame();
    ws_video_core_tick(&core, 100);                   // 次のフレームをキューへ (まだ送り始めない)
    TEST_ASSERT_EQUAL(2, begin_count);
    TEST_ASSERT_FALSE(core.send_begun[0]);
    ws_video_core_tick(&core, 5000);                  // キュー待ち 4900 ms。前のフレームの開始時刻 (0) から数えると、閉じてしまう
    TEST_ASSERT_EQUAL(0, total_closes);
}

void test_begin_send_failure_does_not_leave_queue_state(void) {
    start(2, 3000, 10000, 200, 1000000);
    connect_fd(10, 4, 0);
    begin_ret = -1;
    ws_video_core_tick(&core, 0);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_FALSE(core.clients.clients[0].is_sending);
    TEST_ASSERT_FALSE(core.send_begun[0]);
}

void test_on_close_during_queue_then_send_done_unregisters_once(void) {
    start(2, 3000, 10000, 200, 1000000);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_on_close(&core, 10);
    TEST_ASSERT_EQUAL(0, unreg_count);
    ws_video_core_on_send_begin(&core, 10, 5);        // CLOSING でも、クラッシュしない
    ws_video_core_tick(&core, 100000);                // CLOSING のクライアントは、tick で閉じ直さない
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_on_send_done(&core, 10, true);
    TEST_ASSERT_EQUAL(1, unreg_count);
    TEST_ASSERT_EQUAL(0, hub_held[0]);
    TEST_ASSERT_EQUAL(0, bad_unregister_violations);
}

void test_slot_reuse_clears_queue_state(void) {
    start(2, 3000, 10000, 200, 1000000);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 777);
    ws_video_core_on_send_begin(&core, 10, 800);
    ws_video_core_on_close(&core, 10);
    ws_video_core_on_send_done(&core, 10, true);      // FREE
    connect_fd(11, 4, 9000);
    TEST_ASSERT_FALSE(core.send_begun[0]);
    TEST_ASSERT_EQUAL_UINT64(0, core.send_started_ms[0]);
    TEST_ASSERT_EQUAL_UINT64(0, core.queued_ms[0]);
}

/* ---- 1 フレーム全体の期限 ---- */

/* 大きさごとの期限 (base 200、1 MB/s) の境界 ±1 ms を、send_timeout を無効にして見る */
void test_deadline_boundary_per_frame_size(void) {
    static const size_t sizes[] = {0, 1, 4096, 114688, 600000, 786432, 5000000};
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        start(2, 0, 10000, 200, 1000000);
        connect_fd(10, sizes[k], 0);
        uint64_t d = ws_video_frame_deadline_ms(sizes[k], 200, 1000000);
        TEST_ASSERT_TRUE(d >= 200);
        const uint64_t t0 = 5000;
        ws_video_core_tick(&core, t0);                         // キューへ
        ws_video_core_on_send_begin(&core, 10, t0 + 100);      // 100 ms 後に送り始める
        const uint64_t s = t0 + 100;
        ws_video_core_tick(&core, s + d - 1);
        TEST_ASSERT_EQUAL_MESSAGE(0, total_closes, "closed 1 ms before the deadline");
        ws_video_core_tick(&core, s + d);
        TEST_ASSERT_EQUAL_MESSAGE(1, close_cnt[10], "not closed at the deadline");
        ws_video_core_tick(&core, s + d + 5000);
        TEST_ASSERT_EQUAL(1, total_closes);
        TEST_ASSERT_EQUAL(1, hub_held[0]);                     // フレームは on_send_done まで保持
        ws_video_core_on_send_done(&core, 10, false);
        TEST_ASSERT_EQUAL(0, hub_held[0]);
    }
}

void test_deadline_base_only_when_frame_is_empty(void) {
    start(2, 0, 10000, 333, 1000000);
    connect_fd(10, 0, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_on_send_begin(&core, 10, 0);
    ws_video_core_tick(&core, 332);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 333);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_deadline_not_applied_before_send_begin(void) {
    start(2, 0, 10000, 200, 1000000);
    connect_fd(10, 600000, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_tick(&core, 5000);   // キュー待ち 5000 ms は、期限 (800) を超えているが、キュー待ちには期限を適用しない
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 9999);
    TEST_ASSERT_EQUAL(0, total_closes);
}

void test_deadline_disabled_when_min_rate_zero(void) {
    start(2, 0, 10000, 200, 0);
    connect_fd(10, 5000000, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_on_send_begin(&core, 10, 0);
    ws_video_core_tick(&core, 4000000000ull);
    TEST_ASSERT_EQUAL(0, total_closes);
}

void test_deadline_closes_only_the_slow_client(void) {
    start(4, 0, 10000, 200, 1000000);
    connect_fd(10, 600000, 0);   // A: 期限 800 ms (遅い。送り終わらない)
    connect_fd(11, 4096, 0);     // B: 期限 204 ms
    connect_fd(12, 114688, 0);   // C: 期限 314 ms
    ws_video_core_tick(&core, 0);
    TEST_ASSERT_EQUAL(3, begin_count);
    for (int fd = 10; fd <= 12; fd++) ws_video_core_on_send_begin(&core, fd, 0);

    ws_video_core_tick(&core, 203);                // B の期限 (204) の 1 ms 前
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_on_send_done(&core, 11, true);   // B は間に合った
    ws_video_core_tick(&core, 313);                // C の期限 (314) の 1 ms 前
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_on_send_done(&core, 12, true);   // C も間に合った
    ws_video_core_tick(&core, 799);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 800);                // A の期限
    TEST_ASSERT_EQUAL(1, close_cnt[10]);           // A だけ
    TEST_ASSERT_EQUAL(0, close_cnt[11]);
    TEST_ASSERT_EQUAL(0, close_cnt[12]);
    TEST_ASSERT_EQUAL(1, total_closes);

    // B と C は、影響を受けず、次のフレームを受け取れる
    next_frame();
    ws_video_core_tick(&core, 900);
    TEST_ASSERT_EQUAL(5, begin_count);
    TEST_ASSERT_EQUAL(1, total_closes);
}

void test_deadline_uses_each_clients_own_frame_size(void) {
    start(4, 0, 10000, 200, 1000000);
    connect_fd(10, 0, 0);        // 期限 200
    connect_fd(11, 600000, 0);   // 期限 800
    ws_video_core_tick(&core, 0);
    ws_video_core_on_send_begin(&core, 10, 0);
    ws_video_core_on_send_begin(&core, 11, 0);
    ws_video_core_tick(&core, 199);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 200);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    TEST_ASSERT_EQUAL(0, close_cnt[11]);
    ws_video_core_tick(&core, 799);
    TEST_ASSERT_EQUAL(0, close_cnt[11]);
    ws_video_core_tick(&core, 800);
    TEST_ASSERT_EQUAL(1, close_cnt[11]);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_normal_client_not_blocked_by_slow_client_queue(void) {
    // 遅いクライアント (A) が httpd を占有して、B の送信のキュー待ちが長くなっても、B は send_timeout で切られない。
    start(2, 3000, 10000, 200, 1000000);
    connect_fd(10, 114688, 0);
    connect_fd(11, 114688, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_on_send_begin(&core, 10, 0);    // A が先に送り始める (期限 314 ms)
    ws_video_core_tick(&core, 313);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 314);               // A は期限で閉じる
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
    ws_video_core_on_send_done(&core, 10, false); // A の送信が終わる (失敗)
    ws_video_core_on_send_begin(&core, 11, 314);  // B が、ようやく送り始める。キュー待ちは 314 ms
    ws_video_core_tick(&core, 500);
    TEST_ASSERT_EQUAL(0, close_cnt[11]);
    ws_video_core_on_send_done(&core, 11, true);
    TEST_ASSERT_EQUAL(0, close_cnt[11]);
}

void test_send_timeout_and_deadline_both_apply_but_close_once(void) {
    start(2, 300, 10000, 200, 1000000);           // 期限 314 より、send_timeout (300) が早い
    connect_fd(10, 114688, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_on_send_begin(&core, 10, 0);
    ws_video_core_tick(&core, 299);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 300);
    TEST_ASSERT_EQUAL(1, total_closes);
    ws_video_core_tick(&core, 314);
    ws_video_core_tick(&core, 400);
    TEST_ASSERT_EQUAL(1, total_closes);

    // 期限が send_timeout より早い場合
    start(2, 3000, 10000, 200, 1000000);
    connect_fd(10, 114688, 0);
    ws_video_core_tick(&core, 0);
    ws_video_core_on_send_begin(&core, 10, 0);
    ws_video_core_tick(&core, 313);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 314);
    TEST_ASSERT_EQUAL(1, total_closes);
    ws_video_core_tick(&core, 3000);
    TEST_ASSERT_EQUAL(1, total_closes);
}

void test_deadline_resets_for_each_new_frame(void) {
    start(2, 0, 10000, 200, 1000000);
    connect_fd(10, 114688, 0);                    // 期限 314
    ws_video_core_tick(&core, 0);
    ws_video_core_on_send_begin(&core, 10, 0);
    ws_video_core_on_send_done(&core, 10, true);  // 313 ms 以内に終わる
    next_frame();
    ws_video_core_tick(&core, 1000);
    ws_video_core_on_send_begin(&core, 10, 1000);
    ws_video_core_tick(&core, 1313);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 1314);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

/* ---- 時刻の巻き戻り ---- */

void test_clock_rollback_after_begin_clamps_start_for_deadline(void) {
    start(2, 0, 10000, 200, 1000000);
    connect_fd(10, 114688, 0);                    // 期限 314
    ws_video_core_tick(&core, 5000);
    ws_video_core_on_send_begin(&core, 10, 5000);
    ws_video_core_tick(&core, 1000);              // 5000 -> 1000 に巻き戻る
    TEST_ASSERT_EQUAL(0, total_closes);           // 符号なしの引き算で、巨大な値になって閉じてはいけない
    TEST_ASSERT_EQUAL_UINT64(1000, core.send_started_ms[0]);
    ws_video_core_tick(&core, 1313);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 1314);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_clock_rollback_in_queue_clamps_queued_time(void) {
    start(2, 3000, 10000, 200, 1000000);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 5000);              // キューへ
    ws_video_core_tick(&core, 1000);              // 巻き戻り
    TEST_ASSERT_EQUAL(0, total_closes);
    TEST_ASSERT_EQUAL_UINT64(1000, core.queued_ms[0]);
    ws_video_core_tick(&core, 10999);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 11000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_clock_rollback_in_legacy_mode_clamps_queued_time(void) {
    start(2, 3000, 0, 0, 0);
    connect_fd(10, 4, 0);
    ws_video_core_tick(&core, 5000);
    ws_video_core_tick(&core, 1000);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 3999);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 4000);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

void test_on_send_begin_with_earlier_time_than_queue_does_not_close(void) {
    // 呼び出し側の時計がずれて、begin の時刻が queued より小さくなっても、誤って閉じない
    start(2, 3000, 10000, 200, 1000000);
    connect_fd(10, 114688, 0);
    ws_video_core_tick(&core, 5000);
    ws_video_core_on_send_begin(&core, 10, 4000);
    ws_video_core_tick(&core, 4100);
    TEST_ASSERT_EQUAL(0, total_closes);
    ws_video_core_tick(&core, 4314);
    TEST_ASSERT_EQUAL(1, close_cnt[10]);
}

/* ---- 不変条件ファズ ----
 * 独立した小さなモデル (queue 時刻・開始時刻をテスト側で持つ) で、tick ごとに「閉じるべきクライアント」を決め、
 * 実際に閉じられた集合と一致することを検査する。ほかに、フレームの二重保持・二重解放・二重の close 要求がないこと。 */

static uint32_t rng_state;
static uint32_t rnd(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}

typedef struct {
    bool connected;
    bool sending;       // モデルの is_sending (tick のキュー投入 / on_send_done で変わる)
    bool begun;
    bool closed;        // 何らかの理由で close 要求済み
    uint64_t queued;
    uint64_t started;
    size_t size;
} model_t;

static model_t mdl[MAXFD];
static uint64_t g_now;

static int fz_begin_send(void *u, ws_video_conn_t c, const uint8_t *d, size_t l) {
    (void)u; (void)d;
    uintptr_t fd = (uintptr_t)c;
    begin_count++;
    if (begin_ret != 0) return begin_ret;
    mdl[fd].sending = true;
    mdl[fd].begun = false;
    mdl[fd].queued = g_now;
    mdl[fd].size = l;
    return 0;
}

static const size_t fuzz_sizes[] = {0, 1, 4096, 114688, 600000, 786432, 5000000};
static const uint64_t fuzz_send_to[] = {0, 300, 3000};
static const uint64_t fuzz_queue_to[] = {0, 500, 10000};
static const uint32_t fuzz_rate[] = {0, 1000000, 40000};
static const uint32_t fuzz_base[] = {0, 200, 5000};

static int fuzz_one(uint32_t seed, int steps) {
    rng_state = seed ? seed : 1;
    uint64_t send_to = fuzz_send_to[rnd() % 3];
    uint64_t queue_to = fuzz_queue_to[rnd() % 3];
    uint32_t rate = fuzz_rate[rnd() % 3];
    uint32_t base = fuzz_base[rnd() % 3];

    ws_video_core_destroy(&core);
    reset_all();
    memset(mdl, 0, sizeof(mdl));
    ws_video_core_config_t cfg = {
        .max_clients = 5,
        .session_check_interval_ms = 1000000000ull,
        .send_timeout_ms = send_to,
        .queue_timeout_ms = queue_to,
        .frame_deadline_base_ms = base,
        .frame_min_rate_bytes_per_s = rate,
        .hub_register = m_register,
        .hub_unregister = m_unregister,
        .hub_get_latest = m_get_latest,
        .hub_release = m_release,
        .io_begin_send = fz_begin_send,
        .io_request_close = m_close,
        .io_validate_session = m_validate,
    };
    if (ws_video_core_init(&core, &cfg) != 0) return -1;

    g_now = 1000;
    int timer_closes_seen = 0;
    for (int step = 0; step < steps; step++) {
        int fd = 10 + (int)(rnd() % 5);
        uint32_t r = rnd() % 100;
        model_t *m = &mdl[fd];
        if (r < 12) { // connect
            if (!m->connected) {
                uint8_t tok[32] = {0};
                hub_next_size = (int)fuzz_sizes[rnd() % 7];
                close_cnt[fd] = 0;
                if (ws_video_core_on_connect(&core, (ws_video_conn_t)(uintptr_t)fd, fd, tok, g_now) >= 0) {
                    memset(m, 0, sizeof(*m));
                    m->connected = true;
                }
            }
        } else if (r < 20) { // close from the transport
            if (m->connected) {
                int slot = ws_clients_find(&core.clients, fd);
                ws_video_core_on_close(&core, fd);
                if (slot >= 0 && core.clients.clients[slot].state == WS_CLIENT_FREE) m->connected = false;
                else m->closed = true; // CLOSING: tick は見ない
            }
        } else if (r < 40) { // on_send_begin
            if (m->connected && m->sending && !m->begun && !m->closed) {
                ws_video_core_on_send_begin(&core, fd, g_now);
                m->begun = true;
                m->started = g_now;
            } else if (m->connected) {
                // 無効な呼び出し (2 回目・送信中でない) は、状態を変えない
                ws_video_core_on_send_begin(&core, fd, g_now + 12345);
            }
        } else if (r < 58) { // on_send_done
            if (m->connected && m->sending) {
                bool ok = (rnd() % 4) != 0;
                ws_video_core_on_send_done(&core, fd, ok);
                m->sending = false;
                m->begun = false;
                if (!ok) m->closed = true;
                int slot = ws_clients_find(&core.clients, fd);
                if (slot < 0) { m->connected = false; }
            }
        } else if (r < 62) { // client data
            if (m->connected) {
                ws_video_core_on_client_data(&core, fd);
                m->closed = true;
            }
        } else { // tick
            g_now += rnd() % 400;
            g_seq++;
            bool should_close[MAXFD] = {0};
            bool pre_sending[MAXFD] = {0};
            for (int f = 10; f < 10 + 5; f++) {
                model_t *q = &mdl[f];
                if (!q->connected || q->closed || !q->sending) continue;
                int sl = ws_clients_find(&core.clients, f);
                if (sl < 0 || core.clients.clients[sl].state != WS_CLIENT_ACTIVE) continue;
                pre_sending[f] = true;
                if (q->begun) {
                    uint64_t el = g_now - q->started;
                    if (send_to > 0 && el >= send_to) should_close[f] = true;
                    if (rate > 0 && el >= ws_video_frame_deadline_ms(q->size, base, rate)) {
                        should_close[f] = true;
                    }
                } else {
                    uint64_t w = g_now - q->queued;
                    if (queue_to > 0) { if (w >= queue_to) should_close[f] = true; }
                    else if (send_to > 0 && w >= send_to) should_close[f] = true;
                }
            }
            int before[MAXFD];
            memcpy(before, close_cnt, sizeof(before));
            ws_video_core_tick(&core, g_now);
            for (int f = 10; f < 15; f++) {
                if (!pre_sending[f]) continue;
                bool did = close_cnt[f] > before[f];
                if (did != should_close[f]) {
                    printf("seed=%u step=%d fd=%d did=%d should=%d begun=%d\n", (unsigned)seed, step, f, did, should_close[f], mdl[f].begun);
                    return -2;
                }
                if (did) { mdl[f].closed = true; timer_closes_seen++; }
            }
            // tick が、新たに送信を始めたクライアントや、失敗で閉じたクライアントを、モデルに反映する
            for (int f = 10; f < 15; f++) {
                model_t *q = &mdl[f];
                if (!q->connected) continue;
                if (close_cnt[f] > 0) q->closed = true;
            }
        }
        for (int f = 10; f < 15; f++) {
            if (close_cnt[f] > 1) { printf("seed=%u step=%d fd=%d double close\n", (unsigned)seed, step, f); return -3; }
        }
        if (double_hold_violations || bad_release_violations || bad_unregister_violations) {
            printf("seed=%u step=%d hub violation d=%d r=%d u=%d\n", (unsigned)seed, step, double_hold_violations, bad_release_violations, bad_unregister_violations);
            return -4;
        }
    }
    // 後始末: すべての送信を終わらせて閉じ、フレームの保持が残らないこと
    for (int f = 10; f < 15; f++) {
        if (mdl[f].connected) {
            int slot = ws_clients_find(&core.clients, f);
            if (slot >= 0 && core.clients.clients[slot].is_sending) ws_video_core_on_send_done(&core, f, true);
            ws_video_core_on_close(&core, f);
            slot = ws_clients_find(&core.clients, f);
            if (slot >= 0 && core.clients.clients[slot].is_sending) ws_video_core_on_send_done(&core, f, true);
        }
    }
    for (int i = 0; i < MAXID; i++) {
        if (hub_held[i] != 0) { printf("seed=%u leftover hold id=%d\n", (unsigned)seed, i); return -5; }
    }
    return timer_closes_seen;
}

void test_fuzz_timers_match_model(void) {
    long timer_closes = 0;
    for (uint32_t seed = 1; seed <= 300; seed++) {
        int r = fuzz_one(seed * 2654435761u + 7u, 4000);
        TEST_ASSERT_TRUE_MESSAGE(r >= 0, "model mismatch or invariant violation (see printed seed)");
        timer_closes += r;
    }
    printf("fuzz timer closes=%ld\n", timer_closes);
    // 空振りでないこと (固定シードなので毎回同じ値)
    TEST_ASSERT_TRUE_MESSAGE(timer_closes > 10000, "fuzz closed too few clients by timers");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_deadline_rate_zero_means_no_deadline);
    RUN_TEST(test_deadline_values);
    RUN_TEST(test_deadline_monotonic_in_size_base_and_inverse_in_rate);
    RUN_TEST(test_deadline_saturates);

    RUN_TEST(test_send_started_set_by_on_send_begin_not_by_begin_send);
    RUN_TEST(test_queue_wait_not_counted_in_send_timeout);
    RUN_TEST(test_queue_timeout_boundary);
    RUN_TEST(test_queue_timeout_zero_is_disabled_when_send_timeout_zero);
    RUN_TEST(test_legacy_mode_queue_timeout_zero_counts_from_queued_time);
    RUN_TEST(test_legacy_mode_on_send_begin_restarts_the_clock);
    RUN_TEST(test_on_send_begin_ignored_for_unknown_or_idle_fd);
    RUN_TEST(test_second_on_send_begin_does_not_extend_the_start);
    RUN_TEST(test_send_done_without_begin_cleans_up);
    RUN_TEST(test_send_done_failure_without_begin_closes_once);
    RUN_TEST(test_begun_flag_does_not_leak_to_next_frame);
    RUN_TEST(test_begin_send_failure_does_not_leave_queue_state);
    RUN_TEST(test_on_close_during_queue_then_send_done_unregisters_once);
    RUN_TEST(test_slot_reuse_clears_queue_state);

    RUN_TEST(test_deadline_boundary_per_frame_size);
    RUN_TEST(test_deadline_base_only_when_frame_is_empty);
    RUN_TEST(test_deadline_not_applied_before_send_begin);
    RUN_TEST(test_deadline_disabled_when_min_rate_zero);
    RUN_TEST(test_deadline_closes_only_the_slow_client);
    RUN_TEST(test_deadline_uses_each_clients_own_frame_size);
    RUN_TEST(test_normal_client_not_blocked_by_slow_client_queue);
    RUN_TEST(test_send_timeout_and_deadline_both_apply_but_close_once);
    RUN_TEST(test_deadline_resets_for_each_new_frame);

    RUN_TEST(test_clock_rollback_after_begin_clamps_start_for_deadline);
    RUN_TEST(test_clock_rollback_in_queue_clamps_queued_time);
    RUN_TEST(test_clock_rollback_in_legacy_mode_clamps_queued_time);
    RUN_TEST(test_on_send_begin_with_earlier_time_than_queue_does_not_close);

    RUN_TEST(test_fuzz_timers_match_model);
    return UNITY_END();
}

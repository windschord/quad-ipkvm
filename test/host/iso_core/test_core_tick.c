/*
 * iso_upload_tick(設計 4.2、6.1): アイドルのタイムアウト(ISO_UPLOAD_IDLE_S = 300 秒。経過 >= 300000 ms で IDLE_CLOSE を投入)、
 * 時刻の巻き戻りは経過 0、upload.state の receiving / idle の境(5 秒)。期待値は手書き。
 */
#include "core_common.h"

void setUp(void) { fx_core_setup(0, 0); }
void tearDown(void) { fx_core_teardown(); }

#define SZ 100000ull
#define T0 1000000ull

static iso_upload_t *up(void) { return iso_core_upload(&CORE); }
static iso_catalog_entry_t *find(const char *id) { return iso_catalog_find(&CAT, id); }

static void start_upload(char id[ISO_ID_LEN + 1]) {
    NOW_MS = T0;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, SZ, NULL, id));
}

void test_tick_without_a_slot_does_nothing(void) {
    iso_upload_tick(&CORE, T0);
    iso_upload_tick(&CORE, T0 + 10000000);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_INT(0, VIEW_UPDATES);
}

void test_idle_timeout_boundary_is_300_seconds(void) {
    char id[9];
    start_upload(id);
    iso_upload_tick(&CORE, T0 + 299999);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    iso_upload_tick(&CORE, T0 + 300000);
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
    /* もう一度呼んでも、重複して積まない(合流) */
    iso_upload_tick(&CORE, T0 + 301000);
    iso_upload_tick(&CORE, T0 + 302000);
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
    iso_job_t *j = iso_core_pick(&CORE);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_INT(ISO_JOB_IDLE_CLOSE, j->kind);
    TEST_ASSERT_EQUAL_STRING(id, j->id);
    iso_step_result_t r = run_job(j, 0, 1000, 10, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, find(id)->state);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(up()));
    iso_upload_tick(&CORE, T0 + 9000000); /* 枠が空いたので、もう積まない */
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
}

void test_the_time_is_counted_from_the_last_put(void) {
    char id[9];
    start_upload(id);
    NOW_MS = T0 + 200000;
    put_all(id, 0, 1000, 1000);
    iso_upload_tick(&CORE, T0 + 200000 + 299999);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    iso_upload_tick(&CORE, T0 + 200000 + 300000);
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
}

void test_a_finalizing_slot_never_times_out(void) {
    char id[9];
    start_upload(id);
    iso_catalog_entry_t *e = find(id);
    iso_meta_t m = e->meta;
    m.part_blocks = PB_SMALL;
    m.part_count = 3;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_write_meta(&CAT, e, &m));
    put_all(id, 0, SZ, 20000);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL));
    iso_upload_tick(&CORE, T0 + 100000000);
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE)); /* FINISH だけ */
    iso_job_t *j = iso_core_pick(&CORE);
    TEST_ASSERT_EQUAL_INT(ISO_JOB_FINISH, j->kind);
    run_job(j, 0, 1000, 1000, NULL);
}

void test_clock_going_backwards_counts_as_zero_elapsed_and_rebases(void) {
    char id[9];
    start_upload(id);
    iso_upload_tick(&CORE, T0 - 500000); /* 巻き戻り */
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_UINT64(T0 - 500000, up()->last_put_ms); /* 基準を引き直す */
    iso_upload_tick(&CORE, T0 - 500000 + 299999);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    iso_upload_tick(&CORE, T0 - 500000 + 300000);
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
}

void test_clock_going_back_by_one_ms_is_also_zero_elapsed(void) {
    char id[9];
    start_upload(id);
    iso_upload_tick(&CORE, T0 - 1);
    TEST_ASSERT_EQUAL_UINT64(T0 - 1, up()->last_put_ms);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    iso_upload_tick(&CORE, T0); /* 同じ時刻は巻き戻りではない */
    TEST_ASSERT_EQUAL_UINT64(T0 - 1, up()->last_put_ms);
}

void test_clock_that_wraps_to_zero_does_not_time_out_at_once(void) {
    char id[9];
    start_upload(id);
    iso_upload_tick(&CORE, 0);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
}

void test_a_full_queue_defers_the_idle_close_to_the_next_tick(void) {
    for (int i = 0; i < 8; i++) {
        char id[9], name[8];
        snprintf(id, sizeof(id), "000001%02d", i);
        snprintf(name, sizeof(name), "q%d", i);
        make_image(id, name, SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    }
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    char id[9];
    start_upload(id);
    for (int i = 0; i < 8; i++) {
        char qid[9];
        snprintf(qid, sizeof(qid), "000001%02d", i);
        TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, qid));
    }
    iso_upload_tick(&CORE, T0 + 300000);
    TEST_ASSERT_EQUAL_UINT(8, iso_core_queue_len(&CORE)); /* あふれた(ISO_ERR_BUSY は、tick の中で吸収する) */
    TEST_ASSERT_NOT_NULL(iso_core_pick(&CORE));
    TEST_ASSERT_EQUAL_UINT(7, iso_core_queue_len(&CORE));
    iso_upload_tick(&CORE, T0 + 301000);
    TEST_ASSERT_EQUAL_UINT(8, iso_core_queue_len(&CORE)); /* 次の tick で積まれた */
}

void test_receiving_to_idle_edge_publishes_the_view_once(void) {
    char id[9];
    start_upload(id);
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_RECEIVING, VIEW_COPY.upload_state);
    int n = VIEW_UPDATES;
    iso_upload_tick(&CORE, T0 + 5000); /* 5 秒ちょうど: まだ receiving */
    TEST_ASSERT_EQUAL_INT(n, VIEW_UPDATES);
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_RECEIVING, VIEW_COPY.upload_state);
    iso_upload_tick(&CORE, T0 + 5001);
    TEST_ASSERT_EQUAL_INT(n + 1, VIEW_UPDATES);
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_IDLE, VIEW_COPY.upload_state);
    TEST_ASSERT_EQUAL_STRING(id, VIEW_COPY.upload_id);
    iso_upload_tick(&CORE, T0 + 6000); /* 変わらなければ、更新しない */
    iso_upload_tick(&CORE, T0 + 7000);
    TEST_ASSERT_EQUAL_INT(n + 1, VIEW_UPDATES);
    /* PUT で receiving に戻る(PUT が更新する) */
    NOW_MS = T0 + 8000;
    put_all(id, 0, 100, 100);
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_RECEIVING, VIEW_COPY.upload_state);
    int m = VIEW_UPDATES;
    iso_upload_tick(&CORE, T0 + 8000 + 5000);
    TEST_ASSERT_EQUAL_INT(m, VIEW_UPDATES);
    iso_upload_tick(&CORE, T0 + 8000 + 5001);
    TEST_ASSERT_EQUAL_INT(m + 1, VIEW_UPDATES);
}

void test_activity_after_a_clock_rewind_is_receiving(void) {
    char id[9];
    start_upload(id);
    iso_upload_tick(&CORE, T0 + 6000); /* idle に */
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_IDLE, VIEW_COPY.upload_state);
    iso_upload_tick(&CORE, T0 - 100000); /* 巻き戻り: 経過 0 = receiving */
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_RECEIVING, VIEW_COPY.upload_state);
}

void test_the_view_shows_finalizing_after_finish_is_accepted_even_if_never_ticked(void) {
    char id[9];
    start_upload(id);
    iso_catalog_entry_t *e = find(id);
    iso_meta_t m = e->meta;
    m.part_blocks = PB_SMALL;
    m.part_count = 3;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_write_meta(&CAT, e, &m));
    put_all(id, 0, SZ, 20000);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_FINALIZING, VIEW_COPY.upload_state);
    int n = VIEW_UPDATES;
    iso_upload_tick(&CORE, T0 + 100);
    iso_upload_tick(&CORE, T0 + 200);
    TEST_ASSERT_EQUAL_INT(n, VIEW_UPDATES);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tick_without_a_slot_does_nothing);
    RUN_TEST(test_idle_timeout_boundary_is_300_seconds);
    RUN_TEST(test_the_time_is_counted_from_the_last_put);
    RUN_TEST(test_a_finalizing_slot_never_times_out);
    RUN_TEST(test_clock_going_backwards_counts_as_zero_elapsed_and_rebases);
    RUN_TEST(test_clock_going_back_by_one_ms_is_also_zero_elapsed);
    RUN_TEST(test_clock_that_wraps_to_zero_does_not_time_out_at_once);
    RUN_TEST(test_a_full_queue_defers_the_idle_close_to_the_next_tick);
    RUN_TEST(test_receiving_to_idle_edge_publishes_the_view_once);
    RUN_TEST(test_activity_after_a_clock_rewind_is_receiving);
    RUN_TEST(test_the_view_shows_finalizing_after_finish_is_accepted_even_if_never_ticked);
    return UNITY_END();
}

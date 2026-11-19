/*
 * iso_catalog: 状態の遷移表(設計 3.4)の全行。7 状態 x 14 イベント = 98 マスを、手書きの表で固定する。
 * 既定の文脈: received_complete = true、slot_free = true、mounted = false。フラグで変わる行は、別のテストで固定する。
 */
#include "ic_common.h"

void setUp(void) {}
void tearDown(void) {}

#define N ISO_STATE_NONE
#define U ISO_STATE_UPLOADING
#define I ISO_STATE_INCOMPLETE
#define F ISO_STATE_FINALIZING
#define R ISO_STATE_READY
#define V ISO_STATE_INVALID
#define D ISO_STATE_DELETING

typedef struct {
    iso_trans_t r;
    iso_state_t next;
} cell_t;

#define OK(s) {ISO_TR_OK, (s)}
/* 拒否: next は現在の状態のまま(表の中では、行の状態を書く) */
#define RS(cur) {ISO_TR_REJECT_STATE, (cur)}

static const char *const STATE_NAME[] = {"NONE", "UPLOADING", "INCOMPLETE", "FINALIZING", "READY", "INVALID", "DELETING"};
static const char *const EV_NAME[] = {"BEGIN",        "PUT",         "IDLE_TIMEOUT",  "WRITE_FAIL",   "REBOOT",
                                      "FINISH",       "FINALIZE_OK", "FINALIZE_FAIL", "FINALIZE_IOERR", "INDEX_OK",
                                      "INDEX_FAIL",   "INDEX_IOERR", "DELETE",        "DELETE_DONE"};

/* 行 = 現在の状態、列 = イベント(iso_event_t の順) */
static const cell_t TABLE[7][14] = {
    /* NONE */
    {OK(U), RS(N), RS(N), RS(N), RS(N), RS(N), RS(N), RS(N), RS(N), RS(N), RS(N), RS(N), RS(N), RS(N)},
    /* UPLOADING:  BEGIN   PUT    IDLE   WFAIL  REBOOT FINISH  FOK    FFAIL  FIO    IOK    IFAIL  IIO    DELETE DDONE */
    {RS(U), OK(U), OK(I), OK(I), OK(I), OK(F), RS(U), RS(U), RS(U), RS(U), RS(U), RS(U), OK(D), RS(U)},
    /* INCOMPLETE */
    {RS(I), OK(U), RS(I), RS(I), OK(I), OK(F), RS(I), RS(I), RS(I), RS(I), RS(I), RS(I), OK(D), RS(I)},
    /* FINALIZING */
    {RS(F), RS(F), RS(F), RS(F), OK(I), OK(F), OK(R), OK(V), OK(I), RS(F), RS(F), RS(F), OK(D), RS(F)},
    /* READY */
    {RS(R), RS(R), RS(R), RS(R), OK(R), OK(R), RS(R), RS(R), RS(R), OK(R), OK(V), OK(R), OK(D), RS(R)},
    /* INVALID: FINISH は 422 */
    {RS(V), RS(V), RS(V), RS(V), OK(V), {ISO_TR_REJECT_INVALID, V}, RS(V), RS(V), RS(V), RS(V), RS(V), RS(V), OK(D), RS(V)},
    /* DELETING */
    {RS(D), RS(D), RS(D), RS(D), OK(D), RS(D), RS(D), RS(D), RS(D), RS(D), RS(D), RS(D), OK(D), OK(N)},
};

void test_transition_table_every_cell(void) {
    /* 走査の前に、表の列数が iso_event_t と一致していること(イベントを足して表を直し忘れると、範囲外を読む) */
    TEST_ASSERT_EQUAL_UINT(ISO_EV_COUNT, sizeof(EV_NAME) / sizeof(EV_NAME[0]));
    TEST_ASSERT_EQUAL_UINT(ISO_EV_COUNT, sizeof(TABLE[0]) / sizeof(TABLE[0][0]));
    TEST_ASSERT_EQUAL_UINT(7, sizeof(TABLE) / sizeof(TABLE[0]));
    TEST_ASSERT_EQUAL_UINT(7, sizeof(STATE_NAME) / sizeof(STATE_NAME[0]));
    const iso_trans_ctx_t ctx = {true, true, false};
    unsigned ok_cells = 0, reject_cells = 0;
    for (int s = 0; s < 7; s++) {
        for (int e = 0; e < ISO_EV_COUNT; e++) {
            iso_state_t next = (iso_state_t)99;
            iso_trans_t r = iso_state_transition((iso_state_t)s, (iso_event_t)e, &ctx, &next);
            char msg[100];
            snprintf(msg, sizeof(msg), "%s + %s", STATE_NAME[s], EV_NAME[e]);
            TEST_ASSERT_EQUAL_INT_MESSAGE(TABLE[s][e].r, r, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(TABLE[s][e].next, next, msg);
            if (r == ISO_TR_OK) ok_cells++; else reject_cells++;
        }
    }
    TEST_ASSERT_EQUAL_UINT(ISO_EV_COUNT, 14); /* 表の列と iso_event_t が一致している */
    /* 受理する行の数(表を数えた値): NONE 1、UPLOADING 6、INCOMPLETE 4、FINALIZING 6、READY 6、INVALID 3、DELETING 3 */
    TEST_ASSERT_EQUAL_UINT(1 + 6 + 4 + 6 + 6 + 2 + 3, ok_cells);
    TEST_ASSERT_EQUAL_UINT(98 - 28, reject_cells);
}

void test_transition_rejection_leaves_next_equal_to_current(void) {
    iso_state_t next = (iso_state_t)42;
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_STATE, iso_state_transition(ISO_STATE_READY, ISO_EV_PUT, NULL, &next));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, next);
    next = (iso_state_t)42;
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_SIZE_MISMATCH, iso_state_transition(ISO_STATE_UPLOADING, ISO_EV_FINISH, NULL, &next));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, next);
    next = (iso_state_t)42;
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_MOUNTED, iso_state_transition(ISO_STATE_READY, ISO_EV_DELETE, &(iso_trans_ctx_t){false, false, true}, &next));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, next);
}

void test_transition_next_may_be_null(void) {
    TEST_ASSERT_EQUAL_INT(ISO_TR_OK, iso_state_transition(ISO_STATE_NONE, ISO_EV_BEGIN, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_STATE, iso_state_transition(ISO_STATE_READY, ISO_EV_BEGIN, NULL, NULL));
}

void test_transition_unknown_event_is_rejected(void) {
    iso_state_t next = (iso_state_t)42;
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_STATE, iso_state_transition(ISO_STATE_UPLOADING, ISO_EV_COUNT, NULL, &next));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, next);
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_STATE, iso_state_transition(ISO_STATE_UPLOADING, (iso_event_t)200, NULL, &next));
}

/* ---- フラグで変わる行 ---- */

static iso_trans_t tr(iso_state_t cur, iso_event_t ev, bool complete, bool slot_free, bool mounted, iso_state_t *next) {
    iso_trans_ctx_t c = {complete, slot_free, mounted};
    return iso_state_transition(cur, ev, &c, next);
}

void test_finish_rows_by_flags(void) {
    iso_state_t n;
    /* uploading: received < size -> size_mismatch(枠は関係ない) */
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_SIZE_MISMATCH, tr(U, ISO_EV_FINISH, false, true, false, &n));
    TEST_ASSERT_EQUAL_INT(U, n);
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_SIZE_MISMATCH, tr(U, ISO_EV_FINISH, false, false, false, &n));
    /* uploading: 完了、枠は自分のもの -> finalizing(slot_free = false でも) */
    TEST_ASSERT_EQUAL_INT(ISO_TR_OK, tr(U, ISO_EV_FINISH, true, false, false, &n));
    TEST_ASSERT_EQUAL_INT(F, n);
    /* incomplete: received < size -> size_mismatch。枠が使われていても、size_mismatch が先(WEB_API 8 の並び) */
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_SIZE_MISMATCH, tr(I, ISO_EV_FINISH, false, true, false, &n));
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_SIZE_MISMATCH, tr(I, ISO_EV_FINISH, false, false, false, &n));
    TEST_ASSERT_EQUAL_INT(I, n);
    /* incomplete: 完了、枠が空いている -> finalizing。別のイメージが枠を使っている -> upload_in_progress */
    TEST_ASSERT_EQUAL_INT(ISO_TR_OK, tr(I, ISO_EV_FINISH, true, true, false, &n));
    TEST_ASSERT_EQUAL_INT(F, n);
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_UPLOAD_IN_PROGRESS, tr(I, ISO_EV_FINISH, true, false, false, &n));
    TEST_ASSERT_EQUAL_INT(I, n);
    /* finalizing / ready: 冪等。フラグに依らない */
    for (int c = 0; c < 8; c++) {
        bool complete = c & 1, slot = c & 2, mounted = c & 4;
        TEST_ASSERT_EQUAL_INT(ISO_TR_OK, tr(F, ISO_EV_FINISH, complete, slot, mounted, &n));
        TEST_ASSERT_EQUAL_INT(F, n);
        TEST_ASSERT_EQUAL_INT(ISO_TR_OK, tr(R, ISO_EV_FINISH, complete, slot, mounted, &n));
        TEST_ASSERT_EQUAL_INT(R, n);
        TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_INVALID, tr(V, ISO_EV_FINISH, complete, slot, mounted, &n));
        TEST_ASSERT_EQUAL_INT(V, n);
    }
}

void test_put_rows_by_flags(void) {
    iso_state_t n;
    /* uploading: 枠のフラグに依らず受理 */
    TEST_ASSERT_EQUAL_INT(ISO_TR_OK, tr(U, ISO_EV_PUT, false, false, false, &n));
    TEST_ASSERT_EQUAL_INT(U, n);
    TEST_ASSERT_EQUAL_INT(ISO_TR_OK, tr(U, ISO_EV_PUT, true, true, true, &n));
    /* incomplete: 枠が空いていれば再開、そうでなければ upload_in_progress */
    TEST_ASSERT_EQUAL_INT(ISO_TR_OK, tr(I, ISO_EV_PUT, false, true, false, &n));
    TEST_ASSERT_EQUAL_INT(U, n);
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_UPLOAD_IN_PROGRESS, tr(I, ISO_EV_PUT, true, false, false, &n));
    TEST_ASSERT_EQUAL_INT(I, n);
    /* ready / invalid / finalizing は 409 not_uploading(REJECT_STATE) */
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_STATE, tr(R, ISO_EV_PUT, true, true, false, &n));
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_STATE, tr(V, ISO_EV_PUT, true, true, false, &n));
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_STATE, tr(F, ISO_EV_PUT, true, true, false, &n));
}

void test_delete_rows_by_flags(void) {
    iso_state_t n;
    /* マウント中の ready だけ拒否 */
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_MOUNTED, tr(R, ISO_EV_DELETE, true, true, true, &n));
    TEST_ASSERT_EQUAL_INT(R, n);
    TEST_ASSERT_EQUAL_INT(ISO_TR_OK, tr(R, ISO_EV_DELETE, true, true, false, &n));
    TEST_ASSERT_EQUAL_INT(D, n);
    /* ほかの状態では、mounted は関係ない(マウントできるのは ready だけ) */
    iso_state_t others[] = {U, I, F, V, D};
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        TEST_ASSERT_EQUAL_INT(ISO_TR_OK, tr(others[i], ISO_EV_DELETE, false, false, true, &n));
        TEST_ASSERT_EQUAL_INT(D, n);
    }
    /* イメージがなければ(NONE)削除できない */
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_STATE, tr(N, ISO_EV_DELETE, true, true, false, &n));
}

void test_null_ctx_means_all_flags_false(void) {
    iso_state_t n;
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_SIZE_MISMATCH, iso_state_transition(U, ISO_EV_FINISH, NULL, &n));
    TEST_ASSERT_EQUAL_INT(ISO_TR_REJECT_UPLOAD_IN_PROGRESS, iso_state_transition(I, ISO_EV_PUT, NULL, &n));
    TEST_ASSERT_EQUAL_INT(ISO_TR_OK, iso_state_transition(R, ISO_EV_DELETE, NULL, &n));
}

void test_state_meta_mapping(void) {
    TEST_ASSERT_EQUAL_INT(0, iso_state_to_meta(U));
    TEST_ASSERT_EQUAL_INT(1, iso_state_to_meta(R));
    TEST_ASSERT_EQUAL_INT(2, iso_state_to_meta(I));
    TEST_ASSERT_EQUAL_INT(3, iso_state_to_meta(V));
    TEST_ASSERT_EQUAL_INT(4, iso_state_to_meta(D));
    TEST_ASSERT_EQUAL_INT(-1, iso_state_to_meta(F)); /* finalizing は保存しない */
    TEST_ASSERT_EQUAL_INT(-1, iso_state_to_meta(N));
    TEST_ASSERT_EQUAL_INT(U, iso_state_from_meta(0));
    TEST_ASSERT_EQUAL_INT(R, iso_state_from_meta(1));
    TEST_ASSERT_EQUAL_INT(I, iso_state_from_meta(2));
    TEST_ASSERT_EQUAL_INT(V, iso_state_from_meta(3));
    TEST_ASSERT_EQUAL_INT(D, iso_state_from_meta(4));
    TEST_ASSERT_EQUAL_INT(N, iso_state_from_meta(5));
    TEST_ASSERT_EQUAL_INT(N, iso_state_from_meta(255));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_transition_table_every_cell);
    RUN_TEST(test_transition_rejection_leaves_next_equal_to_current);
    RUN_TEST(test_transition_next_may_be_null);
    RUN_TEST(test_transition_unknown_event_is_rejected);
    RUN_TEST(test_finish_rows_by_flags);
    RUN_TEST(test_put_rows_by_flags);
    RUN_TEST(test_delete_rows_by_flags);
    RUN_TEST(test_null_ctx_means_all_flags_false);
    RUN_TEST(test_state_meta_mapping);
    return UNITY_END();
}

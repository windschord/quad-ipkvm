/*
 * iso_media: 仮想 CD-ROM のメディアの状態（docs/ISO_MOUNT_DESIGN.md 5.5、docs/WEB_API.md 8.5 / 8.6）。
 * 規則ごとの単体テストと、独立に書いた参照モデルとの比較（固定シード）。テストの説明は docs/ISO_MEDIA_TESTS.md。
 */
#include <stdio.h>
#include <string.h>

#include "iso_media.h"
#include "unity.h"

#define IDA 0xA1B2C3D4u
#define IDB 0x0000BEEFu

static iso_media_t M;
static unsigned CB_CALLS;
static iso_media_t *CB_MEDIA_AT_CALL;
static bool CB_LOADED_AT_CALL;

static void on_force_eject(void *ctx) {
    (void)ctx;
    CB_CALLS++;
    CB_LOADED_AT_CALL = M.loaded; /* コールバックの時点で、状態はすでに EMPTY */
    CB_MEDIA_AT_CALL = &M;
}

void setUp(void) {
    CB_CALLS = 0;
    CB_MEDIA_AT_CALL = NULL;
    iso_media_init(&M, on_force_eject, NULL);
}
void tearDown(void) {}

static void take_all_ua(void) {
    uint8_t a, q;
    while (iso_media_take_unit_attention(&M, &a, &q)) {
    }
}

static void expect_ua(uint8_t asc, uint8_t ascq) {
    uint8_t a = 0xEE, q = 0xEE;
    TEST_ASSERT_TRUE(iso_media_take_unit_attention(&M, &a, &q));
    TEST_ASSERT_EQUAL_HEX8(asc, a);
    TEST_ASSERT_EQUAL_HEX8(ascq, q);
}

/* ---------------------------------------------------------------- 初期状態 */
static void test_init_is_empty_with_all_flags_clear(void) {
    iso_media_status_t st;
    memset(&M, 0xFF, sizeof(M));
    iso_media_init(&M, NULL, NULL);
    iso_media_get_status(&M, &st);
    TEST_ASSERT_FALSE(st.mounted);
    TEST_ASSERT_EQUAL_UINT32(0, st.id);
    TEST_ASSERT_FALSE(st.locked);
    TEST_ASSERT_FALSE(st.ejected_by_target);
    TEST_ASSERT_FALSE(iso_media_is_loaded(&M));
    TEST_ASSERT_FALSE(iso_media_prevent_removal(&M));
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(&M));
    TEST_ASSERT_EQUAL_UINT32(0, iso_media_abort_generation(&M));
    TEST_ASSERT_FALSE(iso_media_get_id(&M, NULL));
}

static void test_null_arguments_are_safe(void) {
    uint8_t a, q;
    iso_media_status_t st;
    memset(&st, 0xAA, sizeof(st));
    iso_media_init(NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_INVALID, iso_media_mount(NULL, IDA, false));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_INVALID, iso_media_eject(NULL, false));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_INVALID, iso_media_target_eject(NULL));
    TEST_ASSERT_FALSE(iso_media_set_prevent(NULL, true));
    TEST_ASSERT_FALSE(iso_media_request_eject(NULL));
    iso_media_bus_reset(NULL);
    iso_media_force_eject(NULL);
    iso_media_get_status(NULL, &st);
    TEST_ASSERT_FALSE(st.mounted);
    iso_media_get_status(&M, NULL);
    TEST_ASSERT_FALSE(iso_media_is_loaded(NULL));
    TEST_ASSERT_FALSE(iso_media_get_id(NULL, NULL));
    TEST_ASSERT_FALSE(iso_media_prevent_removal(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, iso_media_abort_generation(NULL));
    TEST_ASSERT_FALSE(iso_media_take_unit_attention(NULL, &a, &q));
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(NULL));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(NULL));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_take_event(NULL));
}

/* ---------------------------------------------------------------- mount */
static void test_mount_from_empty_sets_unit_attention_and_new_media_event(void) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    TEST_ASSERT_TRUE(iso_media_is_loaded(&M));
    TEST_ASSERT_TRUE(iso_media_get_id(&M, &id));
    TEST_ASSERT_EQUAL_HEX32(IDA, id);
    TEST_ASSERT_TRUE(iso_media_unit_attention_pending(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NEW_MEDIA, iso_media_peek_event(&M));
    TEST_ASSERT_FALSE(iso_media_prevent_removal(&M));
    expect_ua(0x28, 0x00); /* 1 回だけ */
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(&M));
    {
        uint8_t a, q;
        TEST_ASSERT_FALSE(iso_media_take_unit_attention(&M, &a, &q));
    }
}

static void test_mount_same_image_is_already_mounted_and_changes_nothing(void) {
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    take_all_ua();
    (void)iso_media_take_event(&M);
    TEST_ASSERT_TRUE(iso_media_set_prevent(&M, true));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_ALREADY_MOUNTED, iso_media_mount(&M, IDA, false));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_ALREADY_MOUNTED, iso_media_mount(&M, IDA, true)); /* force があっても何もしない */
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(&M));
    TEST_ASSERT_TRUE(iso_media_prevent_removal(&M)); /* 防止も保つ */
}

static void test_mount_replaces_the_image_and_reports_the_change_once(void) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    take_all_ua();
    (void)iso_media_take_event(&M);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDB, false));
    TEST_ASSERT_TRUE(iso_media_get_id(&M, &id));
    TEST_ASSERT_EQUAL_HEX32(IDB, id);
    expect_ua(0x28, 0x00);
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NEW_MEDIA, iso_media_take_event(&M));
}

static void test_mount_replacement_is_refused_while_prevented_unless_forced(void) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    take_all_ua();
    (void)iso_media_take_event(&M);
    TEST_ASSERT_TRUE(iso_media_set_prevent(&M, true));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_LOCKED, iso_media_mount(&M, IDB, false));
    TEST_ASSERT_TRUE(iso_media_get_id(&M, &id));
    TEST_ASSERT_EQUAL_HEX32(IDA, id);
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(&M)); /* 拒否では何も立てない */
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(&M));
    TEST_ASSERT_TRUE(iso_media_prevent_removal(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDB, true));
    TEST_ASSERT_TRUE(iso_media_get_id(&M, &id));
    TEST_ASSERT_EQUAL_HEX32(IDB, id);
    TEST_ASSERT_FALSE(iso_media_prevent_removal(&M)); /* force による差し替えで防止は解除 */
    TEST_ASSERT_TRUE(iso_media_unit_attention_pending(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NEW_MEDIA, iso_media_peek_event(&M));
}

static void test_mount_clears_ejected_by_target(void) {
    iso_media_status_t st;
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_target_eject(&M));
    iso_media_get_status(&M, &st);
    TEST_ASSERT_TRUE(st.ejected_by_target);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false)); /* 同じイメージでも、EMPTY なのでマウント */
    iso_media_get_status(&M, &st);
    TEST_ASSERT_FALSE(st.ejected_by_target);
    TEST_ASSERT_TRUE(st.mounted);
}

/* ---------------------------------------------------------------- Web の eject */
static void test_eject_removes_media_with_removal_event_but_without_unit_attention(void) {
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    take_all_ua();
    (void)iso_media_take_event(&M);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&M, false));
    TEST_ASSERT_FALSE(iso_media_is_loaded(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_MEDIA_REMOVAL, iso_media_peek_event(&M));
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(&M));
    TEST_ASSERT_FALSE(iso_media_get_id(&M, NULL));
}

static void test_eject_of_empty_is_idempotent_and_clears_ejected_by_target(void) {
    iso_media_status_t st;
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_NOT_MOUNTED, iso_media_eject(&M, false));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_target_eject(&M));
    iso_media_get_status(&M, &st);
    TEST_ASSERT_TRUE(st.ejected_by_target);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_NOT_MOUNTED, iso_media_eject(&M, false));
    iso_media_get_status(&M, &st);
    TEST_ASSERT_FALSE(st.ejected_by_target); /* WEB_API.md 8.6: ejected_by_target を false に戻す */
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_NOT_MOUNTED, iso_media_eject(&M, true));
}

static void test_eject_is_refused_while_prevented_unless_forced(void) {
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    take_all_ua();
    (void)iso_media_take_event(&M);
    TEST_ASSERT_TRUE(iso_media_set_prevent(&M, true));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_LOCKED, iso_media_eject(&M, false));
    TEST_ASSERT_TRUE(iso_media_is_loaded(&M));
    TEST_ASSERT_TRUE(iso_media_prevent_removal(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&M, true));
    TEST_ASSERT_FALSE(iso_media_is_loaded(&M));
    TEST_ASSERT_FALSE(iso_media_prevent_removal(&M)); /* force による取り外しで、防止は解除 */
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_MEDIA_REMOVAL, iso_media_peek_event(&M));
}

/* ---------------------------------------------------------------- ターゲットの START STOP UNIT */
static void test_target_eject_refused_when_prevented_and_does_not_mark_the_web_state(void) {
    iso_media_status_t st;
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    TEST_ASSERT_TRUE(iso_media_set_prevent(&M, true));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_LOCKED, iso_media_target_eject(&M));
    iso_media_get_status(&M, &st);
    TEST_ASSERT_TRUE(st.mounted);
    TEST_ASSERT_TRUE(st.locked);
    TEST_ASSERT_FALSE(st.ejected_by_target);
}

static void test_target_eject_marks_ejected_by_target_until_next_mount_or_eject(void) {
    iso_media_status_t st;
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    take_all_ua();
    (void)iso_media_take_event(&M);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_target_eject(&M));
    iso_media_get_status(&M, &st);
    TEST_ASSERT_FALSE(st.mounted);
    TEST_ASSERT_FALSE(st.locked);
    TEST_ASSERT_TRUE(st.ejected_by_target);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_MEDIA_REMOVAL, iso_media_peek_event(&M));
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(&M));
    /* 読み出し（GET）では消えない。バスリセット、force_eject、取り外し要求、防止の操作でも消えない */
    iso_media_get_status(&M, &st);
    TEST_ASSERT_TRUE(st.ejected_by_target);
    iso_media_bus_reset(&M);
    iso_media_force_eject(&M);
    TEST_ASSERT_FALSE(iso_media_request_eject(&M));
    TEST_ASSERT_TRUE(iso_media_set_prevent(&M, false));
    TEST_ASSERT_FALSE(iso_media_set_prevent(&M, true));
    iso_media_get_status(&M, &st);
    TEST_ASSERT_TRUE(st.ejected_by_target);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_NOT_MOUNTED, iso_media_target_eject(&M)); /* EMPTY への再度のイジェクト */
    iso_media_get_status(&M, &st);
    TEST_ASSERT_TRUE(st.ejected_by_target);
}

static void test_target_eject_of_empty_changes_nothing(void) {
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_NOT_MOUNTED, iso_media_target_eject(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(&M));
    {
        iso_media_status_t st;
        iso_media_get_status(&M, &st);
        TEST_ASSERT_FALSE(st.ejected_by_target);
    }
}

/* ---------------------------------------------------------------- prevent_removal */
static void test_removal_paths_clear_the_stored_id(void) {
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&M, false));
    TEST_ASSERT_EQUAL_HEX32(0, M.id);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_target_eject(&M));
    TEST_ASSERT_EQUAL_HEX32(0, M.id);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    iso_media_force_eject(&M);
    TEST_ASSERT_EQUAL_HEX32(0, M.id);
}

static void test_take_unit_attention_accepts_null_output_pointers(void) {
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    iso_media_bus_reset(&M);
    TEST_ASSERT_TRUE(iso_media_take_unit_attention(&M, NULL, NULL)); /* 29h */
    TEST_ASSERT_TRUE(iso_media_take_unit_attention(&M, NULL, NULL)); /* 28h */
    TEST_ASSERT_FALSE(iso_media_take_unit_attention(&M, NULL, NULL));
    TEST_ASSERT_TRUE(iso_media_get_id(&M, NULL));
}

static void test_set_prevent_on_empty_is_refused_and_allow_is_accepted(void) {
    TEST_ASSERT_FALSE(iso_media_set_prevent(&M, true));
    TEST_ASSERT_FALSE(iso_media_prevent_removal(&M));
    TEST_ASSERT_TRUE(iso_media_set_prevent(&M, false));
}

static void test_locked_in_the_web_view_requires_media(void) {
    iso_media_status_t st;
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    TEST_ASSERT_TRUE(iso_media_set_prevent(&M, true));
    iso_media_get_status(&M, &st);
    TEST_ASSERT_TRUE(st.locked);
    TEST_ASSERT_TRUE(st.mounted);
    TEST_ASSERT_EQUAL_HEX32(IDA, st.id);
    TEST_ASSERT_TRUE(iso_media_set_prevent(&M, false));
    iso_media_get_status(&M, &st);
    TEST_ASSERT_FALSE(st.locked);
}

/* ---------------------------------------------------------------- バスリセット */
static void test_bus_reset_clears_prevent_reserves_29h_and_aborts_but_keeps_the_media(void) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    take_all_ua();
    (void)iso_media_take_event(&M);
    TEST_ASSERT_TRUE(iso_media_set_prevent(&M, true));
    TEST_ASSERT_EQUAL_UINT32(0, iso_media_abort_generation(&M));
    iso_media_bus_reset(&M);
    TEST_ASSERT_FALSE(iso_media_prevent_removal(&M));
    TEST_ASSERT_TRUE(iso_media_is_loaded(&M));
    TEST_ASSERT_TRUE(iso_media_get_id(&M, &id));
    TEST_ASSERT_EQUAL_HEX32(IDA, id);
    TEST_ASSERT_EQUAL_UINT32(1, iso_media_abort_generation(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(&M));
    expect_ua(0x29, 0x00);
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(&M));
    TEST_ASSERT_EQUAL_INT(0, CB_CALLS); /* force_eject のコールバックは呼ばない */
    iso_media_bus_reset(&M);
    TEST_ASSERT_EQUAL_UINT32(2, iso_media_abort_generation(&M));
}

static void test_bus_reset_on_empty_still_reserves_29h(void) {
    iso_media_bus_reset(&M);
    TEST_ASSERT_FALSE(iso_media_is_loaded(&M));
    expect_ua(0x29, 0x00);
}

static void test_unit_attention_order_is_reset_before_media_change(void) {
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    iso_media_bus_reset(&M);
    expect_ua(0x29, 0x00);
    expect_ua(0x28, 0x00);
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(&M));
}

/* ---------------------------------------------------------------- force_eject（SD の再初期化） */
static void test_force_eject_ignores_prevent_and_notifies(void) {
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    take_all_ua();
    (void)iso_media_take_event(&M);
    TEST_ASSERT_TRUE(iso_media_set_prevent(&M, true));
    iso_media_force_eject(&M);
    TEST_ASSERT_FALSE(iso_media_is_loaded(&M));
    TEST_ASSERT_FALSE(iso_media_prevent_removal(&M));
    expect_ua(0x28, 0x00);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_MEDIA_REMOVAL, iso_media_peek_event(&M));
    TEST_ASSERT_EQUAL_UINT32(1, iso_media_abort_generation(&M));
    TEST_ASSERT_EQUAL_UINT(1, CB_CALLS);
    TEST_ASSERT_FALSE(CB_LOADED_AT_CALL); /* 通知の時点で、状態は更新済み */
    TEST_ASSERT_TRUE(CB_MEDIA_AT_CALL == &M);
    {
        iso_media_status_t st;
        iso_media_get_status(&M, &st);
        TEST_ASSERT_FALSE(st.ejected_by_target); /* Web の状態: mounted = null、ejected_by_target は立てない */
    }
}

static void test_force_eject_of_empty_notifies_but_raises_no_attention_or_event(void) {
    iso_media_force_eject(&M);
    TEST_ASSERT_EQUAL_UINT(1, CB_CALLS); /* epoch を進めるのは、グルーがこれを受けて行うので、必ず通知する */
    TEST_ASSERT_FALSE(iso_media_unit_attention_pending(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_peek_event(&M));
    TEST_ASSERT_EQUAL_UINT32(1, iso_media_abort_generation(&M));
    iso_media_force_eject(&M);
    TEST_ASSERT_EQUAL_UINT(2, CB_CALLS);
}

static void test_force_eject_without_callback_is_safe(void) {
    iso_media_init(&M, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    iso_media_force_eject(&M);
    TEST_ASSERT_FALSE(iso_media_is_loaded(&M));
}

static void *SEEN_CTX;
static void remember_ctx(void *ctx) { SEEN_CTX = ctx; }

static void test_force_eject_passes_the_registered_context(void) {
    static int token;
    SEEN_CTX = NULL;
    iso_media_init(&M, remember_ctx, &token);
    iso_media_force_eject(&M);
    TEST_ASSERT_TRUE(SEEN_CTX == &token);
}

/* ---------------------------------------------------------------- 取り外し要求のイベント */
static void test_request_eject_sets_the_event_only_when_loaded_and_nothing_stronger_is_pending(void) {
    TEST_ASSERT_FALSE(iso_media_request_eject(&M)); /* EMPTY */
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    TEST_ASSERT_FALSE(iso_media_request_eject(&M)); /* NEW_MEDIA が報告待ち */
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NEW_MEDIA, iso_media_peek_event(&M));
    (void)iso_media_take_event(&M);
    TEST_ASSERT_TRUE(iso_media_request_eject(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_EJECT_REQUEST, iso_media_take_event(&M));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NONE, iso_media_take_event(&M)); /* 1 回だけ */
    TEST_ASSERT_TRUE(iso_media_request_eject(&M));
    TEST_ASSERT_TRUE(iso_media_request_eject(&M)); /* 繰り返しても同じ */
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&M, false));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_MEDIA_REMOVAL, iso_media_peek_event(&M)); /* 取り外しが上書き */
    TEST_ASSERT_FALSE(iso_media_request_eject(&M)); /* EMPTY */
}

static void test_event_priority_latest_media_event_wins(void) {
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDA, false));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, iso_media_eject(&M, false));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_MEDIA_REMOVAL, iso_media_peek_event(&M)); /* NEW_MEDIA を上書き */
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, iso_media_mount(&M, IDB, false));
    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EVENT_NEW_MEDIA, iso_media_peek_event(&M)); /* REMOVAL を上書き */
}

/* ---------------------------------------------------------------- 参照モデルとの比較（固定シード） */
typedef struct {
    bool loaded;
    uint32_t id;
    bool prevent;
    bool ua_media, ua_reset;
    int event; /* 0 なし、1 取り外し要求、2 新メディア、3 取り外し */
    bool ejected_by_target;
    uint32_t gen;
    unsigned cb;
} model_t;

static uint64_t rs;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

static void check_equal(const model_t *m, int op, long step) {
    char msg[120];
    iso_media_status_t st;
    iso_media_get_status(&M, &st);
    snprintf(msg, sizeof(msg), "step %ld op %d", step, op);
    TEST_ASSERT_EQUAL_MESSAGE(m->loaded, M.loaded, msg);
    TEST_ASSERT_EQUAL_MESSAGE(m->loaded, st.mounted, msg);
    if (m->loaded) TEST_ASSERT_EQUAL_HEX32_MESSAGE(m->id, st.id, msg);
    TEST_ASSERT_EQUAL_MESSAGE(m->prevent, M.prevent_removal, msg);
    TEST_ASSERT_EQUAL_MESSAGE(m->loaded && m->prevent, st.locked, msg);
    TEST_ASSERT_EQUAL_MESSAGE(m->ua_media, M.unit_attention_media, msg);
    TEST_ASSERT_EQUAL_MESSAGE(m->ua_reset, M.unit_attention_reset, msg);
    TEST_ASSERT_EQUAL_MESSAGE(m->event, (int)M.event_pending, msg);
    TEST_ASSERT_EQUAL_MESSAGE(m->ejected_by_target, st.ejected_by_target, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(m->gen, iso_media_abort_generation(&M), msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(m->cb, CB_CALLS, msg);
    /* 不変条件: ejected_by_target ならば EMPTY。EMPTY のとき prevent は偽 */
    if (m->ejected_by_target) TEST_ASSERT_FALSE_MESSAGE(m->loaded, msg);
    if (!m->loaded) TEST_ASSERT_FALSE_MESSAGE(m->prevent, msg);
}

static void test_random_operations_match_an_independent_model(void) {
    model_t m;
    memset(&m, 0, sizeof(m));
    rs = 0x9E3779B97F4A7C15ull;
    for (long step = 0; step < 300000; step++) {
        uint32_t id = (rnd() & 1) ? IDA : IDB;
        bool force = (rnd() & 1) != 0;
        int op = (int)(rnd() % 11);
        switch (op) {
            case 0: { /* mount */
                iso_media_result_t r = iso_media_mount(&M, id, force);
                if (m.loaded && m.id == id) {
                    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_ALREADY_MOUNTED, r);
                } else if (m.loaded && m.prevent && !force) {
                    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_LOCKED, r);
                } else {
                    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_MOUNTED, r);
                    m.loaded = true;
                    m.id = id;
                    m.prevent = false;
                    m.ua_media = true;
                    m.event = 2;
                    m.ejected_by_target = false;
                }
                break;
            }
            case 1: { /* Web eject */
                iso_media_result_t r = iso_media_eject(&M, force);
                if (!m.loaded) {
                    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_NOT_MOUNTED, r);
                    m.ejected_by_target = false;
                } else if (m.prevent && !force) {
                    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_LOCKED, r);
                } else {
                    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, r);
                    m.loaded = false;
                    m.prevent = false;
                    m.event = 3;
                    m.ejected_by_target = false;
                }
                break;
            }
            case 2: { /* ターゲットのイジェクト */
                iso_media_result_t r = iso_media_target_eject(&M);
                if (!m.loaded) {
                    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_NOT_MOUNTED, r);
                } else if (m.prevent) {
                    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_LOCKED, r);
                } else {
                    TEST_ASSERT_EQUAL_INT(ISO_MEDIA_EJECTED, r);
                    m.loaded = false;
                    m.event = 3;
                    m.ejected_by_target = true;
                }
                break;
            }
            case 3: { /* PREVENT */
                bool want = (rnd() & 1) != 0;
                bool r = iso_media_set_prevent(&M, want);
                if (want && !m.loaded) {
                    TEST_ASSERT_FALSE(r);
                } else {
                    TEST_ASSERT_TRUE(r);
                    m.prevent = want;
                }
                break;
            }
            case 4: /* バスリセット */
                iso_media_bus_reset(&M);
                m.prevent = false;
                m.ua_reset = true;
                m.gen++;
                break;
            case 5: /* force_eject */
                iso_media_force_eject(&M);
                if (m.loaded) {
                    m.loaded = false;
                    m.ua_media = true;
                    m.event = 3;
                }
                m.prevent = false;
                m.gen++;
                m.cb++;
                break;
            case 6: { /* 取り外し要求 */
                bool r = iso_media_request_eject(&M);
                if (m.loaded && m.event != 2 && m.event != 3) {
                    TEST_ASSERT_TRUE(r);
                    m.event = 1;
                } else {
                    TEST_ASSERT_FALSE(r);
                }
                break;
            }
            case 7: { /* UA を取る */
                uint8_t a = 0, q = 0;
                bool r = iso_media_take_unit_attention(&M, &a, &q);
                if (m.ua_reset) {
                    TEST_ASSERT_TRUE(r);
                    TEST_ASSERT_EQUAL_HEX8(0x29, a);
                    TEST_ASSERT_EQUAL_HEX8(0x00, q);
                    m.ua_reset = false;
                } else if (m.ua_media) {
                    TEST_ASSERT_TRUE(r);
                    TEST_ASSERT_EQUAL_HEX8(0x28, a);
                    TEST_ASSERT_EQUAL_HEX8(0x00, q);
                    m.ua_media = false;
                } else {
                    TEST_ASSERT_FALSE(r);
                }
                break;
            }
            case 8: { /* イベントを取る */
                TEST_ASSERT_EQUAL_INT(m.event, (int)iso_media_take_event(&M));
                m.event = 0;
                break;
            }
            case 9: /* 読み出しだけ（状態を変えない） */
                TEST_ASSERT_EQUAL_INT(m.event, (int)iso_media_peek_event(&M));
                TEST_ASSERT_EQUAL_INT(m.ua_media || m.ua_reset, iso_media_unit_attention_pending(&M));
                {
                    uint32_t got = 0;
                    TEST_ASSERT_EQUAL_INT(m.loaded, iso_media_get_id(&M, &got));
                    if (m.loaded) TEST_ASSERT_EQUAL_HEX32(m.id, got);
                }
                break;
            default: { /* 時々 UA / イベントを全部消す（EMPTY / LOADED のまま長く動かす） */
                uint8_t a, q;
                while (iso_media_take_unit_attention(&M, &a, &q)) {
                }
                (void)iso_media_take_event(&M);
                m.ua_media = m.ua_reset = false;
                m.event = 0;
                break;
            }
        }
        check_equal(&m, op, step);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_is_empty_with_all_flags_clear);
    RUN_TEST(test_null_arguments_are_safe);
    RUN_TEST(test_mount_from_empty_sets_unit_attention_and_new_media_event);
    RUN_TEST(test_mount_same_image_is_already_mounted_and_changes_nothing);
    RUN_TEST(test_mount_replaces_the_image_and_reports_the_change_once);
    RUN_TEST(test_mount_replacement_is_refused_while_prevented_unless_forced);
    RUN_TEST(test_mount_clears_ejected_by_target);
    RUN_TEST(test_eject_removes_media_with_removal_event_but_without_unit_attention);
    RUN_TEST(test_eject_of_empty_is_idempotent_and_clears_ejected_by_target);
    RUN_TEST(test_eject_is_refused_while_prevented_unless_forced);
    RUN_TEST(test_target_eject_refused_when_prevented_and_does_not_mark_the_web_state);
    RUN_TEST(test_target_eject_marks_ejected_by_target_until_next_mount_or_eject);
    RUN_TEST(test_target_eject_of_empty_changes_nothing);
    RUN_TEST(test_removal_paths_clear_the_stored_id);
    RUN_TEST(test_take_unit_attention_accepts_null_output_pointers);
    RUN_TEST(test_set_prevent_on_empty_is_refused_and_allow_is_accepted);
    RUN_TEST(test_locked_in_the_web_view_requires_media);
    RUN_TEST(test_bus_reset_clears_prevent_reserves_29h_and_aborts_but_keeps_the_media);
    RUN_TEST(test_bus_reset_on_empty_still_reserves_29h);
    RUN_TEST(test_unit_attention_order_is_reset_before_media_change);
    RUN_TEST(test_force_eject_ignores_prevent_and_notifies);
    RUN_TEST(test_force_eject_of_empty_notifies_but_raises_no_attention_or_event);
    RUN_TEST(test_force_eject_without_callback_is_safe);
    RUN_TEST(test_force_eject_passes_the_registered_context);
    RUN_TEST(test_request_eject_sets_the_event_only_when_loaded_and_nothing_stronger_is_pending);
    RUN_TEST(test_event_priority_latest_media_event_wins);
    RUN_TEST(test_random_operations_match_an_independent_model);
    return UNITY_END();
}

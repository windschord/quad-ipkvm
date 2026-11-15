/* video_status: 提供元の登録・取り替え・既定、値の検査（範囲外・不整合）、out == NULL */
#include "unity.h"
#include "video_status.h"
#include <stdio.h>
#include <string.h>

void setUp(void) { video_status_set_provider(NULL, NULL); }
void tearDown(void) { video_status_set_provider(NULL, NULL); }

#define SENTINEL 0xA5

static video_status_t get(void) {
    video_status_t st;
    memset(&st, SENTINEL, sizeof(st));
    video_status_get(&st);
    return st;
}

static void expect_status(kvm_video_sync_state_t s, uint32_t w, uint32_t h, video_status_t got) {
    TEST_ASSERT_EQUAL_INT(s, got.state);
    TEST_ASSERT_EQUAL_UINT32(w, got.width);
    TEST_ASSERT_EQUAL_UINT32(h, got.height);
}

/* ---- 既定（提供元なし） ---- */

void test_default_is_unknown_zero_zero(void) {
    expect_status(KVM_VIDEO_SYNC_UNKNOWN, 0, 0, get());
}

void test_default_hal_sync_is_unknown(void) {
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_UNKNOWN, video_status_hal_sync(NULL));
}

/* ---- 登録・取り替え・NULL に戻す ---- */

void test_registered_provider_is_called_with_its_ctx(void) {
    video_status_fake_t f;
    video_status_fake_init(&f);
    video_status_fake_set(&f, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    video_status_set_provider(video_status_fake_provider, &f);
    expect_status(KVM_VIDEO_SYNC_LOCKED, 1920, 1080, get());
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_LOCKED, video_status_hal_sync(NULL));
}

typedef struct {
    int calls;
    void *seen_ctx;
    video_status_t reply;
} spy_t;

static void spy_provider(void *ctx, video_status_t *out) {
    spy_t *s = (spy_t *)ctx;
    s->calls++;
    s->seen_ctx = ctx;
    *out = s->reply;
}

void test_provider_is_called_once_per_get_and_receives_registered_ctx(void) {
    spy_t s;
    memset(&s, 0, sizeof(s));
    s.reply.state = KVM_VIDEO_SYNC_NO_SIGNAL;
    video_status_set_provider(spy_provider, &s);
    TEST_ASSERT_EQUAL(0, s.calls); /* 登録しただけでは呼ばない */
    (void)get();
    TEST_ASSERT_EQUAL(1, s.calls);
    TEST_ASSERT_EQUAL_PTR(&s, s.seen_ctx);
    (void)video_status_hal_sync(NULL);
    TEST_ASSERT_EQUAL(2, s.calls);
}

void test_provider_can_be_replaced(void) {
    video_status_fake_t a, b;
    video_status_fake_init(&a);
    video_status_fake_init(&b);
    video_status_fake_set(&a, KVM_VIDEO_SYNC_LOCKED, 640, 480);
    video_status_fake_set(&b, KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    video_status_set_provider(video_status_fake_provider, &a);
    expect_status(KVM_VIDEO_SYNC_LOCKED, 640, 480, get());
    video_status_set_provider(video_status_fake_provider, &b);
    expect_status(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0, get());
}

void test_null_provider_restores_default(void) {
    video_status_fake_t f;
    video_status_fake_init(&f);
    video_status_fake_set(&f, KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    video_status_set_provider(video_status_fake_provider, &f);
    expect_status(KVM_VIDEO_SYNC_LOCKED, 1280, 720, get());
    video_status_set_provider(NULL, &f); /* fn が NULL なら ctx は捨てる */
    expect_status(KVM_VIDEO_SYNC_UNKNOWN, 0, 0, get());
}

void test_null_fn_discards_ctx_so_a_later_fn_without_ctx_sees_null(void) {
    spy_t s;
    memset(&s, 0, sizeof(s));
    s.reply.state = KVM_VIDEO_SYNC_NO_SIGNAL;
    video_status_set_provider(spy_provider, &s);
    video_status_set_provider(NULL, &s);
    /* ctx を引き継いでいたら、NULL の ctx を想定した提供元が s を読んでしまう。ここでは再登録の ctx が使われる */
    video_status_fake_t f;
    video_status_fake_init(&f);
    video_status_fake_set(&f, KVM_VIDEO_SYNC_LOCKED, 800, 600);
    video_status_set_provider(video_status_fake_provider, &f);
    expect_status(KVM_VIDEO_SYNC_LOCKED, 800, 600, get());
    TEST_ASSERT_EQUAL(0, s.calls);
}

static void ctx_null_probe(void *ctx, video_status_t *out) {
    out->state = ctx == NULL ? KVM_VIDEO_SYNC_NO_SIGNAL : KVM_VIDEO_SYNC_LOCKED;
    out->width = ctx == NULL ? 0 : 10;
    out->height = ctx == NULL ? 0 : 10;
}

void test_provider_registered_with_null_ctx_gets_null(void) {
    int dummy = 0;
    video_status_set_provider(ctx_null_probe, NULL);
    expect_status(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0, get());
    video_status_set_provider(ctx_null_probe, &dummy);
    expect_status(KVM_VIDEO_SYNC_LOCKED, 10, 10, get());
}

/* ---- 提供元が out を書かない ---- */

static void silent_provider(void *ctx, video_status_t *out) {
    (void)ctx;
    (void)out;
}

void test_provider_that_does_not_write_yields_unknown(void) {
    video_status_set_provider(silent_provider, NULL);
    expect_status(KVM_VIDEO_SYNC_UNKNOWN, 0, 0, get());
}

/* ---- 値の検査 ---- */

static video_status_t get_with(kvm_video_sync_state_t s, uint32_t w, uint32_t h) {
    video_status_fake_t f;
    video_status_fake_init(&f);
    video_status_fake_set(&f, s, w, h);
    video_status_set_provider(video_status_fake_provider, &f);
    video_status_t st = get();
    video_status_set_provider(NULL, NULL);
    return st;
}

void test_valid_values_pass_through(void) {
    expect_status(KVM_VIDEO_SYNC_UNKNOWN, 0, 0, get_with(KVM_VIDEO_SYNC_UNKNOWN, 0, 0));
    expect_status(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0, get_with(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0));
    expect_status(KVM_VIDEO_SYNC_LOCKED, 1920, 1080, get_with(KVM_VIDEO_SYNC_LOCKED, 1920, 1080));
    expect_status(KVM_VIDEO_SYNC_LOCKED, 1, 1, get_with(KVM_VIDEO_SYNC_LOCKED, 1, 1));
    expect_status(KVM_VIDEO_SYNC_LOCKED, 0xFFFFFFFFu, 0xFFFFFFFFu, get_with(KVM_VIDEO_SYNC_LOCKED, 0xFFFFFFFFu, 0xFFFFFFFFu));
}

void test_out_of_range_state_becomes_unknown_with_zero_size(void) {
    const uint32_t bad[] = {3, 4, 99, 0x7FFFFFFFu, 0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char msg[40];
        snprintf(msg, sizeof(msg), "state=%u", (unsigned)bad[i]);
        video_status_t st = get_with((kvm_video_sync_state_t)bad[i], 1920, 1080);
        TEST_ASSERT_EQUAL_INT_MESSAGE(KVM_VIDEO_SYNC_UNKNOWN, st.state, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, st.width, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, st.height, msg);
    }
}

void test_locked_with_zero_width_or_height_becomes_unknown(void) {
    expect_status(KVM_VIDEO_SYNC_UNKNOWN, 0, 0, get_with(KVM_VIDEO_SYNC_LOCKED, 0, 0));
    expect_status(KVM_VIDEO_SYNC_UNKNOWN, 0, 0, get_with(KVM_VIDEO_SYNC_LOCKED, 0, 1080));
    expect_status(KVM_VIDEO_SYNC_UNKNOWN, 0, 0, get_with(KVM_VIDEO_SYNC_LOCKED, 1920, 0));
}

void test_non_locked_states_get_zero_size(void) {
    expect_status(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0, get_with(KVM_VIDEO_SYNC_NO_SIGNAL, 1920, 1080));
    expect_status(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0, get_with(KVM_VIDEO_SYNC_NO_SIGNAL, 1920, 0));
    expect_status(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0, get_with(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 1080));
    expect_status(KVM_VIDEO_SYNC_UNKNOWN, 0, 0, get_with(KVM_VIDEO_SYNC_UNKNOWN, 1920, 1080));
    expect_status(KVM_VIDEO_SYNC_UNKNOWN, 0, 0, get_with(KVM_VIDEO_SYNC_UNKNOWN, 1920, 0));
    expect_status(KVM_VIDEO_SYNC_UNKNOWN, 0, 0, get_with(KVM_VIDEO_SYNC_UNKNOWN, 0, 1080));
}

void test_hal_sync_returns_the_checked_state(void) {
    video_status_fake_t f;
    video_status_fake_init(&f);
    video_status_set_provider(video_status_fake_provider, &f);

    video_status_fake_set(&f, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_LOCKED, video_status_hal_sync(NULL));
    video_status_fake_set(&f, KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_NO_SIGNAL, video_status_hal_sync(NULL));
    video_status_fake_set(&f, KVM_VIDEO_SYNC_UNKNOWN, 0, 0);
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_UNKNOWN, video_status_hal_sync(NULL));
    /* 不整合な値は、検査後の state（UNKNOWN）を返す */
    video_status_fake_set(&f, KVM_VIDEO_SYNC_LOCKED, 0, 0);
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_UNKNOWN, video_status_hal_sync(NULL));
    video_status_fake_set(&f, (kvm_video_sync_state_t)77, 1, 1);
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_UNKNOWN, video_status_hal_sync(NULL));
    /* ctx は使わない */
    int dummy;
    video_status_fake_set(&f, KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_NO_SIGNAL, video_status_hal_sync(&dummy));
}

/* ---- out == NULL ---- */

void test_get_with_null_out_does_not_crash_and_does_not_call_provider(void) {
    spy_t s;
    memset(&s, 0, sizeof(s));
    s.reply.state = KVM_VIDEO_SYNC_NO_SIGNAL;
    video_status_get(NULL); /* 提供元なし */
    video_status_set_provider(spy_provider, &s);
    video_status_get(NULL);
    TEST_ASSERT_EQUAL(0, s.calls);
}

/* get は out の全フィールドを書く（呼び出し側の古い値を残さない） */
void test_get_overwrites_every_field(void) {
    video_status_t st;
    st.state = KVM_VIDEO_SYNC_LOCKED;
    st.width = 123;
    st.height = 456;
    video_status_get(&st); /* 提供元なし */
    expect_status(KVM_VIDEO_SYNC_UNKNOWN, 0, 0, st);

    st.state = KVM_VIDEO_SYNC_LOCKED;
    st.width = 123;
    st.height = 456;
    video_status_set_provider(silent_provider, NULL);
    video_status_get(&st);
    expect_status(KVM_VIDEO_SYNC_UNKNOWN, 0, 0, st);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_is_unknown_zero_zero);
    RUN_TEST(test_default_hal_sync_is_unknown);
    RUN_TEST(test_registered_provider_is_called_with_its_ctx);
    RUN_TEST(test_provider_is_called_once_per_get_and_receives_registered_ctx);
    RUN_TEST(test_provider_can_be_replaced);
    RUN_TEST(test_null_provider_restores_default);
    RUN_TEST(test_null_fn_discards_ctx_so_a_later_fn_without_ctx_sees_null);
    RUN_TEST(test_provider_registered_with_null_ctx_gets_null);
    RUN_TEST(test_provider_that_does_not_write_yields_unknown);
    RUN_TEST(test_valid_values_pass_through);
    RUN_TEST(test_out_of_range_state_becomes_unknown_with_zero_size);
    RUN_TEST(test_locked_with_zero_width_or_height_becomes_unknown);
    RUN_TEST(test_non_locked_states_get_zero_size);
    RUN_TEST(test_hal_sync_returns_the_checked_state);
    RUN_TEST(test_get_with_null_out_does_not_crash_and_does_not_call_provider);
    RUN_TEST(test_get_overwrites_every_field);
    return UNITY_END();
}

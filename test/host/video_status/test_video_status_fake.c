/* video_status の偽の提供元（video_status_fake_t）: 初期化、設定、提供元としての読み出し、全遷移、NULL の扱い */
#include "unity.h"
#include "video_status.h"
#include <stdio.h>
#include <string.h>

void setUp(void) { video_status_set_provider(NULL, NULL); }
void tearDown(void) { video_status_set_provider(NULL, NULL); }

static void expect_raw(const video_status_t *p, kvm_video_sync_state_t s, uint32_t w, uint32_t h) {
    TEST_ASSERT_EQUAL_INT(s, p->state);
    TEST_ASSERT_EQUAL_UINT32(w, p->width);
    TEST_ASSERT_EQUAL_UINT32(h, p->height);
}

void test_fake_init_is_unknown_zero_zero(void) {
    video_status_fake_t f;
    memset(&f, 0xFF, sizeof(f));
    video_status_fake_init(&f);
    expect_raw(&f.status, KVM_VIDEO_SYNC_UNKNOWN, 0, 0);
}

void test_fake_set_stores_all_three_fields(void) {
    video_status_fake_t f;
    video_status_fake_init(&f);
    video_status_fake_set(&f, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    expect_raw(&f.status, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    video_status_fake_set(&f, KVM_VIDEO_SYNC_NO_SIGNAL, 7, 9);
    expect_raw(&f.status, KVM_VIDEO_SYNC_NO_SIGNAL, 7, 9); /* 偽の提供元は検査しない（そのまま保持する） */
}

void test_fake_provider_returns_the_stored_value_unchecked(void) {
    video_status_fake_t f;
    video_status_fake_init(&f);
    video_status_fake_set(&f, (kvm_video_sync_state_t)42, 3, 4);
    video_status_t out;
    memset(&out, 0, sizeof(out));
    video_status_fake_provider(&f, &out);
    expect_raw(&out, (kvm_video_sync_state_t)42, 3, 4);
}

/* 3 つの状態の全遷移（自分自身を含む 9 通り）を、provider 経由で get して確かめる */
void test_fake_all_state_transitions_through_video_status_get(void) {
    const kvm_video_sync_state_t states[3] = {KVM_VIDEO_SYNC_UNKNOWN, KVM_VIDEO_SYNC_NO_SIGNAL, KVM_VIDEO_SYNC_LOCKED};
    video_status_fake_t f;
    video_status_fake_init(&f);
    video_status_set_provider(video_status_fake_provider, &f);
    for (int from = 0; from < 3; from++) {
        for (int to = 0; to < 3; to++) {
            char msg[32];
            snprintf(msg, sizeof(msg), "%d -> %d", from, to);
            video_status_fake_set(&f, states[from], states[from] == KVM_VIDEO_SYNC_LOCKED ? 1920 : 0, states[from] == KVM_VIDEO_SYNC_LOCKED ? 1080 : 0);
            video_status_t a;
            video_status_get(&a);
            TEST_ASSERT_EQUAL_INT_MESSAGE(states[from], a.state, msg);

            uint32_t w = states[to] == KVM_VIDEO_SYNC_LOCKED ? 1280 : 0;
            uint32_t h = states[to] == KVM_VIDEO_SYNC_LOCKED ? 720 : 0;
            video_status_fake_set(&f, states[to], w, h);
            video_status_t b;
            video_status_get(&b);
            TEST_ASSERT_EQUAL_INT_MESSAGE(states[to], b.state, msg);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(w, b.width, msg);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(h, b.height, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(states[to], video_status_hal_sync(NULL), msg);
        }
    }
}

void test_fake_resolution_changes_while_locked(void) {
    video_status_fake_t f;
    video_status_fake_init(&f);
    video_status_set_provider(video_status_fake_provider, &f);
    video_status_fake_set(&f, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    video_status_t st;
    video_status_get(&st);
    expect_raw(&st, KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    video_status_fake_set(&f, KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    video_status_get(&st);
    expect_raw(&st, KVM_VIDEO_SYNC_LOCKED, 1280, 720);
}

void test_fake_provider_with_null_ctx_is_unknown(void) {
    video_status_t out;
    out.state = KVM_VIDEO_SYNC_LOCKED;
    out.width = 5;
    out.height = 6;
    video_status_fake_provider(NULL, &out);
    expect_raw(&out, KVM_VIDEO_SYNC_UNKNOWN, 0, 0);
    /* 登録の ctx が NULL でも、video_status_get は UNKNOWN を返す */
    video_status_set_provider(video_status_fake_provider, NULL);
    video_status_get(&out);
    expect_raw(&out, KVM_VIDEO_SYNC_UNKNOWN, 0, 0);
}

void test_fake_functions_accept_null_without_crashing(void) {
    video_status_fake_init(NULL);
    video_status_fake_set(NULL, KVM_VIDEO_SYNC_LOCKED, 1, 1);
    video_status_fake_t f;
    video_status_fake_init(&f);
    video_status_fake_set(&f, KVM_VIDEO_SYNC_LOCKED, 1, 1);
    video_status_fake_provider(&f, NULL);
    expect_raw(&f.status, KVM_VIDEO_SYNC_LOCKED, 1, 1); /* out == NULL は何も壊さない */
}

/* 2 つの偽の提供元は独立（静的な共有状態を持たない） */
void test_two_fakes_are_independent(void) {
    video_status_fake_t a, b;
    video_status_fake_init(&a);
    video_status_fake_init(&b);
    video_status_fake_set(&a, KVM_VIDEO_SYNC_LOCKED, 640, 480);
    expect_raw(&b.status, KVM_VIDEO_SYNC_UNKNOWN, 0, 0);
    video_status_t out;
    video_status_fake_provider(&b, &out);
    expect_raw(&out, KVM_VIDEO_SYNC_UNKNOWN, 0, 0);
    video_status_fake_provider(&a, &out);
    expect_raw(&out, KVM_VIDEO_SYNC_LOCKED, 640, 480);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fake_init_is_unknown_zero_zero);
    RUN_TEST(test_fake_set_stores_all_three_fields);
    RUN_TEST(test_fake_provider_returns_the_stored_value_unchecked);
    RUN_TEST(test_fake_all_state_transitions_through_video_status_get);
    RUN_TEST(test_fake_resolution_changes_while_locked);
    RUN_TEST(test_fake_provider_with_null_ctx_is_unknown);
    RUN_TEST(test_fake_functions_accept_null_without_crashing);
    RUN_TEST(test_two_fakes_are_independent);
    return UNITY_END();
}

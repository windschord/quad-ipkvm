#include "unity.h"
#include "http_guard.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_origin_valid(void) {
    const char *origin = "https://192.168.1.100";
    const char *host = "192.168.1.100";
    TEST_ASSERT_TRUE(http_guard_origin_ok(origin, strlen(origin), host, strlen(host)));

    origin = "https://mY-KvM.LoCaL";
    host = "My-kVm.lOcAl";
    TEST_ASSERT_TRUE(http_guard_origin_ok(origin, strlen(origin), host, strlen(host)));
}

void test_origin_valid_with_port(void) {
    const char *origin = "https://192.168.1.100:8443";
    const char *host = "192.168.1.100:8443";
    TEST_ASSERT_TRUE(http_guard_origin_ok(origin, strlen(origin), host, strlen(host)));
}

void test_origin_reject_http(void) {
    const char *origin = "http://192.168.1.100";
    const char *host = "192.168.1.100";
    TEST_ASSERT_FALSE(http_guard_origin_ok(origin, strlen(origin), host, strlen(host)));
}

void test_origin_reject_mismatch(void) {
    const char *origin = "https://evil.com";
    const char *host = "192.168.1.100";
    TEST_ASSERT_FALSE(http_guard_origin_ok(origin, strlen(origin), host, strlen(host)));
}

void test_origin_reject_null(void) {
    const char *origin = "null";
    const char *host = "192.168.1.100";
    TEST_ASSERT_FALSE(http_guard_origin_ok(origin, strlen(origin), host, strlen(host)));
}

void test_origin_reject_userinfo(void) {
    const char *origin = "https://admin@192.168.1.100";
    const char *host = "192.168.1.100";
    TEST_ASSERT_FALSE(http_guard_origin_ok(origin, strlen(origin), host, strlen(host)));
}

void test_origin_reject_trailing_dot(void) {
    const char *origin = "https://example.com.";
    const char *host = "example.com.";
    TEST_ASSERT_FALSE(http_guard_origin_ok(origin, strlen(origin), host, strlen(host)));
}

void test_origin_reject_ipv6(void) {
    const char *origin = "https://[::1]";
    const char *host = "[::1]";
    TEST_ASSERT_FALSE(http_guard_origin_ok(origin, strlen(origin), host, strlen(host)));
}

void test_origin_reject_prefix_suffix(void) {
    const char *host = "example.com";
    size_t hlen = strlen(host);

    const char *o1 = "https://example.com.evil.com";
    TEST_ASSERT_FALSE(http_guard_origin_ok(o1, strlen(o1), host, hlen));

    const char *o2 = "https://example.comX";
    TEST_ASSERT_FALSE(http_guard_origin_ok(o2, strlen(o2), host, hlen));

    const char *o3 = "https://example.com:8443";
    TEST_ASSERT_FALSE(http_guard_origin_ok(o3, strlen(o3), host, hlen));

    const char *o4 = "https://example.co";
    TEST_ASSERT_FALSE(http_guard_origin_ok(o4, strlen(o4), host, hlen));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_origin_valid);
    RUN_TEST(test_origin_valid_with_port);
    RUN_TEST(test_origin_reject_http);
    RUN_TEST(test_origin_reject_mismatch);
    RUN_TEST(test_origin_reject_null);
    RUN_TEST(test_origin_reject_userinfo);
    RUN_TEST(test_origin_reject_trailing_dot);
    RUN_TEST(test_origin_reject_ipv6);
    RUN_TEST(test_origin_reject_prefix_suffix);
    return UNITY_END();
}

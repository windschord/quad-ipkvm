#include "unity.h"
#include "http_guard.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_host_ipv4(void) {
    const char *allowed[] = {"my-kvm.local"};
    TEST_ASSERT_TRUE(http_guard_host_ok("192.168.1.100", 13, allowed, 1));
    TEST_ASSERT_TRUE(http_guard_host_ok("0.0.0.0", 7, allowed, 1));
    TEST_ASSERT_TRUE(http_guard_host_ok("255.255.255.255", 15, allowed, 1));

    // IPv4 with port
    TEST_ASSERT_TRUE(http_guard_host_ok("192.168.1.100:8443", 18, allowed, 1));
}

void test_host_ipv4_invalid(void) {
    const char *allowed[] = {"my-kvm.local"};
    TEST_ASSERT_FALSE(http_guard_host_ok("256.168.1.100", 13, allowed, 1));
    TEST_ASSERT_FALSE(http_guard_host_ok("192.168.1.01", 12, allowed, 1)); // Leading zero
    TEST_ASSERT_FALSE(http_guard_host_ok("192.168.1", 9, allowed, 1));
    TEST_ASSERT_FALSE(http_guard_host_ok("192.168.1.100.", 14, allowed, 1));
}

void test_host_allowed_names(void) {
    const char *allowed[] = {"my-kvm.local", "example.com"};
    TEST_ASSERT_TRUE(http_guard_host_ok("my-kvm.local", 12, allowed, 2));
    TEST_ASSERT_TRUE(http_guard_host_ok("My-kVM.LOcal", 12, allowed, 2)); // Case insensitive
    TEST_ASSERT_TRUE(http_guard_host_ok("example.com:443", 15, allowed, 2)); // With port
}

void test_host_rejected(void) {
    const char *allowed[] = {"my-kvm.local"};
    TEST_ASSERT_FALSE(http_guard_host_ok("evil.com", 8, allowed, 1));
    TEST_ASSERT_FALSE(http_guard_host_ok("my-kvm.local.evil.com", 21, allowed, 1));
    TEST_ASSERT_FALSE(http_guard_host_ok("[::1]", 5, allowed, 1));
    TEST_ASSERT_FALSE(http_guard_host_ok("admin@my-kvm.local", 18, allowed, 1));
    TEST_ASSERT_FALSE(http_guard_host_ok("my-kvm.local.", 13, allowed, 1));
    TEST_ASSERT_FALSE(http_guard_host_ok("", 0, allowed, 1));
}

void test_host_ports(void) {
    const char *allowed[] = {"example.com"};
    // valid
    TEST_ASSERT_TRUE(http_guard_host_ok("example.com:65535", 17, allowed, 1));
    TEST_ASSERT_TRUE(http_guard_host_ok("example.com:80", 14, allowed, 1));

    // invalid
    TEST_ASSERT_FALSE(http_guard_host_ok("example.com:0", 13, allowed, 1));
    TEST_ASSERT_FALSE(http_guard_host_ok("example.com:65536", 17, allowed, 1));
    TEST_ASSERT_FALSE(http_guard_host_ok("example.com:99999", 17, allowed, 1));
    TEST_ASSERT_FALSE(http_guard_host_ok("example.com:00080", 17, allowed, 1));
    TEST_ASSERT_FALSE(http_guard_host_ok("example.com:01", 14, allowed, 1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_host_ipv4);
    RUN_TEST(test_host_ipv4_invalid);
    RUN_TEST(test_host_allowed_names);
    RUN_TEST(test_host_rejected);
    RUN_TEST(test_host_ports);
    return UNITY_END();
}

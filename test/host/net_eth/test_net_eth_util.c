#include "unity.h"
#include "net_eth_util.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_net_eth_ip4_to_str(void)
{
    char buf[16];

    // 0.0.0.0
    TEST_ASSERT_TRUE(net_eth_ip4_to_str(0x00000000, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("0.0.0.0", buf);

    // 255.255.255.255
    TEST_ASSERT_TRUE(net_eth_ip4_to_str(0xFFFFFFFF, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("255.255.255.255", buf);

    // 192.168.1.10
    // Little-endian: 10 is highest byte, 192 is lowest byte
    // 0x0A01A8C0 -> 10.1.168.192 in memory, but 192 is the first octet
    TEST_ASSERT_TRUE(net_eth_ip4_to_str(0x0A01A8C0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("192.168.1.10", buf);

    // Null buffer
    TEST_ASSERT_FALSE(net_eth_ip4_to_str(0xFFFFFFFF, NULL, sizeof(buf)));
}

void test_net_eth_ip4_to_str_boundary(void)
{
    char buf[16];

    // Exactly 16 bytes is enough for "255.255.255.255" + null terminator
    TEST_ASSERT_TRUE(net_eth_ip4_to_str(0xFFFFFFFF, buf, 16));

    // 15 bytes is not enough (should return false)
    TEST_ASSERT_FALSE(net_eth_ip4_to_str(0xFFFFFFFF, buf, 15));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_net_eth_ip4_to_str);
    RUN_TEST(test_net_eth_ip4_to_str_boundary);
    return UNITY_END();
}

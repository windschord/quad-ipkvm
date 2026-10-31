#include "unity.h"
#include "mipi_phy_power.h"

void setUp(void) {}
void tearDown(void) {}

void test_mipi_phy_power_validate_valid(void) {
    TEST_ASSERT_EQUAL(0, mipi_phy_power_validate(3, 2250));
    TEST_ASSERT_EQUAL(0, mipi_phy_power_validate(3, 2500));
    TEST_ASSERT_EQUAL(0, mipi_phy_power_validate(3, 2750));
}

void test_mipi_phy_power_validate_invalid_chan(void) {
    TEST_ASSERT_LESS_THAN(0, mipi_phy_power_validate(0, 2500));
    TEST_ASSERT_LESS_THAN(0, mipi_phy_power_validate(2, 2500));
    TEST_ASSERT_LESS_THAN(0, mipi_phy_power_validate(4, 2500));
    TEST_ASSERT_LESS_THAN(0, mipi_phy_power_validate(-1, 2500));
}

void test_mipi_phy_power_validate_invalid_voltage(void) {
    TEST_ASSERT_LESS_THAN(0, mipi_phy_power_validate(3, 2249));
    TEST_ASSERT_LESS_THAN(0, mipi_phy_power_validate(3, 2751));
    TEST_ASSERT_LESS_THAN(0, mipi_phy_power_validate(3, 0));
    TEST_ASSERT_LESS_THAN(0, mipi_phy_power_validate(3, -100));
    TEST_ASSERT_LESS_THAN(0, mipi_phy_power_validate(3, 5000));
}

// 主なテストを実行
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mipi_phy_power_validate_valid);
    RUN_TEST(test_mipi_phy_power_validate_invalid_chan);
    RUN_TEST(test_mipi_phy_power_validate_invalid_voltage);
    return UNITY_END();
}

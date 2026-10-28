#include "unity.h"
#include "tc358743_pll_calc.h"

void setUp(void) {}
void tearDown(void) {}

void test_required_lane_rate_basic(void) {
    uint32_t rate_kbps = 0;

    // 720p60 UYVY 16bpp (1280 * 720 * 60 * 16 / 2 = 442368000 bps -> 442368 kbps)
    TEST_ASSERT_TRUE(tc358743_required_lane_rate_kbps(1280, 720, 60000, 16, 2, &rate_kbps));
    TEST_ASSERT_EQUAL(442368, rate_kbps);

    // 720p60 RGB888 24bpp (1280 * 720 * 60 * 24 / 2 = 663552000 bps -> 663552 kbps)
    TEST_ASSERT_TRUE(tc358743_required_lane_rate_kbps(1280, 720, 60000, 24, 2, &rate_kbps));
    TEST_ASSERT_EQUAL(663552, rate_kbps);

    // 720p50 UYVY 16bpp (1280 * 720 * 50 * 16 / 2 = 368640000 bps -> 368640 kbps)
    TEST_ASSERT_TRUE(tc358743_required_lane_rate_kbps(1280, 720, 50000, 16, 2, &rate_kbps));
    TEST_ASSERT_EQUAL(368640, rate_kbps);

    // 720p50 RGB888 24bpp (1280 * 720 * 50 * 24 / 2 = 552960000 bps -> 552960 kbps)
    TEST_ASSERT_TRUE(tc358743_required_lane_rate_kbps(1280, 720, 50000, 24, 2, &rate_kbps));
    TEST_ASSERT_EQUAL(552960, rate_kbps);

    // 1080p25 UYVY 16bpp (1920 * 1080 * 25 * 16 / 2 = 414720000 bps -> 414720 kbps)
    TEST_ASSERT_TRUE(tc358743_required_lane_rate_kbps(1920, 1080, 25000, 16, 2, &rate_kbps));
    TEST_ASSERT_EQUAL(414720, rate_kbps);

    // 1080p25 RGB888 24bpp (1920 * 1080 * 25 * 24 / 2 = 622080000 bps -> 622080 kbps)
    TEST_ASSERT_TRUE(tc358743_required_lane_rate_kbps(1920, 1080, 25000, 24, 2, &rate_kbps));
    TEST_ASSERT_EQUAL(622080, rate_kbps);

    // 1080p30 UYVY 16bpp (1920 * 1080 * 30 * 16 / 2 = 497664000 bps -> 497664 kbps)
    TEST_ASSERT_TRUE(tc358743_required_lane_rate_kbps(1920, 1080, 30000, 16, 2, &rate_kbps));
    TEST_ASSERT_EQUAL(497664, rate_kbps);

    // 1080p30 RGB888 24bpp (1920 * 1080 * 30 * 24 / 2 = 746496000 bps -> 746496 kbps)
    TEST_ASSERT_TRUE(tc358743_required_lane_rate_kbps(1920, 1080, 30000, 24, 2, &rate_kbps));
    TEST_ASSERT_EQUAL(746496, rate_kbps);

    // 1080p50 UYVY 16bpp (1920 * 1080 * 50 * 16 / 2 = 829440000 bps -> 829440 kbps)
    TEST_ASSERT_TRUE(tc358743_required_lane_rate_kbps(1920, 1080, 50000, 16, 2, &rate_kbps));
    TEST_ASSERT_EQUAL(829440, rate_kbps);

    // 1080p60 UYVY 16bpp (1920 * 1080 * 60 * 16 / 2 = 995328000 bps -> 995328 kbps)
    TEST_ASSERT_TRUE(tc358743_required_lane_rate_kbps(1920, 1080, 60000, 16, 2, &rate_kbps));
    TEST_ASSERT_EQUAL(995328, rate_kbps);
}

void test_required_lane_rate_edge_cases(void) {
    uint32_t rate_kbps = 0;

    // Argument 0 checks
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(0, 720, 60000, 16, 2, &rate_kbps));
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(1280, 0, 60000, 16, 2, &rate_kbps));
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(1280, 720, 0, 16, 2, &rate_kbps));
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(1280, 720, 60000, 0, 2, &rate_kbps));
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(1280, 720, 60000, 16, 2, NULL));

    // Invalid lane count
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(1280, 720, 60000, 16, 3, &rate_kbps));

    // Overflow check (unrealistic large inputs)
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(UINT32_MAX, UINT32_MAX, 60000, 16, 2, &rate_kbps));

    // Rounding check
    // 1 * 1 * 1000 * 1 / 2 = 500 bps -> 0.5 kbps -> requires rounding up to 1 kbps
    TEST_ASSERT_TRUE(tc358743_required_lane_rate_kbps(1, 1, 1000, 1, 2, &rate_kbps));
    TEST_ASSERT_EQUAL(1, rate_kbps);
}

void test_pll_calc_basic(void) {
    tc358743_pll_config_t cfg;

    // 27MHz Refclk, 720p60 UYVY (req: 442368 kbps)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 442368, &cfg));
    TEST_ASSERT_EQUAL(4, cfg.pll_prd); // 27MHz / 6MHz = 4 (6.75MHz input)
    TEST_ASSERT_EQUAL(66, cfg.pll_fbd); // 442368 / 6.75 = 65.536 -> 66
    TEST_ASSERT_EQUAL(445500, cfg.lane_rate_kbps); // 6.75MHz * 66 = 445.5Mbps
    TEST_ASSERT_EQUAL(1, cfg.frs); // 445.5MHz > 250MHz => 1

    // 27MHz Refclk, 720p60 RGB888 (req: 663552 kbps)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 663552, &cfg));
    TEST_ASSERT_EQUAL(4, cfg.pll_prd);
    TEST_ASSERT_EQUAL(99, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(668250, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(0, cfg.frs); // > 500MHz => 0

    // 27MHz Refclk, 720p50 UYVY (req: 368640 kbps)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 368640, &cfg));
    TEST_ASSERT_EQUAL(4, cfg.pll_prd);
    TEST_ASSERT_EQUAL(55, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(371250, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(1, cfg.frs);

    // 27MHz Refclk, 720p50 RGB888 (req: 552960 kbps)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 552960, &cfg));
    TEST_ASSERT_EQUAL(4, cfg.pll_prd);
    TEST_ASSERT_EQUAL(82, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(553500, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(0, cfg.frs);

    // 27MHz Refclk, 1080p25 UYVY (req: 414720 kbps)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 414720, &cfg));
    TEST_ASSERT_EQUAL(4, cfg.pll_prd);
    TEST_ASSERT_EQUAL(62, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(418500, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(1, cfg.frs);

    // 27MHz Refclk, 1080p25 RGB888 (req: 622080 kbps)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 622080, &cfg));
    TEST_ASSERT_EQUAL(4, cfg.pll_prd);
    TEST_ASSERT_EQUAL(93, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(627750, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(0, cfg.frs);

    // 27MHz Refclk, 1080p30 UYVY (req: 497664 kbps)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 497664, &cfg));
    TEST_ASSERT_EQUAL(4, cfg.pll_prd);
    TEST_ASSERT_EQUAL(74, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(499500, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(1, cfg.frs);

    // 27MHz Refclk, 1080p30 RGB888 (req: 746496 kbps)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 746496, &cfg));
    TEST_ASSERT_EQUAL(4, cfg.pll_prd);
    TEST_ASSERT_EQUAL(111, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(749250, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(0, cfg.frs);

    // 27MHz Refclk, 1080p50 UYVY (req: 829440 kbps)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 829440, &cfg));
    TEST_ASSERT_EQUAL(4, cfg.pll_prd);
    TEST_ASSERT_EQUAL(123, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(830250, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(0, cfg.frs);

    // 27MHz Refclk, 1080p60 UYVY (req: 995328 kbps)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 995328, &cfg));
    TEST_ASSERT_EQUAL(4, cfg.pll_prd);
    TEST_ASSERT_EQUAL(148, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(999000, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(0, cfg.frs);
}

void test_pll_calc_manual_fbd(void) {
    tc358743_pll_config_t cfg;

    // Explicit 972Mbps => 972000 kbps (1080p50 UYVY overhead test)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 972000, &cfg));
    TEST_ASSERT_EQUAL(144, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(972000, cfg.lane_rate_kbps);

    // Explicit 594Mbps => 594000 kbps
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 594000, &cfg));
    TEST_ASSERT_EQUAL(88, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(594000, cfg.lane_rate_kbps);
}

void test_pll_calc_boundaries(void) {
    tc358743_pll_config_t cfg;

    // 1080p60 RGB888 (req: 1493.0Mbps) -> should fail due to > 1Gbps
    TEST_ASSERT_FALSE(tc358743_pll_calc(27000000, 1492992, &cfg));

    // 1080p50 RGB888 (req: 1244.2Mbps) -> should fail due to > 1Gbps
    TEST_ASSERT_FALSE(tc358743_pll_calc(27000000, 1244160, &cfg));

    // Refclk outside allowed list (e.g. 25MHz)
    TEST_ASSERT_FALSE(tc358743_pll_calc(25000000, 500000, &cfg));

    // Zero required rate
    TEST_ASSERT_FALSE(tc358743_pll_calc(27000000, 0, &cfg));

    // NULL pointer
    TEST_ASSERT_FALSE(tc358743_pll_calc(27000000, 500000, NULL));

    // < 62.5Mbps
    TEST_ASSERT_FALSE(tc358743_pll_calc(27000000, 60000, &cfg));

    // > 1Gbps
    TEST_ASSERT_FALSE(tc358743_pll_calc(27000000, 1000001, &cfg));
}

void test_pll_calc_frs_boundaries(void) {
    tc358743_pll_config_t cfg;

    // Exact 500MHz (requires 500000 kbps) -> > 500MHz is 0, so exact 500MHz should be 1
    // Refclk 27MHz, pll_prd 4, input 6.75MHz
    // 500000 / 6.75 = 74.07 -> fbd=75 -> 506.25MHz (frs=0)
    // fbd=74 -> 499.5MHz (frs=1)

    // Let's use 26MHz for precise boundary hit if possible
    // 26MHz / 6MHz = 4 (6.5MHz input)
    // 500000 / 6.5 = 76.9 -> fbd=77 -> 500.5MHz

    // We can directly test the frs logic by calculating the rate

    // frs boundary > 500MHz (506.25MHz)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 500000, &cfg));
    TEST_ASSERT_EQUAL(75, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(506250, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(0, cfg.frs);

    // frs boundary > 125MHz
    // 125000 / 6.75 = 18.5 -> fbd=19 -> 128.25MHz
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 125000, &cfg));
    TEST_ASSERT_EQUAL(19, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(128250, cfg.lane_rate_kbps);
    // Removed wrong assert
    // Wait: 128.25MHz is > 125MHz, so FRS should be 2. Let's verify our expectation.
    TEST_ASSERT_EQUAL(2, cfg.frs);

    // Rate < 125MHz -> FRS=3
    // 100000 / 6.75 = 14.8 -> fbd=15 -> 101.25MHz
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 100000, &cfg));
    TEST_ASSERT_EQUAL(15, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(101250, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(3, cfg.frs);
}


int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_required_lane_rate_basic);
    RUN_TEST(test_required_lane_rate_edge_cases);
    RUN_TEST(test_pll_calc_basic);
    RUN_TEST(test_pll_calc_manual_fbd);
    RUN_TEST(test_pll_calc_boundaries);
    RUN_TEST(test_pll_calc_frs_boundaries);
    return UNITY_END();
}

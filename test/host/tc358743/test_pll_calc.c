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

void test_pll_calc_frs_representative_values(void) {
    tc358743_pll_config_t cfg;

    // FRS > 500MHz (e.g. 506.25MHz)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 500000, &cfg));
    TEST_ASSERT_EQUAL(75, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(506250, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(0, cfg.frs);

    // FRS > 125MHz (e.g. 128.25MHz)
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 125000, &cfg));
    TEST_ASSERT_EQUAL(19, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(128250, cfg.lane_rate_kbps);

    TEST_ASSERT_EQUAL(2, cfg.frs);

    // Rate < 125MHz -> FRS=3
    // 100000 / 6.75 = 14.8 -> fbd=15 -> 101.25MHz
    TEST_ASSERT_TRUE(tc358743_pll_calc(27000000, 100000, &cfg));
    TEST_ASSERT_EQUAL(15, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(101250, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(3, cfg.frs);
}


void test_required_lane_rate_limits(void);
void test_required_lane_rate_uint32_max(void);
void test_pll_calc_26mhz_42mhz(void);
void test_pll_calc_out_unmodified_on_fail(void);

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_required_lane_rate_basic);
    RUN_TEST(test_required_lane_rate_edge_cases);
    RUN_TEST(test_pll_calc_basic);
    RUN_TEST(test_pll_calc_manual_fbd);
    RUN_TEST(test_pll_calc_boundaries);
    RUN_TEST(test_pll_calc_frs_representative_values);
    RUN_TEST(test_required_lane_rate_limits);
    RUN_TEST(test_required_lane_rate_uint32_max);
    RUN_TEST(test_pll_calc_26mhz_42mhz);
    RUN_TEST(test_pll_calc_out_unmodified_on_fail);
    return UNITY_END();
}

void test_required_lane_rate_limits(void) {
    uint32_t rate_kbps = 0;

    // Bounds check
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(65536, 720, 60000, 16, 2, &rate_kbps));
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(1280, 65536, 60000, 16, 2, &rate_kbps));
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(1280, 720, 1000001, 16, 2, &rate_kbps));
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(1280, 720, 60000, 65, 2, &rate_kbps));

    // Maximum allowed values
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(65535, 65535, 1000000, 64, 2, &rate_kbps));
    // h * v * fps * bpp = 65535 * 65535 * 1000000 * 64 = 2.7487373e+17 < 2^64 (1.844e+19)
    // divided by 2000000 = 137436862502 kbps > UINT32_MAX
    // Wait, the result is greater than UINT32_MAX, so it will actually fail the UINT32_MAX check.
    // So let's test a value that passes the max input check but fails the UINT32_MAX check.
}

void test_required_lane_rate_uint32_max(void) {
    uint32_t rate_kbps = 0;

    // h_active=1048576 (but wait, max is 65535 now).
    // Let's use max valid inputs that produce > UINT32_MAX
    // 65535 * 65535 * 1000000 * 64 / 2000000 = 137436862502 kbps (fails UINT32_MAX)
    TEST_ASSERT_FALSE(tc358743_required_lane_rate_kbps(65535, 65535, 1000000, 64, 2, &rate_kbps));

    // Max valid that fits in UINT32_MAX
    // UINT32_MAX = 4294967295 kbps
    // e.g. 65535 * 65535 * 1000 * 2 / 2000000 = 4294836 kbps -> OK
    TEST_ASSERT_TRUE(tc358743_required_lane_rate_kbps(65535, 65535, 2000, 2, 2, &rate_kbps));
}

void test_pll_calc_26mhz_42mhz(void) {
    tc358743_pll_config_t cfg;

    // 26MHz Refclk, 720p60 UYVY (req: 442368 kbps)
    // pll_prd = 26M / 6M = 4. Input = 26M / 4 = 6.5MHz.
    // min_lane_rate = 442368. fbd = ceil(442368 / 6500) = ceil(68.056) = 69.
    // lane_rate_kbps = 6500 * 69 = 448500 kbps (448.5 Mbps).
    // frs: 448.5MHz > 250MHz -> frs = 1.
    TEST_ASSERT_TRUE(tc358743_pll_calc(26000000, 442368, &cfg));
    TEST_ASSERT_EQUAL(4, cfg.pll_prd);
    TEST_ASSERT_EQUAL(69, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(448500, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(1, cfg.frs);

    // 42MHz Refclk, 720p60 UYVY (req: 442368 kbps)
    // pll_prd = 42M / 6M = 7. Input = 42M / 7 = 6.0MHz.
    // min_lane_rate = 442368. fbd = ceil(442368 / 6000) = ceil(73.728) = 74.
    // lane_rate_kbps = 6000 * 74 = 444000 kbps (444.0 Mbps).
    // frs: 444.0MHz > 250MHz -> frs = 1.
    TEST_ASSERT_TRUE(tc358743_pll_calc(42000000, 442368, &cfg));
    TEST_ASSERT_EQUAL(7, cfg.pll_prd);
    TEST_ASSERT_EQUAL(74, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(444000, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(1, cfg.frs);
}

void test_pll_calc_out_unmodified_on_fail(void) {
    tc358743_pll_config_t cfg;
    cfg.pll_prd = 999;
    cfg.pll_fbd = 999;
    cfg.lane_rate_kbps = 999;
    cfg.frs = 999;

    // Invalid refclk
    TEST_ASSERT_FALSE(tc358743_pll_calc(25000000, 442368, &cfg));
    TEST_ASSERT_EQUAL(999, cfg.pll_prd);
    TEST_ASSERT_EQUAL(999, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(999, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(999, cfg.frs);

    // Exceed max lane rate (1Gbps)
    TEST_ASSERT_FALSE(tc358743_pll_calc(27000000, 1500000, &cfg));
    TEST_ASSERT_EQUAL(999, cfg.pll_prd);
    TEST_ASSERT_EQUAL(999, cfg.pll_fbd);
    TEST_ASSERT_EQUAL(999, cfg.lane_rate_kbps);
    TEST_ASSERT_EQUAL(999, cfg.frs);
}

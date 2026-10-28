#include "tc358743_pll_calc.h"
#include <stddef.h>

bool tc358743_required_lane_rate_kbps(uint32_t h_active, uint32_t v_active, uint32_t fps_x1000, uint32_t bpp, uint32_t lanes, uint32_t *out_kbps) {
    if (out_kbps == NULL || h_active == 0 || v_active == 0 || fps_x1000 == 0 || bpp == 0) {
        return false;
    }
    if (lanes != 1 && lanes != 2) {
        return false;
    }

    // Rate in bps: h_active * v_active * fps * bpp / lanes
    // To get kbps directly (avoiding 64-bit overflow where possible, though uint64_t is safe):
    // fps_x1000 provides the factor of 1000 directly.
    // rate_kbps = (h_active * v_active * (fps_x1000 / 1000) * bpp) / (lanes * 1000)
    // Equivalently: rate_kbps = (h_active * v_active * fps_x1000 * bpp) / (lanes * 1000000)

    uint64_t total_bits_per_sec_x1000 = (uint64_t)h_active * v_active * fps_x1000 * bpp;

    // Check for overflow on the uint64_t multiplication (unlikely but safe)
    if (total_bits_per_sec_x1000 / bpp / fps_x1000 / v_active != h_active) {
        return false;
    }

    uint64_t divisor = lanes * 1000000ULL;
    // Round up
    uint64_t rate_kbps = (total_bits_per_sec_x1000 + divisor - 1) / divisor;

    if (rate_kbps > UINT32_MAX) {
        return false;
    }

    *out_kbps = (uint32_t)rate_kbps;
    return true;
}

bool tc358743_pll_calc(uint32_t refclk_hz, uint32_t min_lane_rate_kbps, tc358743_pll_config_t *out) {
    if (out == NULL || min_lane_rate_kbps == 0) {
        return false;
    }

    // Refclk must be 26MHz, 27MHz, or 42MHz
    if (refclk_hz != 26000000 && refclk_hz != 27000000 && refclk_hz != 42000000) {
        return false;
    }

    uint32_t pll_prd = refclk_hz / 6000000;
    if (pll_prd == 0) {
        return false;
    }

    uint32_t pll_input_hz = refclk_hz / pll_prd;
    if (pll_input_hz < 6000000 || pll_input_hz > 40000000) {
        return false;
    }

    // lane_rate_bps = (refclk_hz / pll_prd) * pll_fbd
    // lane_rate_kbps = (refclk_hz / pll_prd / 1000) * pll_fbd
    // We want lane_rate_kbps >= min_lane_rate_kbps
    // pll_fbd >= min_lane_rate_kbps / (refclk_hz / pll_prd / 1000)

    uint32_t pll_input_khz = pll_input_hz / 1000;
    if (pll_input_khz == 0) {
        return false;
    }

    // Calculate FBD (round up)
    uint32_t pll_fbd = (min_lane_rate_kbps + pll_input_khz - 1) / pll_input_khz;

    // FBD bounds check (register field is 9 bits, 0-511 mapped to 1-512)
    if (pll_fbd < 1 || pll_fbd > 512) {
        return false;
    }

    uint64_t lane_rate_kbps = (uint64_t)pll_input_khz * pll_fbd;
    uint64_t lane_rate_bps = lane_rate_kbps * 1000;

    // Range check: 62.5Mbps to 1Gbps
    if (lane_rate_bps < 62500000ULL || lane_rate_bps > 1000000000ULL) {
        return false;
    }

    // Calculate FRS
    // hsck = lane_rate_bps
    uint16_t frs;
    if (lane_rate_bps > 500000000ULL) {
        frs = 0;
    } else if (lane_rate_bps > 250000000ULL) {
        frs = 1;
    } else if (lane_rate_bps > 125000000ULL) {
        frs = 2;
    } else {
        frs = 3;
    }

    out->pll_prd = (uint16_t)pll_prd;
    out->pll_fbd = (uint16_t)pll_fbd;
    out->frs = frs;
    out->lane_rate_kbps = (uint32_t)lane_rate_kbps;

    return true;
}

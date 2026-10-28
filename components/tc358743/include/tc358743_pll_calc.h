#ifndef TC358743_PLL_CALC_H
#define TC358743_PLL_CALC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Calculates the required minimum lane rate in kbps for a given resolution, framerate, bpp, and number of lanes.
 *
 * Formula: (h_active * v_active * fps * bpp) / lanes
 *
 * @param h_active Horizontal active pixels
 * @param v_active Vertical active lines
 * @param fps_x1000 Frames per second multiplied by 1000 (e.g., 60000 for 60fps)
 * @param bpp Bits per pixel (e.g., 16 for UYVY, 24 for RGB888)
 * @param lanes Number of CSI-2 lanes (must be 1 or 2)
 * @param out_kbps Pointer to store the calculated required lane rate in kbps (rounded up)
 * @return true on success, false on invalid input or overflow
 */
bool tc358743_required_lane_rate_kbps(uint32_t h_active, uint32_t v_active, uint32_t fps_x1000, uint32_t bpp, uint32_t lanes, uint32_t *out_kbps);

typedef struct {
    uint16_t pll_prd;      // PLL PRD value (actual value, not register value (-1))
    uint16_t pll_fbd;      // PLL FBD value (actual value, not register value (-1))
    uint16_t frs;          // FRS value
    uint32_t lane_rate_kbps; // Calculated lane rate in kbps
} tc358743_pll_config_t;

/**
 * Calculates the PLL parameters for a given reference clock and minimum required lane rate.
 *
 * @param refclk_hz Reference clock frequency in Hz (must be 26000000, 27000000, or 42000000)
 * @param min_lane_rate_kbps Minimum required lane rate in kbps
 * @param out Pointer to store the calculated PLL configuration
 * @return true on success, false on invalid input or if parameters cannot satisfy constraints
 */
bool tc358743_pll_calc(uint32_t refclk_hz, uint32_t min_lane_rate_kbps, tc358743_pll_config_t *out);

#ifdef __cplusplus
}
#endif

#endif // TC358743_PLL_CALC_H

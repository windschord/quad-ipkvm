#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "tc358743_start.h"

#ifdef __cplusplus
extern "C" {
#endif

// Generates EDID based on a specific profile.
size_t tc358743_edid_build_profile(uint8_t* out_buffer, size_t buffer_size, tc358743_edid_profile_t profile);

// Generates EDID based on parameters.
// Returns the actual size written (usually 256 bytes for Base + CEA Extension).
// Max pixel clock is used to limit advertised modes.
size_t tc358743_edid_build(uint8_t* out_buffer, size_t buffer_size, uint32_t max_pixel_clock_khz);

#ifdef __cplusplus
}
#endif

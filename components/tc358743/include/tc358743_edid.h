#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Generates EDID based on parameters.
// Returns the actual size written (usually 256 bytes for Base + CEA Extension).
// Max pixel clock is used to limit advertised modes.
size_t tc358743_edid_build(uint8_t* out_buffer, size_t buffer_size, uint32_t max_pixel_clock_mhz);

#ifdef __cplusplus
}
#endif

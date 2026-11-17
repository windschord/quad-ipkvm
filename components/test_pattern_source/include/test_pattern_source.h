#pragma once
#include "video_source.h"
#include "video_status.h"
#include "kvm_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

// Original direct start
void test_pattern_source_start(frame_hub_t* hub);

// Video Source interface
const video_source_t* test_pattern_source_get(void);

// ---- Video status (docs/INTERFACES.md 1.3, video_status component) ----
// The test pattern has no HDMI input, so its status is a fake one that can be switched by hand.
// Default: LOCKED at the test pattern's resolution (TEST_PATTERN_WIDTH x TEST_PATTERN_HEIGHT).
//
// Register it ONCE, BEFORE any task that reads video_status is started (see video_status.h):
//     video_status_set_provider(test_pattern_source_get_status, NULL);
void test_pattern_source_get_status(void* ctx, video_status_t* out);   // video_status_get_fn (ctx is unused)

// Switch the fake status (for checking the KVM logic without a device). The values are kept as given
// (video_status_get() validates them). Call it from one task only.
void test_pattern_source_set_status(kvm_video_sync_state_t state, uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif

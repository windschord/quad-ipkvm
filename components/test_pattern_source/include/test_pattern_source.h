#pragma once
#include "video_source.h"

#ifdef __cplusplus
extern "C" {
#endif

// Original direct start
void test_pattern_source_start(frame_hub_t* hub);

// Video Source interface
const video_source_t* test_pattern_source_get(void);

#ifdef __cplusplus
}
#endif

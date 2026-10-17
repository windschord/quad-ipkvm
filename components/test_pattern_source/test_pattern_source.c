#include "test_pattern_source.h"
#include "test_pattern_data.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>

static void test_pattern_task(void* arg) {
    frame_hub_t* hub = (frame_hub_t*)arg;
    int current_frame = 0;

    // FPS could be configurable, defaulting to 10
    const int fps = 10;
    const int frame_delay_ms = 1000 / fps;

    while (1) {
        size_t size = test_pattern_frame_sizes[current_frame];
        const uint8_t* data = test_pattern_frames[current_frame];

        video_frame_t* frame = frame_hub_begin_publish(hub, size);
        if (frame) {
            memcpy(frame->data, data, size);
            frame_hub_commit(hub, frame, 640, 480, esp_timer_get_time() / 1000);
        }

        current_frame = (current_frame + 1) % TEST_PATTERN_NUM_FRAMES;
        vTaskDelay(pdMS_TO_TICKS(frame_delay_ms));
    }
}

void test_pattern_source_start(frame_hub_t* hub) {
    if (!hub) return;
    xTaskCreate(test_pattern_task, "test_pattern", 4096, hub, 5, NULL);
}

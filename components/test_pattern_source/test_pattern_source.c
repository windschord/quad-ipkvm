#include "test_pattern_source.h"
#include "test_pattern_data.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>

#ifndef CONFIG_TEST_PATTERN_FPS
#define CONFIG_TEST_PATTERN_FPS 10
#endif

static void test_pattern_task(void* arg) {
    frame_hub_t* hub = (frame_hub_t*)arg;
    int current_frame = 0;

    const int fps = CONFIG_TEST_PATTERN_FPS;
    const int frame_delay_ms = 1000 / fps;

    while (1) {
        size_t size = test_pattern_frame_sizes[current_frame];
        const uint8_t* data = test_pattern_frames[current_frame];

        video_frame_t* frame = frame_hub_begin_publish(hub, size);
        if (frame) {
            memcpy(frame->data, data, size);
            frame_hub_commit(hub, frame, size, TEST_PATTERN_WIDTH, TEST_PATTERN_HEIGHT, esp_timer_get_time() / 1000);
        }

        current_frame = (current_frame + 1) % TEST_PATTERN_NUM_FRAMES;
        vTaskDelay(pdMS_TO_TICKS(frame_delay_ms));
    }
}

void test_pattern_source_start(frame_hub_t* hub) {
    if (!hub) return;
    xTaskCreate(test_pattern_task, "test_pattern", 4096, hub, 5, NULL);
}

static TaskHandle_t s_task_handle = NULL;

static int test_pattern_source_interface_start(const video_source_t *source, frame_hub_t *hub) {
    if (!hub) return -1;
    if (s_task_handle != NULL) return 0; // Already running
    if (xTaskCreate(test_pattern_task, "test_pattern", 4096, hub, 5, &s_task_handle) != pdPASS) {
        return -1;
    }
    return 0;
}

static void test_pattern_source_interface_stop(const video_source_t *source) {
    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }
}

static const video_source_t s_test_pattern_source = {
    .name = "test_pattern",
    .start = test_pattern_source_interface_start,
    .stop = test_pattern_source_interface_stop,
    .ctx = NULL
};

const video_source_t* test_pattern_source_get(void) {
    return &s_test_pattern_source;
}

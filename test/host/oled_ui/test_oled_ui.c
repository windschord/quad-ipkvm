#include "unity.h"
#include "oled_ui.h"
#include <string.h>

void setUp(void) {
}

void tearDown(void) {
}

void test_oled_ui_init_success(void) {
    oled_ui_t ui = {0};
    oled_ui_config_t config = {
        .num_pages = 3,
        .display_timeout_ms = 30000,
        .redraw_interval_ms = 1000
    };

    TEST_ASSERT_TRUE(oled_ui_init(&ui, &config, 1000));
    TEST_ASSERT_TRUE(oled_ui_is_on(&ui));
    TEST_ASSERT_EQUAL_UINT8(0, oled_ui_page(&ui));
}

void test_oled_ui_init_fail_invalid_params(void) {
    oled_ui_t ui = {0};
    oled_ui_config_t config = {
        .num_pages = 0, // Invalid
        .display_timeout_ms = 30000,
        .redraw_interval_ms = 1000
    };

    TEST_ASSERT_FALSE(oled_ui_init(&ui, &config, 1000));

    config.num_pages = 3;
    config.display_timeout_ms = 0; // Invalid
    TEST_ASSERT_FALSE(oled_ui_init(&ui, &config, 1000));

    TEST_ASSERT_FALSE(oled_ui_init(NULL, &config, 1000));
    TEST_ASSERT_FALSE(oled_ui_init(&ui, NULL, 1000));
}

void test_oled_ui_button_when_off(void) {
    oled_ui_t ui = {0};
    oled_ui_config_t config = { .num_pages = 3, .display_timeout_ms = 30000, .redraw_interval_ms = 1000 };
    oled_ui_init(&ui, &config, 1000);

    // Force OFF state
    ui.is_on = false;
    ui.current_page = 2; // Should reset to 0

    uint32_t flags = oled_ui_on_button(&ui, 5000);

    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_TURN_ON | OLED_UI_FLAG_REDRAW, flags);
    TEST_ASSERT_TRUE(oled_ui_is_on(&ui));
    TEST_ASSERT_EQUAL_UINT8(0, oled_ui_page(&ui));
    TEST_ASSERT_EQUAL_UINT64(5000, ui.last_action_time_ms);
}

void test_oled_ui_button_when_on_page_wrap(void) {
    oled_ui_t ui = {0};
    oled_ui_config_t config = { .num_pages = 3, .display_timeout_ms = 30000, .redraw_interval_ms = 1000 };
    oled_ui_init(&ui, &config, 1000);

    uint32_t flags;
    // Page 0 -> 1
    flags = oled_ui_on_button(&ui, 2000);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_REDRAW, flags);
    TEST_ASSERT_EQUAL_UINT8(1, oled_ui_page(&ui));
    TEST_ASSERT_EQUAL_UINT64(2000, ui.last_action_time_ms);

    // Page 1 -> 2
    flags = oled_ui_on_button(&ui, 3000);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_REDRAW, flags);
    TEST_ASSERT_EQUAL_UINT8(2, oled_ui_page(&ui));

    // Page 2 -> 0 (wrap)
    flags = oled_ui_on_button(&ui, 4000);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_REDRAW, flags);
    TEST_ASSERT_EQUAL_UINT8(0, oled_ui_page(&ui));
}

void test_oled_ui_ip_acquired(void) {
    oled_ui_t ui = {0};
    oled_ui_config_t config = { .num_pages = 3, .display_timeout_ms = 30000, .redraw_interval_ms = 1000 };

    // When OFF
    oled_ui_init(&ui, &config, 1000);
    ui.is_on = false;
    uint32_t flags = oled_ui_on_ip_acquired(&ui, 5000);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_TURN_ON | OLED_UI_FLAG_REDRAW, flags);
    TEST_ASSERT_TRUE(oled_ui_is_on(&ui));
    TEST_ASSERT_EQUAL_UINT8(0, oled_ui_page(&ui));

    // When ON, on page 2 (which is page 1 or more)
    ui.current_page = 2;
    flags = oled_ui_on_ip_acquired(&ui, 6000);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_REDRAW, flags); // Changes page, so REDRAW
    TEST_ASSERT_TRUE(oled_ui_is_on(&ui));
    TEST_ASSERT_EQUAL_UINT8(0, oled_ui_page(&ui));

    // When ON, on page 1
    ui.current_page = 1;
    flags = oled_ui_on_ip_acquired(&ui, 7000);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_REDRAW, flags);
    TEST_ASSERT_TRUE(oled_ui_is_on(&ui));
    TEST_ASSERT_EQUAL_UINT8(0, oled_ui_page(&ui));

    // When ON, on page 0
    flags = oled_ui_on_ip_acquired(&ui, 8000);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_REDRAW, flags); // Should always REDRAW
    TEST_ASSERT_TRUE(oled_ui_is_on(&ui));
    TEST_ASSERT_EQUAL_UINT8(0, oled_ui_page(&ui));
    TEST_ASSERT_EQUAL_UINT64(8000, ui.last_action_time_ms);
}

void test_oled_ui_tick_auto_off(void) {
    oled_ui_t ui = {0};
    oled_ui_config_t config = { .num_pages = 3, .display_timeout_ms = 30000, .redraw_interval_ms = 1000 };
    oled_ui_init(&ui, &config, 1000);

    uint32_t flags;
    // Tick before timeout
    flags = oled_ui_tick(&ui, 1000 + 29999);
    // Should be REDRAW because 29999 >= 1000 redraw interval
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_REDRAW, flags);
    TEST_ASSERT_TRUE(oled_ui_is_on(&ui));

    // Tick exact timeout
    flags = oled_ui_tick(&ui, 1000 + 30000);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_TURN_OFF, flags);
    TEST_ASSERT_FALSE(oled_ui_is_on(&ui));
    TEST_ASSERT_EQUAL_UINT8(0, oled_ui_page(&ui));

    // Tick after timeout (when OFF)
    flags = oled_ui_tick(&ui, 1000 + 30001);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_NONE, flags);
}

void test_oled_ui_tick_redraw(void) {
    oled_ui_t ui = {0};
    oled_ui_config_t config = { .num_pages = 3, .display_timeout_ms = 30000, .redraw_interval_ms = 1000 };
    oled_ui_init(&ui, &config, 1000);

    // At 1999, not yet 1000ms elapsed
    uint32_t flags = oled_ui_tick(&ui, 1999);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_NONE, flags);

    // At 2000, 1000ms elapsed
    flags = oled_ui_tick(&ui, 2000);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_REDRAW, flags);
    TEST_ASSERT_EQUAL_UINT64(2000, ui.last_redraw_time_ms);
}

void test_oled_ui_time_backwards(void) {
    oled_ui_t ui = {0};
    oled_ui_config_t config = { .num_pages = 3, .display_timeout_ms = 30000, .redraw_interval_ms = 1000 };
    oled_ui_init(&ui, &config, 1000);

    // Time backwards
    uint32_t flags = oled_ui_tick(&ui, 999);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_TURN_OFF, flags);
    TEST_ASSERT_FALSE(oled_ui_is_on(&ui));
}

void test_oled_ui_uint64_boundary(void) {
    oled_ui_t ui = {0};
    oled_ui_config_t config = { .num_pages = 3, .display_timeout_ms = 30000, .redraw_interval_ms = 1000 };
    uint64_t near_max = UINT64_MAX - 10000;

    oled_ui_init(&ui, &config, near_max);

    // Check redraw before timeout
    uint32_t flags = oled_ui_tick(&ui, near_max + 1000);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_REDRAW, flags);

    // Check timeout
    flags = oled_ui_tick(&ui, near_max + 30000);
    TEST_ASSERT_EQUAL_UINT32(OLED_UI_FLAG_TURN_OFF, flags);
}

// === Random Test / Oracle ===

// Simple model representing the specification
typedef struct {
    bool is_on;
    uint8_t page;
    uint64_t last_action_ms;
    uint64_t last_redraw_ms;
} oracle_state_t;

void test_oled_ui_fuzzing(void) {
    oled_ui_t ui = {0};
    oled_ui_config_t config = { .num_pages = 3, .display_timeout_ms = 30000, .redraw_interval_ms = 1000 };

    uint64_t current_time = 100;
    oled_ui_init(&ui, &config, current_time);

    oracle_state_t oracle = {
        .is_on = true,
        .page = 0,
        .last_action_ms = current_time,
        .last_redraw_ms = current_time
    };

    // PRNG (LCG)
    uint32_t seed = 0x12345678;

    for (int i = 0; i < 10000; i++) {
        seed = seed * 1664525 + 1013904223;
        uint32_t action = (seed >> 16) % 100;

        // Time advancement
        uint64_t time_advance = (seed >> 24) % 5000;
        current_time += time_advance;

        // Time backwards simulation (rare)
        if (action < 1) { // 1% chance
            current_time -= 10000;
        }

        uint32_t act_flags = OLED_UI_FLAG_NONE;
        uint32_t expected_flags = OLED_UI_FLAG_NONE;

        if (action >= 1 && action < 10) { // Button press
            act_flags = oled_ui_on_button(&ui, current_time);

            // Oracle logic
            if (current_time < oracle.last_action_ms) {
                // Ignore, let tick handle time backwards or we just reset
                oracle.last_action_ms = current_time;
                oracle.last_redraw_ms = current_time;
            }
            if (!oracle.is_on) {
                oracle.is_on = true;
                oracle.page = 0;
                expected_flags = OLED_UI_FLAG_TURN_ON | OLED_UI_FLAG_REDRAW;
            } else {
                oracle.page = (oracle.page + 1) % 3;
                expected_flags = OLED_UI_FLAG_REDRAW;
            }
            oracle.last_action_ms = current_time;
            oracle.last_redraw_ms = current_time;

        } else if (action >= 10 && action < 15) { // IP acquired
            act_flags = oled_ui_on_ip_acquired(&ui, current_time);

            // Oracle logic
            if (current_time < oracle.last_action_ms) {
                oracle.last_action_ms = current_time;
                oracle.last_redraw_ms = current_time;
            }
            if (!oracle.is_on) {
                oracle.is_on = true;
                oracle.page = 0;
                expected_flags = OLED_UI_FLAG_TURN_ON | OLED_UI_FLAG_REDRAW;
            } else {
                expected_flags = OLED_UI_FLAG_REDRAW;
            }
            oracle.page = 0;
            oracle.last_action_ms = current_time;
            oracle.last_redraw_ms = current_time;

        } else { // Tick
            act_flags = oled_ui_tick(&ui, current_time);

            // Oracle logic
            if (oracle.is_on) {
                if (current_time < oracle.last_action_ms || current_time < oracle.last_redraw_ms) {
                    oracle.is_on = false;
                    oracle.page = 0;
                    expected_flags = OLED_UI_FLAG_TURN_OFF;
                } else if (current_time - oracle.last_action_ms >= 30000) {
                    oracle.is_on = false;
                    oracle.page = 0;
                    expected_flags = OLED_UI_FLAG_TURN_OFF;
                } else if (current_time - oracle.last_redraw_ms >= 1000) {
                    oracle.last_redraw_ms = current_time;
                    expected_flags = OLED_UI_FLAG_REDRAW;
                }
            }
        }

        // Validation
        // If time went backwards, things might be off temporarily if we didn't tick, so let's check invariants mainly
        if (current_time < ui.last_action_time_ms && act_flags == OLED_UI_FLAG_NONE && (action < 1 || (action >= 15))) {
            // Unhandled backwards time until tick, wait for next tick
        } else {
             if (act_flags != expected_flags && !(current_time < oracle.last_action_ms)) {
                 // In time backwards case, oracle and SUT might diverge slightly if not ticked. Let's strictly compare only forward time.
                 TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected_flags, act_flags, "Flags mismatch in fuzzing");
             }
             if (!(current_time < oracle.last_action_ms)) {
                 TEST_ASSERT_EQUAL_MESSAGE(oracle.is_on, oled_ui_is_on(&ui), "State mismatch in fuzzing");
                 if (oracle.is_on) {
                     TEST_ASSERT_EQUAL_UINT8_MESSAGE(oracle.page, oled_ui_page(&ui), "Page mismatch in fuzzing");
                 }
             }
        }

        // Invariants
        if (!oled_ui_is_on(&ui)) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, oled_ui_page(&ui), "Page must be 0 when OFF");
        } else {
            TEST_ASSERT_LESS_THAN_UINT8_MESSAGE(config.num_pages, oled_ui_page(&ui), "Page out of bounds");
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_oled_ui_init_success);
    RUN_TEST(test_oled_ui_init_fail_invalid_params);
    RUN_TEST(test_oled_ui_button_when_off);
    RUN_TEST(test_oled_ui_button_when_on_page_wrap);
    RUN_TEST(test_oled_ui_ip_acquired);
    RUN_TEST(test_oled_ui_tick_auto_off);
    RUN_TEST(test_oled_ui_tick_redraw);
    RUN_TEST(test_oled_ui_time_backwards);
    RUN_TEST(test_oled_ui_uint64_boundary);
    RUN_TEST(test_oled_ui_fuzzing);
    return UNITY_END();
}

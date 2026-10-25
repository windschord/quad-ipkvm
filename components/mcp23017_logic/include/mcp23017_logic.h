#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MCP23017 Register Definitions (IOCON.BANK = 0)
 * Reference: Microchip MCP23017/MCP23S17 Data Sheet (DS20001952C)
 * Table 3-1: REGISTER ADDRESS MAP
 */
#define MCP23017_REG_IODIRA   0x00
#define MCP23017_REG_IODIRB   0x01
#define MCP23017_REG_IPOLA    0x02
#define MCP23017_REG_IPOLB    0x03
#define MCP23017_REG_GPINTENA 0x04
#define MCP23017_REG_GPINTENB 0x05
#define MCP23017_REG_DEFVALA  0x06
#define MCP23017_REG_DEFVALB  0x07
#define MCP23017_REG_INTCONA  0x08
#define MCP23017_REG_INTCONB  0x09
#define MCP23017_REG_IOCON    0x0A // Also 0x0B in BANK=0
#define MCP23017_REG_GPPUA    0x0C
#define MCP23017_REG_GPPUB    0x0D
#define MCP23017_REG_INTFA    0x0E
#define MCP23017_REG_INTFB    0x0F
#define MCP23017_REG_INTCAPA  0x10
#define MCP23017_REG_INTCAPB  0x11
#define MCP23017_REG_GPIOA    0x12
#define MCP23017_REG_GPIOB    0x13
#define MCP23017_REG_OLATA    0x14
#define MCP23017_REG_OLATB    0x15

/*
 * IOCON bit definitions
 * Reference: Register 1-5: IOCON
 */
#define MCP23017_IOCON_INTPOL (1 << 1)
#define MCP23017_IOCON_ODR    (1 << 2)
#define MCP23017_IOCON_HAEN   (1 << 3) // For SPI only
#define MCP23017_IOCON_DISSLW (1 << 4)
#define MCP23017_IOCON_SEQOP  (1 << 5)
#define MCP23017_IOCON_MIRROR (1 << 6)
#define MCP23017_IOCON_BANK   (1 << 7)

#define MCP23017_NUM_PANEL_LEDS 4
#define MCP23017_NUM_BUTTONS    5

/* Button IDs */
#define MCP23017_BTN_PANEL_0    0 // GPIOA.4
#define MCP23017_BTN_PANEL_1    1 // GPIOA.5
#define MCP23017_BTN_PANEL_2    2 // GPIOA.6
#define MCP23017_BTN_PANEL_3    3 // GPIOA.7
#define MCP23017_BTN_DISPLAY    4 // GPIOB.0

typedef enum {
    MCP23017_EVENT_PRESSED,
    MCP23017_EVENT_RELEASED
} mcp23017_event_type_t;

typedef struct {
    uint8_t button_id;
    mcp23017_event_type_t type;
} mcp23017_event_t;

typedef struct {
    int (*write_reg)(void* user, uint8_t reg, uint8_t value);
    int (*read_reg)(void* user, uint8_t reg, uint8_t* value);
    int (*read_regs)(void* user, uint8_t start_reg, uint8_t* buf, uint8_t len);
    int (*set_reset_line)(void* user, int level); // Optional, can be NULL
    void* user;

    bool led_active_high;
    uint32_t debounce_ms; // Default 20
    uint32_t max_consecutive_errors; // Default 5
    uint32_t verify_interval_polls; // Default 100, 0 to disable
} mcp23017_config_t;

typedef struct {
    uint8_t shadow_olata;
    uint32_t consecutive_errors;
    bool is_offline;
    uint32_t reinit_count;
    uint32_t verify_counter;

    uint64_t btn_last_changed_ms[MCP23017_NUM_BUTTONS];
    bool btn_state_raw[MCP23017_NUM_BUTTONS];
    bool btn_state_debounced[MCP23017_NUM_BUTTONS];

    mcp23017_event_t event_queue[16];
    uint32_t event_head;
    uint32_t event_tail;
    uint32_t event_overflow_count;

    uint64_t last_poll_ms;
} mcp23017_state_t;

typedef struct {
    mcp23017_config_t cfg;
    mcp23017_state_t state;
} mcp23017_t;

int mcp23017_logic_init(mcp23017_t* ctx, const mcp23017_config_t* cfg);
int mcp23017_logic_set_led(mcp23017_t* ctx, uint8_t led_idx, bool on);
int mcp23017_logic_set_leds(mcp23017_t* ctx, uint8_t mask);
void mcp23017_logic_poll(mcp23017_t* ctx, uint64_t now_ms);
bool mcp23017_logic_next_event(mcp23017_t* ctx, mcp23017_event_t* event);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HID_ABS_MAX 32767 // 絶対ポインタの座標は 0..32767（論理最大値）

typedef struct {
    uint8_t modifiers; // bit0 LCtrl, 1 LShift, 2 LAlt, 3 LGUI, 4 RCtrl, 5 RShift, 6 RAlt, 7 RGUI
    uint8_t keys[6];   // HID Usage（Keyboard/Keypad Page 0x07）。0 = なし。7個以上のときは全部0x01（ErrorRollOver）
} hid_kbd_report_t;

typedef struct {
    uint8_t  buttons;  // bit0 左, bit1 右, bit2 中
    uint16_t x, y;     // 0..HID_ABS_MAX
    int8_t   wheel;    // -127..127
} hid_abs_report_t;

typedef struct {
    uint8_t buttons;
    int8_t  dx, dy;    // -127..127
    int8_t  wheel;
} hid_rel_report_t;

typedef struct {
    void *ctx;
    bool (*ready)(void *ctx); // ホストが列挙済みで、送れる状態か
    bool (*send_keyboard)(void *ctx, const hid_kbd_report_t *r);
    bool (*send_abs)(void *ctx, const hid_abs_report_t *r);
    bool (*send_rel)(void *ctx, const hid_rel_report_t *r);
} hid_sink_t;

#ifdef __cplusplus
}
#endif

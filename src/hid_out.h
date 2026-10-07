/*
 * USB HID report output. All functions are non-blocking: reports are queued
 * per interface and sent as the host polls. If an interface is not
 * configured by the host yet, the report is silently dropped.
 */

#ifndef HID_OUT_H_
#define HID_OUT_H_

#include <stdint.h>

#define HID_KB_REPORT_LEN 8

int hid_out_init(void);

/* Boot keyboard report: [modifiers, reserved, key1..key6]. */
void hid_out_keyboard(const uint8_t report[HID_KB_REPORT_LEN]);

void hid_out_mouse_rel(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);

/* x, y in 0..4095 */
void hid_out_mouse_abs(uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel);

/*
 * Release all keys and buttons. The absolute mouse keeps its last position
 * (an all-zero report would move the pointer to the top-left corner).
 */
void hid_out_release_all(void);

#endif /* HID_OUT_H_ */

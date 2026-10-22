/* Channels -> virtual joystick: pure logic, no I/O (DESIGN.md section 4).
 *
 * CH1..CH8 are axes in the order of the Windows app's vJoy mapping (X, Y, RX, RY, RZ, Z, then the two
 * sliders, here ABS_THROTTLE and ABS_RUDDER), so bindings carry over. The raw value 0..32767 is the axis
 * value as it is: the device's range is 0..32767, and nothing is scaled or filtered on the way.
 * CH9..CH16 are joystick buttons 1..8 (BTN_TRIGGER..BTN_BASE2), like the ELRS BLE joystick: pressed at
 * raw >= 24576 (75 %), released below 20480 (62.5 %). The gap between the two is hysteresis, so a switch
 * that sits near a threshold never chatters.
 *
 * joymap keeps what the device shows and returns only the events that change it; the caller writes them
 * with a SYN_REPORT (uinput.c). */
#ifndef JOYMAP_H
#define JOYMAP_H

#include <stdbool.h>
#include <stdint.h>

#define JOY_AXES        8
#define JOY_BUTTONS     8
#define JOY_AXIS_MIN    0
#define JOY_AXIS_MAX    32767
#define JOY_CENTER      16384
#define JOY_BTN_PRESS   24576       /* raw >= this: pressed (75 %) */
#define JOY_BTN_RELEASE 20480       /* raw < this: released (62.5 %) */
#define JOY_MAX_EVENTS  (JOY_AXES + JOY_BUTTONS)

/* linux/input-event-codes.h values, so this file needs no kernel headers (and tests can check them) */
#define JOY_EV_SYN      0x00
#define JOY_EV_KEY      0x01
#define JOY_EV_ABS      0x03
#define JOY_SYN_REPORT  0

typedef struct { uint16_t type, code; int32_t value; } joy_event_t;

typedef struct {
  bool buttons;                     /* CH9..CH16 as buttons */
  int32_t axis[JOY_AXES];           /* what the device shows */
  bool btn[JOY_BUTTONS];
} joymap_t;

/* ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_RZ, ABS_Z, ABS_THROTTLE, ABS_RUDDER */
extern const uint16_t JOY_AXIS_CODES[JOY_AXES];
/* BTN_TRIGGER (0x120) .. BTN_BASE2 (0x127) */
extern const uint16_t JOY_BUTTON_CODES[JOY_BUTTONS];
/* "X", "Y", "RX", "RY", "RZ", "Z", "THROTTLE", "RUDDER" */
extern const char *const JOY_AXIS_NAMES[JOY_AXES];

/* Every axis at JOY_CENTER, every button released: the state of a new device. */
void joymap_init(joymap_t *m, bool buttons);
/* The next state of a button: hysteresis between JOY_BTN_RELEASE and JOY_BTN_PRESS. */
bool joymap_button_next(bool pressed, int raw);
/* One applied frame (raw 0..32767, n channels; values above 32767 count as 32767). A channel the frame
 * does not carry keeps its axis/button. Writes the events that change the device to out (room for
 * JOY_MAX_EVENTS) and returns how many: axes first (CH order), then buttons. 0 = nothing to write. */
int  joymap_frame(joymap_t *m, const uint16_t *raw, int n, joy_event_t *out);
/* Center every axis and release every button (lock lost, stop, exit). Returns the events for what
 * changed (0 when everything already is there). */
int  joymap_reset(joymap_t *m, joy_event_t *out);
/* Every axis and button as it is now (for a new device whose initial state may differ: old kernels
 * without UI_ABS_SETUP start every axis at 0). Returns how many. */
int  joymap_full(const joymap_t *m, joy_event_t *out);

#endif

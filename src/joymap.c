/* Channels -> virtual joystick events: see joymap.h. */
#include "joymap.h"

#include <string.h>

const uint16_t JOY_AXIS_CODES[JOY_AXES] = { 0x00, 0x01, 0x03, 0x04, 0x05, 0x02, 0x06, 0x07 };
const uint16_t JOY_BUTTON_CODES[JOY_BUTTONS] = { 0x120, 0x121, 0x122, 0x123, 0x124, 0x125, 0x126, 0x127 };
const char *const JOY_AXIS_NAMES[JOY_AXES] = { "X", "Y", "RX", "RY", "RZ", "Z", "THROTTLE", "RUDDER" };

void joymap_init(joymap_t *m, bool buttons) {
  memset(m, 0, sizeof *m);
  m->buttons = buttons;
  for (int i = 0; i < JOY_AXES; i++) m->axis[i] = JOY_CENTER;
}

bool joymap_button_next(bool pressed, int raw) {
  if (raw >= JOY_BTN_PRESS) return true;
  if (raw < JOY_BTN_RELEASE) return false;
  return pressed;
}

static int ev(joy_event_t *out, int k, uint16_t type, uint16_t code, int32_t value) {
  out[k].type = type;
  out[k].code = code;
  out[k].value = value;
  return k + 1;
}

int joymap_frame(joymap_t *m, const uint16_t *raw, int n, joy_event_t *out) {
  int k = 0;
  if (!raw || n <= 0) return 0;
  int na = n < JOY_AXES ? n : JOY_AXES;
  for (int i = 0; i < na; i++) {
    int32_t v = raw[i] > JOY_AXIS_MAX ? JOY_AXIS_MAX : raw[i];
    if (v == m->axis[i]) continue;
    m->axis[i] = v;
    k = ev(out, k, JOY_EV_ABS, JOY_AXIS_CODES[i], v);
  }
  if (!m->buttons) return k;
  for (int i = 0; i < JOY_BUTTONS && JOY_AXES + i < n; i++) {
    int r = raw[JOY_AXES + i] > JOY_AXIS_MAX ? JOY_AXIS_MAX : raw[JOY_AXES + i];
    bool p = joymap_button_next(m->btn[i], r);
    if (p == m->btn[i]) continue;
    m->btn[i] = p;
    k = ev(out, k, JOY_EV_KEY, JOY_BUTTON_CODES[i], p);
  }
  return k;
}

int joymap_reset(joymap_t *m, joy_event_t *out) {
  int k = 0;
  for (int i = 0; i < JOY_AXES; i++) {
    if (m->axis[i] == JOY_CENTER) continue;
    m->axis[i] = JOY_CENTER;
    k = ev(out, k, JOY_EV_ABS, JOY_AXIS_CODES[i], JOY_CENTER);
  }
  for (int i = 0; i < JOY_BUTTONS; i++) {
    if (!m->btn[i]) continue;
    m->btn[i] = false;
    k = ev(out, k, JOY_EV_KEY, JOY_BUTTON_CODES[i], 0);
  }
  return k;
}

int joymap_full(const joymap_t *m, joy_event_t *out) {
  int k = 0;
  for (int i = 0; i < JOY_AXES; i++) k = ev(out, k, JOY_EV_ABS, JOY_AXIS_CODES[i], m->axis[i]);
  if (m->buttons)
    for (int i = 0; i < JOY_BUTTONS; i++) k = ev(out, k, JOY_EV_KEY, JOY_BUTTON_CODES[i], m->btn[i]);
  return k;
}

/* joymap: channels -> axis and button events (DESIGN.md section 4). */
#include "tests/t.h"
#include "joymap.h"

#include <linux/input.h>

static joymap_t M;
static joy_event_t ev[JOY_MAX_EVENTS + 4];

static int frame(int n, const int *v) {
  uint16_t raw[16];
  for (int i = 0; i < n; i++) raw[i] = (uint16_t)v[i];
  return joymap_frame(&M, raw, n, ev);
}
#define NV(...) ((int)(sizeof((int[]){__VA_ARGS__}) / sizeof(int)))
#define FRAME(...) frame(NV(__VA_ARGS__), (int[]){__VA_ARGS__})

static bool has(int n, int type, int code, int value) {
  for (int i = 0; i < n; i++) if (ev[i].type == type && ev[i].code == code && ev[i].value == value) return true;
  return false;
}

static void codes_are_the_kernels(void) {
  /* the Windows app's vJoy order: X, Y, RX, RY, RZ, Z, slider 0, slider 1 */
  CHECK_INT(JOY_AXIS_CODES[0], ABS_X);
  CHECK_INT(JOY_AXIS_CODES[1], ABS_Y);
  CHECK_INT(JOY_AXIS_CODES[2], ABS_RX);
  CHECK_INT(JOY_AXIS_CODES[3], ABS_RY);
  CHECK_INT(JOY_AXIS_CODES[4], ABS_RZ);
  CHECK_INT(JOY_AXIS_CODES[5], ABS_Z);
  CHECK_INT(JOY_AXIS_CODES[6], ABS_THROTTLE);
  CHECK_INT(JOY_AXIS_CODES[7], ABS_RUDDER);
  static const int btn[8] = { BTN_TRIGGER, BTN_THUMB, BTN_THUMB2, BTN_TOP, BTN_TOP2, BTN_PINKIE, BTN_BASE, BTN_BASE2 };
  for (int i = 0; i < 8; i++) CHECK_INT(JOY_BUTTON_CODES[i], btn[i]);
  CHECK_INT(JOY_EV_ABS, EV_ABS);
  CHECK_INT(JOY_EV_KEY, EV_KEY);
  CHECK_INT(JOY_EV_SYN, EV_SYN);
  CHECK_INT(JOY_SYN_REPORT, SYN_REPORT);
  CHECK_STR(JOY_AXIS_NAMES[6], "THROTTLE");
  /* distinct codes: no two channels on one axis */
  for (int i = 0; i < 8; i++)
    for (int k = i + 1; k < 8; k++) CHECK(JOY_AXIS_CODES[i] != JOY_AXIS_CODES[k]);
}

static void init_centers(void) {
  joymap_init(&M, true);
  for (int i = 0; i < JOY_AXES; i++) CHECK_INT(M.axis[i], 16384);
  for (int i = 0; i < JOY_BUTTONS; i++) CHECK(!M.btn[i]);
  CHECK(M.buttons);
  joymap_init(&M, false);
  CHECK(!M.buttons);
  /* nothing to center on a new map */
  CHECK_INT(joymap_reset(&M, ev), 0);
}

static void axes_pass_raw_through(void) {
  joymap_init(&M, true);
  int n = FRAME(0, 32767, 100, 16384, 20000, 1, 32766, 12345);
  CHECK_INT(n, 7);                                   /* CH4 = center already: no event */
  CHECK(has(n, EV_ABS, ABS_X, 0));
  CHECK(has(n, EV_ABS, ABS_Y, 32767));
  CHECK(has(n, EV_ABS, ABS_RX, 100));
  CHECK(!has(n, EV_ABS, ABS_RY, 16384));
  CHECK(has(n, EV_ABS, ABS_RZ, 20000));
  CHECK(has(n, EV_ABS, ABS_Z, 1));
  CHECK(has(n, EV_ABS, ABS_THROTTLE, 32766));
  CHECK(has(n, EV_ABS, ABS_RUDDER, 12345));
  /* in channel order */
  CHECK_INT(ev[0].code, ABS_X);
  CHECK_INT(ev[1].code, ABS_Y);
  CHECK_INT(ev[2].code, ABS_RX);
  CHECK_INT(ev[6].code, ABS_RUDDER);
  /* the same frame again: nothing changes, nothing to write */
  CHECK_INT(FRAME(0, 32767, 100, 16384, 20000, 1, 32766, 12345), 0);
  /* one axis moves by 1: exactly that one (no dead band) */
  n = FRAME(1, 32767, 100, 16384, 20000, 1, 32766, 12345);
  CHECK_INT(n, 1);
  CHECK(has(n, EV_ABS, ABS_X, 1));
  /* above 32767 counts as 32767 (the core already clamps; joymap never trusts it) */
  n = FRAME(65535, 32767, 100, 16384, 20000, 1, 32766, 12345);
  CHECK_INT(n, 1);
  CHECK(has(n, EV_ABS, ABS_X, 32767));
}

static void short_frames_release_the_rest(void) {
  joymap_init(&M, true);
  FRAME(100, 200, 300, 400, 500, 600, 700, 800, 32767, 32767);
  CHECK(M.btn[0] && M.btn[1]);
  int n = FRAME(1, 2, 3, 4);                          /* 4 channels: CH5.. are no longer driven */
  CHECK_INT(n, 4 + 4 + 2);
  CHECK(has(n, EV_ABS, ABS_X, 1));
  CHECK(has(n, EV_ABS, ABS_RZ, 16384));               /* CH5 */
  CHECK(has(n, EV_ABS, ABS_RUDDER, 16384));           /* CH8 */
  CHECK(has(n, EV_KEY, BTN_TRIGGER, 0));              /* CH9 */
  CHECK(has(n, EV_KEY, BTN_THUMB, 0));                /* CH10 */
  CHECK_INT(M.axis[4], 16384);
  CHECK_INT(M.axis[7], 16384);
  CHECK(!M.btn[0] && !M.btn[1]);
  CHECK_INT(FRAME(1, 2, 3, 4), 0);                    /* and then nothing more to write */
  /* 8 channels after 16: the buttons are released, the axes follow */
  FRAME(1, 2, 3, 4, 5, 6, 7, 8, 32767, 0, 0, 0, 0, 0, 0, 32767);
  CHECK(M.btn[0] && M.btn[7]);
  n = FRAME(1, 2, 3, 4, 5, 6, 7, 8);
  CHECK_INT(n, 2);
  CHECK(has(n, EV_KEY, BTN_TRIGGER, 0) && has(n, EV_KEY, BTN_BASE2, 0));
  /* without buttons: only the axes */
  joymap_init(&M, false);
  FRAME(100, 200, 300, 400, 500, 600, 700, 800, 32767);
  CHECK(!M.btn[0]);
  n = FRAME(1, 2, 3, 4);
  CHECK_INT(n, 8);
  /* n <= 0 or no data: nothing */
  CHECK_INT(joymap_frame(&M, NULL, 4, ev), 0);
  uint16_t r[1] = { 5 };
  CHECK_INT(joymap_frame(&M, r, 0, ev), 0);
  CHECK_INT(joymap_frame(&M, r, -3, ev), 0);
  CHECK_INT(M.axis[0], 1);
}

static void buttons_with_hysteresis(void) {
  CHECK(!joymap_button_next(false, 0));
  CHECK(!joymap_button_next(false, 24575));
  CHECK(joymap_button_next(false, 24576));          /* 75 % */
  CHECK(joymap_button_next(true, 24575));           /* held between the two */
  CHECK(joymap_button_next(true, 20480));           /* 62.5 % still held */
  CHECK(!joymap_button_next(true, 20479));          /* below: released */
  CHECK(!joymap_button_next(false, 20480));         /* in the band, released stays released */
  CHECK(joymap_button_next(true, 32767));

  joymap_init(&M, true);
  int v[16];
  for (int i = 0; i < 16; i++) v[i] = 16384;
  CHECK_INT(frame(16, v), 0);
  v[8] = 32767;                                       /* CH9 = button 1 */
  v[15] = 24576;                                      /* CH16 = button 8, at the threshold */
  int n = frame(16, v);
  CHECK_INT(n, 2);
  CHECK(has(n, EV_KEY, BTN_TRIGGER, 1));
  CHECK(has(n, EV_KEY, BTN_BASE2, 1));
  /* a switch drifting in the band: no chatter */
  v[8] = 22000;
  v[15] = 20480;
  CHECK_INT(frame(16, v), 0);
  v[8] = 24576;
  CHECK_INT(frame(16, v), 0);
  v[8] = 20479;
  n = frame(16, v);
  CHECK_INT(n, 1);
  CHECK(has(n, EV_KEY, BTN_TRIGGER, 0));
  v[8] = 24575;                                       /* below the press threshold: stays up */
  CHECK_INT(frame(16, v), 0);
  /* every button at once, after the axes */
  for (int i = 8; i < 16; i++) v[i] = 30000;
  v[0] = 1;
  n = frame(16, v);
  CHECK_INT(n, 1 + 7);
  CHECK_INT(ev[0].type, EV_ABS);
  for (int i = 1; i < n; i++) CHECK_INT(ev[i].type, EV_KEY);
  /* a 12-channel frame: buttons 1-4 follow (CH9-12 = 0), 5-8 are no longer driven: released */
  int w[12];
  for (int i = 0; i < 12; i++) w[i] = i < 8 ? v[i] : 0;
  n = frame(12, w);
  CHECK_INT(n, 8);
  for (int i = 0; i < 8; i++) CHECK(!M.btn[i]);
  /* CH9-12 pressed again in a 12-channel frame: only those */
  for (int i = 8; i < 12; i++) w[i] = 32767;
  n = frame(12, w);
  CHECK_INT(n, 4);
  for (int i = 0; i < 4; i++) CHECK(M.btn[i]);
  for (int i = 4; i < 8; i++) CHECK(!M.btn[i]);
}

static void no_buttons_mode(void) {
  joymap_init(&M, false);
  int v[16];
  for (int i = 0; i < 16; i++) v[i] = 32767;
  int n = frame(16, v);
  CHECK_INT(n, 8);                                    /* the axes only */
  for (int i = 0; i < n; i++) CHECK_INT(ev[i].type, EV_ABS);
  for (int i = 0; i < 8; i++) CHECK(!M.btn[i]);
  CHECK_INT(joymap_reset(&M, ev), 8);
  CHECK_INT(joymap_full(&M, ev), 8);                  /* no buttons on the device either */
}

static void reset_centers_and_releases(void) {
  joymap_init(&M, true);
  int v[16];
  for (int i = 0; i < 16; i++) v[i] = i == 3 ? 16384 : 32767;
  frame(16, v);
  int n = joymap_reset(&M, ev);
  CHECK_INT(n, 7 + 8);                                /* CH4 was at the center */
  for (int i = 0; i < 7; i++) { CHECK_INT(ev[i].type, EV_ABS); CHECK_INT(ev[i].value, JOY_CENTER); }
  for (int i = 7; i < n; i++) { CHECK_INT(ev[i].type, EV_KEY); CHECK_INT(ev[i].value, 0); }
  CHECK_INT(joymap_reset(&M, ev), 0);                 /* twice: nothing more */
  for (int i = 0; i < 8; i++) CHECK_INT(M.axis[i], 16384);
  /* after a reset the same frame is a change again */
  CHECK_INT(frame(16, v), 15);
}

static void full_state(void) {
  joymap_init(&M, true);
  int n = joymap_full(&M, ev);
  CHECK_INT(n, 16);
  for (int i = 0; i < 8; i++) { CHECK_INT(ev[i].type, EV_ABS); CHECK_INT(ev[i].code, JOY_AXIS_CODES[i]); CHECK_INT(ev[i].value, 16384); }
  for (int i = 8; i < 16; i++) { CHECK_INT(ev[i].type, EV_KEY); CHECK_INT(ev[i].code, JOY_BUTTON_CODES[i - 8]); CHECK_INT(ev[i].value, 0); }
  FRAME(5, 5, 5, 5, 5, 5, 5, 5, 30000);
  n = joymap_full(&M, ev);
  CHECK_INT(ev[0].value, 5);
  CHECK_INT(ev[8].value, 1);
}

/* random frames never produce more than JOY_MAX_EVENTS, and the map always equals what was sent */
static void fuzz(void) {
  uint32_t x = 12345;
  joymap_init(&M, true);
  int32_t axis[8];
  bool btn[8];
  for (int i = 0; i < 8; i++) { axis[i] = 16384; btn[i] = false; }
  for (int it = 0; it < 20000; it++) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    int n = 4 + (int)(x % 13);
    uint16_t raw[16];
    for (int i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; raw[i] = (uint16_t)(x & 0x7fff); }
    int k = joymap_frame(&M, raw, n, ev);
    if (k < 0 || k > JOY_MAX_EVENTS) { CHECK(false); break; }
    for (int i = 0; i < k; i++) {
      if (ev[i].type == EV_ABS) {
        for (int a = 0; a < 8; a++) if (JOY_AXIS_CODES[a] == ev[i].code) axis[a] = ev[i].value;
      } else {
        btn[ev[i].code - BTN_TRIGGER] = ev[i].value;
      }
    }
    if (it % 97 == 0) {
      k = joymap_reset(&M, ev);
      for (int i = 0; i < k; i++) {
        if (ev[i].type == EV_ABS) { for (int a = 0; a < 8; a++) if (JOY_AXIS_CODES[a] == ev[i].code) axis[a] = ev[i].value; }
        else btn[ev[i].code - BTN_TRIGGER] = ev[i].value;
      }
    }
    for (int a = 0; a < 8; a++) if (axis[a] != M.axis[a] || btn[a] != M.btn[a]) { CHECK(false); it = 1 << 30; break; }
  }
}

int main(void) {
  codes_are_the_kernels();
  init_centers();
  axes_pass_raw_through();
  short_frames_release_the_rest();
  buttons_with_hysteresis();
  no_buttons_mode();
  reset_centers_and_releases();
  full_state();
  fuzz();
  T_DONE();
}

/* The virtual joystick (uinput): see uinput.h. */
#include "uinput.h"
#include "hints.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* joymap.h carries the codes without kernel headers: they must be the kernel's */
_Static_assert(JOY_EV_SYN == EV_SYN && JOY_EV_KEY == EV_KEY && JOY_EV_ABS == EV_ABS && JOY_SYN_REPORT == SYN_REPORT,
               "event types");
_Static_assert(ABS_X == 0 && ABS_Y == 1 && ABS_Z == 2 && ABS_RX == 3 && ABS_RY == 4 && ABS_RZ == 5 &&
               ABS_THROTTLE == 6 && ABS_RUDDER == 7, "axis codes");
_Static_assert(BTN_TRIGGER == 0x120 && BTN_BASE2 == 0x127, "button codes");
_Static_assert(sizeof(ujoy_event_t) == 2 * sizeof(long) + 8, "struct input_event layout");

const ujoy_sys_t *ujoy_sys;

static int s_open(const char *p, int fl) { return ujoy_sys && ujoy_sys->open ? ujoy_sys->open(p, fl) : open(p, fl); }
static int s_ioctl(int fd, unsigned long req, void *arg) {
  return ujoy_sys && ujoy_sys->ioctl ? ujoy_sys->ioctl(fd, req, arg) : ioctl(fd, req, arg);
}
static ssize_t s_write(int fd, const void *b, size_t n) {
  return ujoy_sys && ujoy_sys->write ? ujoy_sys->write(fd, b, n) : write(fd, b, n);
}
static int s_close(int fd) { return ujoy_sys && ujoy_sys->close ? ujoy_sys->close(fd) : close(fd); }
static const char *s_sysfs(void) { return ujoy_sys && ujoy_sys->sysfs ? ujoy_sys->sysfs : "/sys"; }

/* an int argument as the kernel reads it: a full unsigned long, never an int with stale upper bits */
#define IARG(v) ((void *)(uintptr_t)(v))

int ujoy_encode(const joy_event_t *ev, int n, ujoy_event_t *out) {
  if (n < 0) n = 0;
  for (int i = 0; i <= n; i++) {
    out[i].sec = out[i].usec = 0;     /* the kernel stamps events itself */
    out[i].type = i < n ? ev[i].type : (uint16_t)EV_SYN;
    out[i].code = i < n ? ev[i].code : (uint16_t)SYN_REPORT;
    out[i].value = i < n ? ev[i].value : 0;
  }
  return n + 1;
}

int ujoy_find_node(const char *sysfs, const char *sysname, char *out, size_t cap) {
  if (cap) out[0] = 0;
  if (!sysname || !*sysname || strchr(sysname, '/') || !strcmp(sysname, "..")) return -1;
  char dir[512];
  snprintf(dir, sizeof dir, "%s/devices/virtual/input/%s", sysfs ? sysfs : "/sys", sysname);
  DIR *d = opendir(dir);
  if (!d) return -1;
  int found = -1;
  struct dirent *de;
  while ((de = readdir(d))) {
    const char *s = de->d_name;
    if (strncmp(s, "event", 5) || !s[5]) continue;
    bool digits = true;
    for (const char *c = s + 5; *c; c++) digits = digits && *c >= '0' && *c <= '9';
    if (!digits || strlen(s) > 16) continue;
    snprintf(out, cap, "/dev/input/%s", s);
    found = 0;
    break;
  }
  closedir(d);
  return found;
}

static int fail(ujoy_t *j, char *err, size_t cap, const char *what, int e) {
  snprintf(err, cap, "cannot create the virtual joystick: %s failed (%s)", what, strerror(e));
  if (j->fd >= 0) s_close(j->fd);
  j->fd = -1;
  return -1;
}

int ujoy_open(ujoy_t *j, const char *path, const char *name, bool buttons, const char *user, char *err, size_t cap) {
  memset(j, 0, sizeof *j);
  j->fd = -1;
  j->buttons = buttons;
  snprintf(j->name, sizeof j->name, "%s", name && *name ? name : "ELRS Crossfire WiFi Joystick");
  if (cap) err[0] = 0;
  if (!path) path = UJOY_PATH;
  /* O_NONBLOCK: a write never waits (uinput never makes it wait anyway) */
  j->fd = s_open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (j->fd < 0) {
    hints_uinput(errno, path, user, err, cap);
    j->fd = -1;
    return -1;
  }
  if (s_ioctl(j->fd, UI_SET_EVBIT, IARG(EV_ABS)) < 0) return fail(j, err, cap, "UI_SET_EVBIT", errno);
  for (int i = 0; i < JOY_AXES; i++)
    if (s_ioctl(j->fd, UI_SET_ABSBIT, IARG(JOY_AXIS_CODES[i])) < 0) return fail(j, err, cap, "UI_SET_ABSBIT", errno);
  /* the buttons exist even when CH9-CH16 are not buttons (they are never pressed then): udev and SDL take
   * a device with X, Y and Z axes and no EV_KEY for an accelerometer, not a joystick */
  if (s_ioctl(j->fd, UI_SET_EVBIT, IARG(EV_KEY)) < 0) return fail(j, err, cap, "UI_SET_EVBIT", errno);
  for (int i = 0; i < JOY_BUTTONS; i++)
    if (s_ioctl(j->fd, UI_SET_KEYBIT, IARG(JOY_BUTTON_CODES[i])) < 0) return fail(j, err, cap, "UI_SET_KEYBIT", errno);

  /* kernel 4.5+: UI_ABS_SETUP (it also sets the initial value) and UI_DEV_SETUP; older kernels answer
   * EINVAL (unknown uinput ioctl) or ENOTTY, and take a struct uinput_user_dev written to the fd */
  for (int i = 0; i < JOY_AXES && !j->legacy; i++) {
    struct uinput_abs_setup a;
    memset(&a, 0, sizeof a);
    a.code = JOY_AXIS_CODES[i];
    a.absinfo.value = JOY_CENTER;
    a.absinfo.minimum = JOY_AXIS_MIN;
    a.absinfo.maximum = JOY_AXIS_MAX;
    a.absinfo.fuzz = 0;
    a.absinfo.flat = 0;
    a.absinfo.resolution = 0;
    if (s_ioctl(j->fd, UI_ABS_SETUP, &a) == 0) continue;
    int e = errno;
    if (i == 0 && (e == EINVAL || e == ENOTTY)) j->legacy = true;
    else return fail(j, err, cap, "UI_ABS_SETUP", e);
  }
  if (!j->legacy) {
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0;
    us.id.product = 0;
    us.id.version = 1;
    snprintf(us.name, sizeof us.name, "%s", j->name);
    if (s_ioctl(j->fd, UI_DEV_SETUP, &us) < 0) return fail(j, err, cap, "UI_DEV_SETUP", errno);
  } else {
    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof ud);
    snprintf(ud.name, sizeof ud.name, "%s", j->name);
    ud.id.bustype = BUS_VIRTUAL;
    ud.id.version = 1;
    for (int i = 0; i < JOY_AXES; i++) {
      ud.absmin[JOY_AXIS_CODES[i]] = JOY_AXIS_MIN;
      ud.absmax[JOY_AXIS_CODES[i]] = JOY_AXIS_MAX;
      ud.absfuzz[JOY_AXIS_CODES[i]] = 0;
      ud.absflat[JOY_AXIS_CODES[i]] = 0;
    }
    ssize_t k = s_write(j->fd, &ud, sizeof ud);
    if (k != (ssize_t)sizeof ud) return fail(j, err, cap, "the device setup write", k < 0 ? errno : EIO);
  }
  if (s_ioctl(j->fd, UI_DEV_CREATE, NULL) < 0) return fail(j, err, cap, "UI_DEV_CREATE", errno);

  char sys[64] = "";
  /* the kernel answers the length of the name it copied (> 0), not 0 */
  if (s_ioctl(j->fd, UI_GET_SYSNAME(sizeof sys), sys) >= 0) {
    sys[sizeof sys - 1] = 0;
    snprintf(j->sysname, sizeof j->sysname, "%s", sys);
    /* evdev attaches while the device registers, so the node is normally there at once; a moment's
     * patience for a slow machine (this runs once, at startup) */
    for (int t = 0; t < 20 && ujoy_find_node(s_sysfs(), j->sysname, j->device, sizeof j->device) < 0; t++) {
      struct timespec ts = { 0, 10000000 };
      nanosleep(&ts, NULL);
    }
  }
  if (j->legacy) {
    /* no initial value in the old interface: every axis starts at 0 there */
    joymap_t m;
    joy_event_t ev[JOY_MAX_EVENTS];
    joymap_init(&m, buttons);
    ujoy_write(j, ev, joymap_full(&m, ev));
  }
  return 0;
}

void ujoy_close(ujoy_t *j) {
  if (!j || j->fd < 0) return;
  s_ioctl(j->fd, UI_DEV_DESTROY, NULL);
  s_close(j->fd);
  j->fd = -1;
  j->device[0] = 0;
}

int ujoy_write(ujoy_t *j, const joy_event_t *ev, int n) {
  if (!j || j->fd < 0 || n <= 0) return 0;
  if (n > JOY_MAX_EVENTS) n = JOY_MAX_EVENTS;
  ujoy_event_t buf[JOY_MAX_EVENTS + 1];
  int m = ujoy_encode(ev, n, buf);
  size_t len = sizeof buf[0] * (size_t)m;
  for (;;) {
    ssize_t k = s_write(j->fd, buf, len);
    if (k == (ssize_t)len) { j->write_errno = 0; return 0; }
    if (k < 0 && errno == EINTR) continue;
    j->write_errno = k < 0 ? errno : EIO;
    return -1;
  }
}

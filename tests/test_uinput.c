/* uinput: the device setup sequence, the legacy fallback, error advice, node discovery and the
 * events written, all without the kernel: the system calls go to a fake (ujoy_sys) that records the
 * ioctls and sends writes into a pipe, which this test reads back as struct input_event. */
#include "tests/t.h"
#include "uinput.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------- the fake system */
#define MAXCALLS 128
typedef struct { unsigned long req; unsigned long arg; } call_t;
static call_t calls[MAXCALLS];
static int ncalls;
static int open_errno;                    /* != 0: open fails with it */
static int legacy_kernel;                 /* UI_ABS_SETUP / UI_DEV_SETUP answer EINVAL */
static unsigned long fail_req;            /* this ioctl fails with fail_errno */
static int fail_errno;
static int no_sysname;
static int pipe_rd = -1, pipe_wr = -1;
static int closed_fd = -2;
static struct uinput_abs_setup abs_seen[16];
static int nabs;
static struct uinput_setup setup_seen;
static struct uinput_user_dev legacy_seen;
static int legacy_writes;
static char sysname[32] = "input42";
static char sysfs[256];

static int f_open(const char *path, int flags) {
  if (open_errno) { errno = open_errno; return -1; }
  CHECK(flags & O_NONBLOCK);
  CHECK(flags & O_CLOEXEC);
  CHECK((flags & O_ACCMODE) == O_WRONLY);
  return pipe_wr;
}

static int f_ioctl(int fd, unsigned long req, void *arg) {
  CHECK_INT(fd, pipe_wr);
  if (ncalls < MAXCALLS) { calls[ncalls].req = req; calls[ncalls].arg = (unsigned long)(uintptr_t)arg; ncalls++; }
  if (fail_req && req == fail_req) { errno = fail_errno; return -1; }
  if (req == UI_ABS_SETUP) {
    if (legacy_kernel) { errno = EINVAL; return -1; }
    if (nabs < 16) abs_seen[nabs++] = *(struct uinput_abs_setup *)arg;
    return 0;
  }
  if (req == UI_DEV_SETUP) {
    if (legacy_kernel) { errno = EINVAL; return -1; }
    setup_seen = *(struct uinput_setup *)arg;
    return 0;
  }
  if (req == UI_GET_SYSNAME(64)) {
    if (no_sysname) { errno = EINVAL; return -1; }
    snprintf(arg, 64, "%s", sysname);
    return 0;
  }
  return 0;
}

static ssize_t f_write(int fd, const void *b, size_t n) {
  if (n == sizeof(struct uinput_user_dev) && legacy_kernel && legacy_writes == 0) {
    legacy_writes++;
    memcpy(&legacy_seen, b, sizeof legacy_seen);
    return (ssize_t)n;
  }
  return write(fd, b, n);
}

static int f_close(int fd) {
  closed_fd = fd;
  return 0;
}

static ujoy_sys_t FAKE = { f_open, f_ioctl, f_write, f_close, sysfs };

static void fresh(void) {
  if (pipe_rd >= 0) close(pipe_rd);
  if (pipe_wr >= 0) close(pipe_wr);
  int p[2];
  if (pipe(p) != 0) { perror("pipe"); exit(2); }
  pipe_rd = p[0];
  pipe_wr = p[1];
  fcntl(pipe_rd, F_SETFL, O_NONBLOCK);
  ncalls = nabs = legacy_writes = 0;
  open_errno = legacy_kernel = no_sysname = 0;
  fail_req = 0;
  closed_fd = -2;
  memset(&setup_seen, 0, sizeof setup_seen);
  memset(&legacy_seen, 0, sizeof legacy_seen);
  ujoy_sys = &FAKE;
}

static int count_req(unsigned long req, unsigned long arg) {
  int n = 0;
  for (int i = 0; i < ncalls; i++) n += calls[i].req == req && calls[i].arg == arg;
  return n;
}
static int count_any(unsigned long req) {
  int n = 0;
  for (int i = 0; i < ncalls; i++) n += calls[i].req == req;
  return n;
}
static int index_of(unsigned long req) {
  for (int i = 0; i < ncalls; i++) if (calls[i].req == req) return i;
  return -1;
}

/* everything in the pipe, as the kernel would read it */
static int read_events(struct input_event *out, int max) {
  char buf[8192];
  ssize_t n = read(pipe_rd, buf, sizeof buf);
  if (n <= 0) return 0;
  CHECK_INT(n % (ssize_t)sizeof(struct input_event), 0);
  int k = (int)(n / (ssize_t)sizeof(struct input_event));
  if (k > max) k = max;
  memcpy(out, buf, sizeof(struct input_event) * (size_t)k);
  return k;
}

/* ---------------------------------------------------------------- tests */
static void layout(void) {
  /* the kernel's struct input_event, whatever the libc's struct timeval is */
  CHECK_INT(sizeof(ujoy_event_t), sizeof(struct input_event));
  ujoy_event_t u;
  struct input_event ie;
  CHECK_INT((char *)&u.type - (char *)&u, (char *)&ie.type - (char *)&ie);
  CHECK_INT((char *)&u.value - (char *)&u, (char *)&ie.value - (char *)&ie);
}

static void encode(void) {
  joy_event_t ev[3] = { { EV_ABS, ABS_X, 32767 }, { EV_KEY, BTN_TRIGGER, 1 }, { EV_ABS, ABS_RUDDER, 0 } };
  ujoy_event_t out[4];
  memset(out, 0x55, sizeof out);
  CHECK_INT(ujoy_encode(ev, 3, out), 4);
  CHECK_INT(out[0].type, EV_ABS);
  CHECK_INT(out[0].code, ABS_X);
  CHECK_INT(out[0].value, 32767);
  CHECK_INT(out[1].type, EV_KEY);
  CHECK_INT(out[1].code, BTN_TRIGGER);
  CHECK_INT(out[1].value, 1);
  CHECK_INT(out[2].code, ABS_RUDDER);
  CHECK_INT(out[3].type, EV_SYN);
  CHECK_INT(out[3].code, SYN_REPORT);
  CHECK_INT(out[3].value, 0);
  for (int i = 0; i < 4; i++) CHECK(out[i].sec == 0 && out[i].usec == 0);
  CHECK_INT(ujoy_encode(ev, 0, out), 1);       /* a bare SYN_REPORT */
  CHECK_INT(out[0].type, EV_SYN);
  CHECK_INT(ujoy_encode(ev, -1, out), 1);
}

static void create_modern(void) {
  fresh();
  /* the event node, in a fake sysfs */
  char d[512];
  snprintf(d, sizeof d, "%s/devices/virtual/input/input42/event17", sysfs);
  char cmd[2048];
  snprintf(cmd, sizeof cmd, "mkdir -p '%s' '%s/../js3' '%s/../capabilities'", d, d, d);
  CHECK(system(cmd) == 0);
  ujoy_t j;
  char err[300];
  CHECK_INT(ujoy_open(&j, NULL, "My Joystick", true, "pilot", err, sizeof err), 0);
  CHECK_STR(err, "");
  CHECK(j.fd == pipe_wr);
  CHECK(!j.legacy);
  CHECK_STR(j.name, "My Joystick");
  CHECK_STR(j.sysname, "input42");
  CHECK_STR(j.device, "/dev/input/event17");
  /* event types and codes */
  CHECK_INT(count_req(UI_SET_EVBIT, EV_ABS), 1);
  CHECK_INT(count_req(UI_SET_EVBIT, EV_KEY), 1);
  static const int axes[8] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_RZ, ABS_Z, ABS_THROTTLE, ABS_RUDDER };
  for (int i = 0; i < 8; i++) CHECK_INT(count_req(UI_SET_ABSBIT, (unsigned long)axes[i]), 1);
  CHECK_INT(count_any(UI_SET_ABSBIT), 8);
  for (int b = BTN_TRIGGER; b <= BTN_BASE2; b++) CHECK_INT(count_req(UI_SET_KEYBIT, (unsigned long)b), 1);
  CHECK_INT(count_any(UI_SET_KEYBIT), 8);
  /* every axis: 0..32767, fuzz 0, flat 0, starting at the center */
  CHECK_INT(nabs, 8);
  for (int i = 0; i < nabs; i++) {
    CHECK_INT(abs_seen[i].code, axes[i]);
    CHECK_INT(abs_seen[i].absinfo.minimum, 0);
    CHECK_INT(abs_seen[i].absinfo.maximum, 32767);
    CHECK_INT(abs_seen[i].absinfo.fuzz, 0);
    CHECK_INT(abs_seen[i].absinfo.flat, 0);
    CHECK_INT(abs_seen[i].absinfo.resolution, 0);
    CHECK_INT(abs_seen[i].absinfo.value, 16384);
  }
  /* identity: virtual bus, no vendor/product */
  CHECK_INT(setup_seen.id.bustype, BUS_VIRTUAL);
  CHECK_INT(setup_seen.id.vendor, 0);
  CHECK_INT(setup_seen.id.product, 0);
  CHECK_INT(setup_seen.id.version, 1);
  CHECK_STR(setup_seen.name, "My Joystick");
  CHECK_INT(setup_seen.ff_effects_max, 0);
  /* the order the kernel needs: bits and abs setup, device setup, create, then the name */
  CHECK(index_of(UI_ABS_SETUP) < index_of(UI_DEV_SETUP));
  CHECK(index_of(UI_DEV_SETUP) < index_of(UI_DEV_CREATE));
  CHECK(index_of(UI_DEV_CREATE) < index_of(UI_GET_SYSNAME(64)));
  CHECK_INT(count_any(UI_DEV_CREATE), 1);
  /* the modern interface needs no initial write */
  struct input_event ie[64];
  CHECK_INT(read_events(ie, 64), 0);

  /* one frame: changed axes + buttons + SYN_REPORT, in ONE write */
  joymap_t m;
  joymap_init(&m, true);
  uint16_t raw[16];
  for (int i = 0; i < 16; i++) raw[i] = 16384;
  raw[0] = 100;
  raw[7] = 32767;
  raw[8] = 30000;
  joy_event_t ev[JOY_MAX_EVENTS];
  int n = joymap_frame(&m, raw, 16, ev);
  CHECK_INT(n, 3);
  CHECK_INT(ujoy_write(&j, ev, n), 0);
  int k = read_events(ie, 64);
  CHECK_INT(k, 4);
  CHECK(ie[0].type == EV_ABS && ie[0].code == ABS_X && ie[0].value == 100);
  CHECK(ie[1].type == EV_ABS && ie[1].code == ABS_RUDDER && ie[1].value == 32767);
  CHECK(ie[2].type == EV_KEY && ie[2].code == BTN_TRIGGER && ie[2].value == 1);
  CHECK(ie[3].type == EV_SYN && ie[3].code == SYN_REPORT && ie[3].value == 0);
  /* nothing changed: nothing written (not even a SYN_REPORT) */
  CHECK_INT(ujoy_write(&j, ev, joymap_frame(&m, raw, 16, ev)), 0);
  CHECK_INT(read_events(ie, 64), 0);
  /* more than 16 events can't happen; ujoy_write caps them */
  joy_event_t many[40];
  for (int i = 0; i < 40; i++) many[i] = (joy_event_t){ EV_ABS, ABS_X, i };
  CHECK_INT(ujoy_write(&j, many, 40), 0);
  CHECK_INT(read_events(ie, 64), JOY_MAX_EVENTS + 1);
  /* centering */
  n = joymap_reset(&m, ev);
  CHECK_INT(n, 3);
  CHECK_INT(ujoy_write(&j, ev, n), 0);
  k = read_events(ie, 64);
  CHECK_INT(k, 4);
  CHECK(ie[0].value == 16384 && ie[1].value == 16384 && ie[2].type == EV_KEY && ie[2].value == 0);
  /* a failed write: reported, errno kept */
  int keep = j.fd;
  close(pipe_rd);
  pipe_rd = -1;
  signal(SIGPIPE, SIG_IGN);
  CHECK_INT(ujoy_write(&j, ev, 1), -1);
  CHECK_INT(j.write_errno, EPIPE);
  CHECK_INT(keep, pipe_wr);
  ujoy_close(&j);
  CHECK_INT(count_any(UI_DEV_DESTROY), 1);
  CHECK_INT(closed_fd, keep);
  CHECK_INT(j.fd, -1);
  CHECK_STR(j.device, "");
  ujoy_close(&j);                                  /* twice: nothing */
  CHECK_INT(count_any(UI_DEV_DESTROY), 1);
  CHECK_INT(ujoy_write(&j, ev, 1), 0);             /* closed: a no-op */
  ujoy_close(NULL);
  snprintf(cmd, sizeof cmd, "rm -rf '%s/devices'", sysfs);
  CHECK(system(cmd) == 0);
}

static void create_without_buttons_and_node(void) {
  fresh();
  no_sysname = 1;
  ujoy_t j;
  char err[300];
  CHECK_INT(ujoy_open(&j, NULL, "", false, NULL, err, sizeof err), 0);
  CHECK_STR(j.name, "ELRS Crossfire WiFi Joystick");    /* "" = the default name */
  CHECK_INT(count_req(UI_SET_EVBIT, EV_KEY), 0);
  CHECK_INT(count_any(UI_SET_KEYBIT), 0);
  CHECK_STR(j.sysname, "");
  CHECK_STR(j.device, "");                              /* best effort: unknown */
  ujoy_close(&j);
  /* a sysname but no sysfs entry (the 200 ms patience runs out) */
  fresh();
  ujoy_t j2;
  CHECK_INT(ujoy_open(&j2, NULL, "x", false, NULL, err, sizeof err), 0);
  CHECK_STR(j2.sysname, "input42");
  CHECK_STR(j2.device, "");
  ujoy_close(&j2);
}

static void create_legacy(void) {
  fresh();
  legacy_kernel = 1;
  ujoy_t j;
  char err[300];
  CHECK_INT(ujoy_open(&j, "/dev/uinput", "Old Kernel Stick", true, "pilot", err, sizeof err), 0);
  CHECK(j.legacy);
  CHECK_INT(legacy_writes, 1);
  CHECK_STR(legacy_seen.name, "Old Kernel Stick");
  CHECK_INT(legacy_seen.id.bustype, BUS_VIRTUAL);
  CHECK_INT(legacy_seen.id.vendor, 0);
  CHECK_INT(legacy_seen.id.product, 0);
  CHECK_INT(legacy_seen.id.version, 1);
  static const int axes[8] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_RZ, ABS_Z, ABS_THROTTLE, ABS_RUDDER };
  for (int i = 0; i < 8; i++) {
    CHECK_INT(legacy_seen.absmin[axes[i]], 0);
    CHECK_INT(legacy_seen.absmax[axes[i]], 32767);
    CHECK_INT(legacy_seen.absfuzz[axes[i]], 0);
    CHECK_INT(legacy_seen.absflat[axes[i]], 0);
  }
  CHECK_INT(legacy_seen.absmax[ABS_WHEEL], 0);          /* nothing else */
  CHECK_INT(count_any(UI_DEV_SETUP), 0);                /* not tried once UI_ABS_SETUP said no */
  CHECK_INT(count_any(UI_DEV_CREATE), 1);
  /* the old interface starts every axis at 0: the center is written at once */
  struct input_event ie[64];
  int k = read_events(ie, 64);
  CHECK_INT(k, 8 + 8 + 1);
  for (int i = 0; i < 8; i++) CHECK(ie[i].type == EV_ABS && ie[i].code == axes[i] && ie[i].value == 16384);
  for (int i = 8; i < 16; i++) CHECK(ie[i].type == EV_KEY && ie[i].value == 0);
  CHECK(ie[16].type == EV_SYN);
  ujoy_close(&j);
}

static void errors(void) {
  ujoy_t j;
  char err[400];
  fresh();
  open_errno = ENOENT;
  CHECK_INT(ujoy_open(&j, NULL, "x", true, "pilot", err, sizeof err), -1);
  CHECK(strstr(err, "/dev/uinput does not exist") && strstr(err, "sudo modprobe uinput"));
  CHECK_INT(j.fd, -1);
  open_errno = EACCES;
  CHECK_INT(ujoy_open(&j, NULL, "x", true, "pilot", err, sizeof err), -1);
  CHECK(strstr(err, "no permission") && strstr(err, "u:pilot:rw"));
  open_errno = ENODEV;
  CHECK_INT(ujoy_open(&j, "/dev/uinput", "x", true, NULL, err, sizeof err), -1);
  CHECK(strstr(err, "modprobe") != NULL);
  ujoy_close(&j);                                       /* never opened: nothing */
  CHECK_INT(closed_fd, -2);
  /* an ioctl that fails: closed, said which */
  static const struct { unsigned long req; const char *name; } F[] = {
    { UI_SET_EVBIT, "UI_SET_EVBIT" }, { UI_SET_ABSBIT, "UI_SET_ABSBIT" }, { UI_SET_KEYBIT, "UI_SET_KEYBIT" },
    { UI_DEV_SETUP, "UI_DEV_SETUP" }, { UI_DEV_CREATE, "UI_DEV_CREATE" },
  };
  for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) {
    fresh();
    fail_req = F[i].req;
    fail_errno = ENOMEM;
    CHECK_INT(ujoy_open(&j, NULL, "x", true, NULL, err, sizeof err), -1);
    CHECK(strstr(err, F[i].name) != NULL);
    CHECK(strstr(err, "cannot create the virtual joystick") != NULL);
    CHECK_INT(closed_fd, pipe_wr);
    CHECK_INT(j.fd, -1);
  }
  /* UI_ABS_SETUP failing for another reason than an old kernel: an error, no fallback */
  fresh();
  fail_req = UI_ABS_SETUP;
  fail_errno = EPERM;
  CHECK_INT(ujoy_open(&j, NULL, "x", true, NULL, err, sizeof err), -1);
  CHECK(strstr(err, "UI_ABS_SETUP") != NULL);
  /* ENOTTY is an old kernel too */
  fresh();
  fail_req = UI_ABS_SETUP;
  fail_errno = ENOTTY;
  legacy_kernel = 0;
  CHECK_INT(ujoy_open(&j, NULL, "x", true, NULL, err, sizeof err), 0);
  CHECK(j.legacy);
  ujoy_close(&j);
}

static void find_node(void) {
  char out[64], d[512], cmd[3000];
  snprintf(d, sizeof d, "%s/devices/virtual/input/input7", sysfs);
  snprintf(cmd, sizeof cmd, "mkdir -p '%s/js0' '%s/eventX' '%s/event' '%s/event3' '%s/power'", d, d, d, d, d);
  CHECK(system(cmd) == 0);
  CHECK_INT(ujoy_find_node(sysfs, "input7", out, sizeof out), 0);
  CHECK_STR(out, "/dev/input/event3");                  /* not js0, eventX or a bare "event" */
  CHECK_INT(ujoy_find_node(sysfs, "input8", out, sizeof out), -1);
  CHECK_STR(out, "");
  CHECK_INT(ujoy_find_node(sysfs, "", out, sizeof out), -1);
  CHECK_INT(ujoy_find_node(sysfs, NULL, out, sizeof out), -1);
  CHECK_INT(ujoy_find_node(sysfs, "../input7", out, sizeof out), -1);   /* never outside sysfs */
  CHECK_INT(ujoy_find_node(sysfs, "..", out, sizeof out), -1);
  snprintf(cmd, sizeof cmd, "rm -rf '%s/devices'", sysfs);
  CHECK(system(cmd) == 0);
}

int main(void) {
  const char *tmp = getenv("TMPDIR");
  snprintf(sysfs, sizeof sysfs, "%s/test_uinput.XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(sysfs)) { perror("mkdtemp"); return 2; }
  layout();
  encode();
  create_modern();
  create_without_buttons_and_node();
  create_legacy();
  errors();
  find_node();
  rmdir(sysfs);
  T_DONE();
}

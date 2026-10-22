/* The virtual joystick: a Linux uinput device (DESIGN.md section 4).
 *
 * Name configurable (default ENG_DEVICE_NAME), BUS_VIRTUAL, vendor/product 0, version 1; 8 axes
 * 0..32767 with fuzz 0 and flat 0 (the kernel must neither drop small moves nor add a dead zone),
 * starting at the center; buttons BTN_TRIGGER..BTN_BASE2 always declared (without EV_KEY, udev and SDL
 * take a device with X/Y/Z axes for an accelerometer), and with buttons=false never pressed. Created with
 * UI_DEV_SETUP / UI_ABS_SETUP (kernel 4.5+), else the legacy struct uinput_user_dev write. The event
 * node (/dev/input/eventN) is found through UI_GET_SYSNAME and sysfs, best effort.
 *
 * ujoy_write sends one batch (the changed axes/buttons + SYN_REPORT) in one write(): the kernel
 * timestamps the events at that moment, and an application never sees half a frame. */
#ifndef UINPUT_H
#define UINPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "joymap.h"

#define UJOY_PATH "/dev/uinput"

typedef struct {
  int fd;                   /* -1 = none */
  bool buttons;
  bool legacy;              /* made with the pre-4.5 interface */
  char name[64];
  char sysname[64];         /* "input42" when the kernel told us */
  char device[64];          /* "/dev/input/event7" when known, else "" */
  int write_errno;          /* the last write error (0 = none) */
} ujoy_t;

/* The system calls, replaceable by tests (no kernel uinput in CI containers). NULL members = the real
 * call. sysfs: the directory that holds devices/virtual/input ("/sys" when NULL). */
typedef struct {
  int (*open)(const char *path, int flags);
  int (*ioctl)(int fd, unsigned long req, void *arg);
  ssize_t (*write)(int fd, const void *buf, size_t n);
  int (*close)(int fd);
  const char *sysfs;
} ujoy_sys_t;
extern const ujoy_sys_t *ujoy_sys;          /* NULL = the real system */

/* The kernel's struct input_event: two longs of time (ignored on write: the kernel stamps the
 * event), type, code, value. Defined here because 32-bit libcs with a 64-bit time_t disagree about
 * struct timeval; the kernel wants the native long on every architecture (and on 64-bit kernels
 * running 32-bit programs, the 32-bit layout). */
typedef struct { unsigned long sec, usec; uint16_t type, code; int32_t value; } ujoy_event_t;

/* Open path (UJOY_PATH when NULL) and create the device. 0, or -1 with err: what failed and how to
 * fix it (e.g. "/dev/uinput does not exist - load the uinput kernel module: sudo modprobe uinput").
 * user: the account name for the advice ("" = unknown). On success the device starts centered. */
int  ujoy_open(ujoy_t *j, const char *path, const char *name, bool buttons, const char *user, char *err, size_t cap);
/* Remove the device (UI_DEV_DESTROY) and close it. Safe on a closed one. */
void ujoy_close(ujoy_t *j);
/* events + SYN_REPORT in one write; nothing when n == 0. 0 ok, -1 failed (errno in j->write_errno). */
int  ujoy_write(ujoy_t *j, const joy_event_t *ev, int n);

/* pure: events -> the bytes of one write (n events + SYN_REPORT). out must hold n + 1. Returns n + 1. */
int  ujoy_encode(const joy_event_t *ev, int n, ujoy_event_t *out);
/* "/dev/input/eventN" of a uinput device with that sysname, from <sysfs>/devices/virtual/input/<sysname>/;
 * 0 found, -1 not (out ""). */
int  ujoy_find_node(const char *sysfs, const char *sysname, char *out, size_t cap);

#endif

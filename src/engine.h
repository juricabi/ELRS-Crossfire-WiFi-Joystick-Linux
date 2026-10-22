/* The receiver: one thread owns the UDP socket, the protocol engine (core/wifi_proto) and the
 * virtual joystick; a worker thread sends the activation POSTs (core/wifi_net). The CLI and the web
 * server talk to it only through this API: commands in, snapshots and log lines out. Every function
 * here is thread-safe and returns at once (none waits on the network); the receive thread applies
 * commands within one loop pass (at most ~100 ms, at once when a command wakes it).
 *
 * Latency path: a datagram wakes the receive thread (poll on the socket), wifi_proto_packet applies
 * it, and its channels go to the virtual joystick in the same pass (one write: axes, buttons,
 * SYN_REPORT). Nothing else runs between the socket and the joystick. */
#ifndef ENGINE_H
#define ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wifi_proto.h"

/* ---------------------------------------------------------------- settings that change live */
typedef struct {
  char module_ip[64];       /* dotted IPv4 of the module, "" = discover it by its beacon */
  uint16_t port;            /* UDP port to listen on, 1..65535 (11000: the module always sends there) */
  uint16_t http_port;       /* the module's web server, 1..65535 (80) */
  int interval_us;          /* the request: WIFI_INTERVAL_MIN..WIFI_INTERVAL_MAX (4000 = 250 Hz) */
  int channels;             /* the request: WIFI_MIN_CH..WIFI_MAX_CH (16) */
} eng_config_t;

void eng_config_default(eng_config_t *c);     /* "", 11000, 80, 4000, 16 */
/* 0 when every field is supported, else -1 and one line in err naming the field and the supported
 * values, e.g. "channels: 3 is not supported - use 4 to 16" or "module_ip: \"10.0.0\" is not an IPv4
 * address - use a.b.c.d, or leave it empty to find the module by its broadcast". */
int  eng_config_check(const eng_config_t *c, char *err, size_t cap);

/* ---------------------------------------------------------------- lifetime */
#define ENG_DEVICE_NAME "ELRS Crossfire WiFi Joystick"

typedef struct {
  bool joystick;            /* create the virtual joystick (uinput); false = monitor only */
  bool buttons;             /* CH9..CH16 as buttons 1..8 */
  char device_name[64];     /* "" = ENG_DEVICE_NAME */
  /* called (on the caller's thread) by eng_set_config after it accepted a config; NULL = not saved.
   * 0 = saved; else -1 with err (the config still applies). */
  int (*save)(void *ud, const eng_config_t *c, char *err, size_t cap);
  void *save_ud;
} eng_options_t;

typedef struct engine engine_t;

/* Creates the virtual joystick (when asked; a failure is not fatal: monitor only, joy_error says why
 * and how to fix it), starts the receive and worker threads, and starts listening (eng_start). cfg
 * must pass eng_config_check. NULL (with err) only when out of memory or a thread can't start. */
engine_t *eng_create(const eng_config_t *cfg, const eng_options_t *opt, char *err, size_t cap);
/* Stop, center the joystick and remove it, end the threads (a POST in flight is aborted), free. */
void eng_destroy(engine_t *e);

/* ---------------------------------------------------------------- commands */
void eng_start(engine_t *e);         /* listen (a new protocol session); no-op while running */
void eng_stop(engine_t *e);          /* close the socket, forget the lock; the joystick is centered */
/* Check (eng_config_check), then apply live and save (opt.save): module_ip -> new target (or
 * discovery); port -> rebind; interval/channels -> wifi_proto_set_request (a streaming ELRS module
 * is restarted so they apply); http_port -> the next POST. Returns 0, or -1 with err and nothing
 * changed. A save failure is logged and reported in err with return 0. */
int  eng_set_config(engine_t *e, const eng_config_t *c, char *err, size_t cap);
void eng_restart_stream(engine_t *e);   /* wifi_proto_restart: re-send our request to the module */

/* ---------------------------------------------------------------- state */
typedef struct {
  uint32_t seq;             /* changes whenever anything below changes */
  eng_config_t cfg;         /* in effect */
  bool running;             /* started (listening, or retrying the bind) */
  bool listening;           /* socket bound */
  wifi_state_t state;
  char detail[64];          /* wifi_proto's state detail */
  /* one line for people: "stopped", "listening", "activating 10.0.0.1", "connecting to 10.0.0.1",
   * "no data from 10.0.0.1", "no stick data from 10.0.0.1", "streaming", "UDP port 11000 in use" */
  char status[80];
  bool bound;               /* locked to a module */
  char source[16];          /* its IP, "" when not bound */
  wifi_kind_t kind;         /* its kind (from its beacons this session), UNKNOWN when not bound */
  char module_name[32];     /* its ELRS device name from the beacon, or "" */
  double rate_hz, jitter_ms;   /* the last 1 s window (0 when not bound) */
  int frames_last_s;
  uint8_t nch;              /* channels in the last applied frame (0 = none yet) */
  uint16_t raw[WIFI_MAX_CH];   /* the last applied frame, 0..32767, 0 beyond nch */
  uint32_t frames;          /* applied frames, lifetime */
  uint32_t rx, beacons, malformed, dropped_filter, dropped_other;
  uint32_t posts, posts_ok, posts_failed, restarts;
  uint32_t gaps;            /* pauses of 250 ms or more in the stream of the locked module */
  int64_t gap_max_us;
  /* the virtual joystick */
  bool joy_enabled;         /* asked for */
  bool joy_ok;              /* exists */
  bool joy_buttons;
  char joy_name[64];
  char joy_device[64];      /* "/dev/input/event7" when known, else "" */
  char joy_error[256];      /* why there is none, with the fix ("" when ok or not asked for) */
  uint32_t log_seq;         /* seq of the newest log line (0 = none) */
  int64_t uptime_us;
} eng_snapshot_t;

void eng_snapshot(engine_t *e, eng_snapshot_t *out);
/* Block until the snapshot seq differs from seq or a log line newer than log_seq exists, at most
 * timeout_ms. true when something changed. (The CLI's pace; the web server polls instead.) */
bool eng_wait(engine_t *e, uint32_t seq, uint32_t log_seq, int timeout_ms);

/* ---------------------------------------------------------------- log */
#define ENG_LOG_LINES 512
#define ENG_LOG_LEN 320
typedef struct { uint32_t seq; int64_t wall_ms; char text[ENG_LOG_LEN]; } eng_logline_t;

/* Lines with seq > since, oldest first, at most max; returns how many. The ring keeps the newest
 * ENG_LOG_LINES: a gap (first seq > since + 1) means older ones are gone. */
int  eng_log_read(engine_t *e, uint32_t since, eng_logline_t *out, int max);
/* One line from any thread (control characters become '?', longer lines are cut). */
void eng_log(engine_t *e, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;

#endif

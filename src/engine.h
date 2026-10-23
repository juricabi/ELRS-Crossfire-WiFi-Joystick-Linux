/* The receiver: one thread owns the UDP socket, the protocol engine (core/wifi_proto) and the
 * virtual joystick; a worker thread sends the activation POSTs and a stop's releases (core/wifi_net).
 * The CLI and the web server talk to it only through this API: commands in, snapshots and log lines
 * out. Every function here is thread-safe and returns at once (none waits on the network; the eng_wait*
 * ones and eng_destroy wait as they say); the receive thread applies commands within one loop pass (at
 * most ~100 ms, at once when a command wakes it).
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

/* the fields of eng_config_t, for eng_update_config (the same bits as config.h's CFG_K_* live keys) */
#define ENG_F_MODULE_IP  (1u << 0)
#define ENG_F_PORT       (1u << 1)
#define ENG_F_HTTP_PORT  (1u << 2)
#define ENG_F_INTERVAL   (1u << 3)
#define ENG_F_CHANNELS   (1u << 4)
#define ENG_F_ALL        0x1Fu

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
  /* called (on the caller's thread) by eng_update_config / eng_set_config after it accepted a config;
   * NULL = not saved.
   * 0 = saved; else -1 with err (the config still applies). */
  int (*save)(void *ud, const eng_config_t *c, char *err, size_t cap);
  void *save_ud;
} eng_options_t;

typedef struct engine engine_t;

/* Creates the virtual joystick (when asked; a failure is not fatal: monitor only, joy_error says why
 * and how to fix it), starts the receive and worker threads, and starts listening (eng_start). cfg
 * must pass eng_config_check. NULL (with err) only when out of memory or a thread can't start. */
engine_t *eng_create(const eng_config_t *cfg, const eng_options_t *opt, char *err, size_t cap);
/* Stop (the module released, as by eng_stop), center the joystick and remove it, end the threads, free.
 * Before the worker ends it waits for the releases still under way, at most WIFI_RELEASE_WAIT_US (1.5 s)
 * after the last stop that handed releases over (eng_wait_released has the same end, so the CLI's call of it
 * before this one adds nothing: an exit waits that long in all), logging their results; what is left then is
 * cut short (logged as failed), as is any other POST in flight. */
void eng_destroy(engine_t *e);

/* ---------------------------------------------------------------- commands
 * Commands are kept as state, never queued, so none is ever lost however many come at once: a stop
 * given since the last pass is applied first (a stop and a start = a new session), then the latest
 * config, then the last of start/stop, then a restart. */
void eng_start(engine_t *e);         /* listen (a new protocol session); no-op while running */
/* Close the socket, forget the lock, center the joystick, and release the module (R14 of core/wifi_proto.h):
 * ELRS streams until it gets joystick_end and beacons only while idle, so a module left streaming to us could
 * not be found by any other app. joystick_end alone goes to the locked module, then to every module that
 * streams to us while we drop its frames (a frame of it less than 3 s ago, and it is a 2nd source, sends
 * placeholder frames, or is not the module IP, e.g. the module left by a switch), then to the modules
 * activated less than 3 s ago that have not locked since (on the worker; logged: "Releasing <ip>: ...", then
 * "Released <ip>: ..." or "Releasing <ip> failed: ..."), except one known as Crossfire by its beacon ("Not
 * releasing <ip>: ..."). Each joystick_end gets 2 s for its answer (eng_wait_released and the exit may cut it
 * short sooner), then fails ("timed out after 2 s"): what queues behind it on the worker (a start's
 * joystick_begin) waits that long at most. A port change does the same before it listens on the new port. */
void eng_stop(engine_t *e);
/* Change the fields in `fields` (ENG_F_*) to c's values, the others staying as the latest accepted
 * config has them; the merge happens under the engine's lock, so concurrent callers (the CLI, the web
 * page) never undo each other's fields. Check (eng_config_check), then apply live and save (opt.save):
 * module_ip -> new target (or discovery); port -> rebind; interval/channels -> wifi_proto_set_request
 * (a streaming ELRS module is restarted so they apply); http_port -> the next POST. Returns 0 (out, when
 * not NULL, gets the whole config now asked for, module_ip in its plain form), or -1 with err and
 * nothing changed. A save failure is logged and reported in err with return 0. */
int  eng_update_config(engine_t *e, const eng_config_t *c, unsigned fields, eng_config_t *out, char *err, size_t cap);
int  eng_set_config(engine_t *e, const eng_config_t *c, char *err, size_t cap);   /* every field */
void eng_restart_stream(engine_t *e);   /* wifi_proto_restart: re-send our request to the module */
/* Block until every command given before this call (start, stop, restart, a config) has been applied
 * and its snapshot published, at most timeout_ms. true when it has. (For a script's commands in a row:
 * "s" twice is a stop, then a start; "status" after "stop" says stopped.) */
bool eng_wait_applied(engine_t *e, int timeout_ms);
/* Block until the commands given before this call are applied (as eng_wait_applied: a stop has handed its
 * releases over) and no release is under way (snapshot releasing 0: each joystick_end answered or failed, and
 * logged), at most timeout_ms and at most WIFI_RELEASE_WAIT_US after the last stop that handed releases over
 * (that stop's time, not this call's: a wait that starts 1 s after the stop has 0.5 s left, one that starts
 * later none); true when none was left by then. What is left then is cut short and logged as failed, so no
 * joystick_end is still on its way (that takes at most ~100 ms more): the one in flight as "Releasing <ip>
 * failed: no answer in time - it may stream on until its WiFi or power is cycled", one still queued behind it
 * as "... failed: not sent, the wait ran out before its turn - ...". For the last log lines before the program
 * ends (the CLI at quit) and before the process stops (Ctrl+Z, also after a stop). */
bool eng_wait_released(engine_t *e, int timeout_ms);
/* eng_wait_released's wait, a log line at a time, cutting nothing short: 1 when it is over (no release under way,
 * as eng_wait_released's true), 0 as soon as the log has a line after log_seq, -1 when timeout_ms or the 1.5 s
 * after the stop have run out first. For showing the releases' lines as they come while it waits (the CLI at quit
 * and Ctrl+Z); eng_wait_released after it cuts short what is left. */
int  eng_wait_releasing(engine_t *e, uint32_t log_seq, int timeout_ms);

/* ---------------------------------------------------------------- state */
typedef struct {
  uint32_t seq;             /* changes whenever anything below changes */
  eng_config_t cfg;         /* in effect */
  bool running;             /* started (listening, or retrying the bind) */
  bool listening;           /* socket bound */
  wifi_state_t state;
  char detail[64];          /* wifi_proto's state detail */
  /* one line for people, every text there is: "stopped"; "opening UDP port 11000" (running, the bind
   * not done yet), "UDP port 11000 in use", "no permission for UDP port 11000", "cannot open UDP port
   * 11000" (the bind failed: state error, retried every 5 s); "listening", "activating 10.0.0.1" (a POST
   * under way; with a module IP set, only its own), "connecting to 10.0.0.1", "no data from 10.0.0.1",
   * "no stick data from 10.0.0.1"; "streaming" */
  char status[80];
  bool bound;               /* locked to a module */
  char source[16];          /* its IP, "" when not bound */
  wifi_kind_t kind;         /* its kind (from its beacons this session), UNKNOWN when not bound */
  char module_name[32];     /* its ELRS device name from the beacon, or "" */
  double rate_hz, jitter_ms;   /* the last 1 s window (0 when not bound) */
  int frames_last_s;
  uint8_t nch;              /* channels in the last applied frame (0 = none yet, or no module locked) */
  uint16_t raw[WIFI_MAX_CH];   /* the last applied frame, 0..32767, 0 beyond nch */
  uint32_t frames;          /* applied frames, lifetime */
  uint32_t rx, beacons, malformed, dropped_filter, dropped_other;
  uint32_t posts, posts_ok, posts_failed, restarts;   /* activation POSTs (a restart's joystick_end + joystick_begin
                                                     * is one: ok when both got a 2xx), their results, restarts
                                                     * sent; a release's joystick_end is none of them */
  int releasing;            /* releases (R14, a stop's joystick_end) queued, in flight or not logged yet */
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

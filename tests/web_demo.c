/* The web page with a fake engine: src/web.c serving the real page, sticks that move by themselves.
 * For working on the page and for tests/test_ui.py when the real program is not there. Not built by
 * build.sh; tests/test_ui.py builds it, or by hand (gen/webpage.h as build.sh makes it):
 *
 *   cc -std=gnu99 -O2 -D_GNU_SOURCE -Icore -Isrc -Ibuild/native/gen -o web_demo \
 *      tests/web_demo.c src/web.c core/wifi_proto.c core/wifi_net.c -lpthread -lm
 *   ./web_demo [--addr 127.0.0.1] [--port 11000] [--kind elrs|xf|wifi] [--no-joystick] [--joy-error TEXT]
 *
 * It prints "URL <url>", then takes commands on stdin (one per line, each answered "ok" or "error ..."):
 *   kind elrs|xf|wifi        the module's kind          present on|off    a module answers (or not)
 *   state <st> [status...]   force a state (streaming, searching, stopped, error) and its status text
 *   name <text>              ELRS device name           ip <a.b.c.d>      the module's address
 *   joy ok|off|err <text>    the virtual joystick (\n in text: a new line)   buttons on|off  CH9-16 as buttons
 *   raw <ch 1-16> <0..32767|auto>   pin a channel       nch <n|auto>      channels per frame
 *   log <text>               a log line                 warn <text>       the next saves "fail" with this
 *   restart                  as if the program restarted: a new web server (a new instance), the log and
 *                            the uptime from 0
 *   quit
 * End of input keeps it running (Ctrl+C ends it). */
#include "engine.h"
#include "web.h"
#include "wifi_net.h"
#include "wifi_proto.h"

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct engine {
  pthread_mutex_t mu;
  eng_config_t cfg;
  bool running, present, forced;
  wifi_state_t state;
  char status[80], detail[64], forced_status[80];
  wifi_kind_t kind;
  char ip[16], name[32];
  bool joy_enabled, joy_ok, buttons;
  char joy_error[256];
  int pin[WIFI_MAX_CH];          /* -1 = animated */
  int nch_override;              /* 0 = from the request (ELRS) or 16 (Crossfire) */
  char warn[160];
  uint32_t seq, restarts, posts, frames_base;
  int64_t t0_us, stream_t0_us;
  eng_logline_t log[ENG_LOG_LINES];
  uint32_t log_seq;
};

static engine_t E;
static volatile sig_atomic_t quit_sig;

static void on_sig(int s) { (void)s; quit_sig = 1; }

static void dlog(engine_t *e, const char *fmt, ...) {
  char buf[ENG_LOG_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  eng_logline_t *l = &e->log[e->log_seq % ENG_LOG_LINES];
  l->seq = ++e->log_seq;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  l->wall_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  snprintf(l->text, sizeof l->text, "%s", buf);
  for (char *p = l->text; *p; p++) if ((unsigned char)*p < 0x20) *p = '?';
  e->seq++;
}

/* the state that follows from running/present unless one was forced */
static void settle(engine_t *e) {
  if (e->forced) {
    snprintf(e->status, sizeof e->status, "%s", e->forced_status);
    return;
  }
  if (!e->running) {
    e->state = WIFI_ST_STOPPED;
    snprintf(e->status, sizeof e->status, "stopped");
  } else if (e->present) {
    if (e->state != WIFI_ST_STREAMING) e->stream_t0_us = wifi_net_now_us();
    e->state = WIFI_ST_STREAMING;
    snprintf(e->status, sizeof e->status, "streaming");
  } else {
    e->state = WIFI_ST_SEARCHING;
    if (e->cfg.module_ip[0]) snprintf(e->status, sizeof e->status, "connecting to %s", e->cfg.module_ip);
    else snprintf(e->status, sizeof e->status, "listening");
  }
  snprintf(e->detail, sizeof e->detail, "%.63s", e->state == WIFI_ST_STREAMING ? e->ip : e->status);
  e->seq++;
}

/* ---------------------------------------------------------------- engine.h */
void eng_config_default(eng_config_t *c) {
  memset(c, 0, sizeof *c);
  c->port = WIFI_PORT_DEFAULT;
  c->http_port = WIFI_HTTP_PORT_DEFAULT;
  c->interval_us = WIFI_INTERVAL_DEFAULT;
  c->channels = WIFI_CHANNELS_DEFAULT;
}

int eng_config_check(const eng_config_t *c, char *err, size_t cap) {
  uint32_t ip;
  if (c->module_ip[0] && wifi_parse_ip(c->module_ip, &ip) != 1) {
    snprintf(err, cap, "module_ip: \"%s\" is not an IPv4 address - use a.b.c.d, or leave it empty to find the "
             "module by its broadcast", c->module_ip);
    return -1;
  }
  if (!c->port) { snprintf(err, cap, "port: 0 is not supported - use 1 to 65535"); return -1; }
  if (!c->http_port) { snprintf(err, cap, "http_port: 0 is not supported - use 1 to 65535"); return -1; }
  if (c->interval_us < WIFI_INTERVAL_MIN || c->interval_us > WIFI_INTERVAL_MAX) {
    snprintf(err, cap, "interval_us: %d is not supported - use %d to %d", c->interval_us, WIFI_INTERVAL_MIN, WIFI_INTERVAL_MAX);
    return -1;
  }
  if (c->channels < WIFI_MIN_CH || c->channels > WIFI_MAX_CH) {
    snprintf(err, cap, "channels: %d is not supported - use %d to %d", c->channels, WIFI_MIN_CH, WIFI_MAX_CH);
    return -1;
  }
  return 0;
}

engine_t *eng_create(const eng_config_t *cfg, const eng_options_t *opt, char *err, size_t cap) { (void)cfg; (void)opt; (void)err; (void)cap; return &E; }
void eng_destroy(engine_t *e) { (void)e; }

void eng_start(engine_t *e) {
  pthread_mutex_lock(&e->mu);
  if (!e->running) {
    e->running = true;
    e->forced = false;
    dlog(e, "Listening for joystick data on UDP %u", (unsigned)e->cfg.port);
    if (e->present) {
      e->posts++;
      dlog(e, "Joystick source locked to %s", e->ip);
    }
    settle(e);
  }
  pthread_mutex_unlock(&e->mu);
}

void eng_stop(engine_t *e) {
  pthread_mutex_lock(&e->mu);
  if (e->running) {
    e->running = false;
    e->forced = false;
    dlog(e, "Stopped");
    settle(e);
  }
  pthread_mutex_unlock(&e->mu);
}

int eng_update_config(engine_t *e, const eng_config_t *cin, unsigned fields, eng_config_t *out, char *err, size_t cap) {
  pthread_mutex_lock(&e->mu);
  eng_config_t old = e->cfg, n = e->cfg;
  pthread_mutex_unlock(&e->mu);
  if (fields & ENG_F_MODULE_IP) memcpy(n.module_ip, cin->module_ip, sizeof n.module_ip);
  if (fields & ENG_F_PORT) n.port = cin->port;
  if (fields & ENG_F_HTTP_PORT) n.http_port = cin->http_port;
  if (fields & ENG_F_INTERVAL) n.interval_us = cin->interval_us;
  if (fields & ENG_F_CHANNELS) n.channels = cin->channels;
  if (eng_config_check(&n, err, cap)) return -1;
  uint32_t ip;
  char plain[16] = "";
  if (wifi_parse_ip(n.module_ip, &ip) == 1) wifi_ip_str(ip, plain);
  memset(n.module_ip, 0, sizeof n.module_ip);
  memcpy(n.module_ip, plain, strlen(plain));
  if (out) *out = n;
  const eng_config_t *c = &n;
  pthread_mutex_lock(&e->mu);
  e->cfg = *c;
  if (strcmp(old.module_ip, c->module_ip))
    dlog(e, c->module_ip[0] ? "Module IP set to %s" : "Module IP cleared: finding the module by its broadcast%s", c->module_ip);
  if (old.port != c->port) dlog(e, "Listening for joystick data on UDP %u", (unsigned)c->port);
  if (old.interval_us != c->interval_us || old.channels != c->channels) {
    if (e->running && e->present && e->kind == WIFI_KIND_XF) {
      dlog(e, "Crossfire ignores the request (always 16 channels, about 90 Hz)");
    } else if (e->running && e->present) {
      e->restarts++;
      e->posts += 2;
      dlog(e, "Restarting the stream of %s: %d Hz, %d channels", e->ip, wifi_interval_hz(c->interval_us), c->channels);
    }
  }
  settle(e);
  snprintf(err, cap, "%s", e->warn);
  if (e->warn[0]) dlog(e, "%s", e->warn);
  pthread_mutex_unlock(&e->mu);
  return 0;
}

int eng_set_config(engine_t *e, const eng_config_t *c, char *err, size_t cap) {
  return eng_update_config(e, c, ENG_F_ALL, NULL, err, cap);
}

bool eng_wait_applied(engine_t *e, int timeout_ms) { (void)e; (void)timeout_ms; return true; }

void eng_restart_stream(engine_t *e) {
  pthread_mutex_lock(&e->mu);
  if (e->running && e->present && e->kind == WIFI_KIND_XF) {
    dlog(e, "Crossfire ignores the request: nothing to restart");
  } else if (e->running && (e->present || e->cfg.module_ip[0])) {
    e->restarts++;
    e->posts += 2;
    dlog(e, "Restarting the stream of %s: %d Hz, %d channels", e->present ? e->ip : e->cfg.module_ip,
         wifi_interval_hz(e->cfg.interval_us), e->cfg.channels);
  }
  pthread_mutex_unlock(&e->mu);
}

static int animated(int ch, double t) {
  if (ch < 4) return (int)lround(16383.5 + 16383.5 * sin(t * (0.6 + 0.25 * ch) + ch));
  if (ch < 8) return (int)lround(16383.5 + 8000.0 * sin(t * 0.15 + ch));
  return fmod(t + ch * 0.7, 4.0 + (ch - 8) * 0.5) < 2.0 ? 32767 : 0;          /* switches */
}

void eng_snapshot(engine_t *e, eng_snapshot_t *o) {
  pthread_mutex_lock(&e->mu);
  memset(o, 0, sizeof *o);
  int64_t now = wifi_net_now_us();
  double t = (double)(now - e->t0_us) / 1e6;
  o->cfg = e->cfg;
  o->running = e->running;
  o->listening = e->running && e->state != WIFI_ST_ERROR;     /* as the engine: an error state is a failed bind */
  o->state = e->state;
  snprintf(o->detail, sizeof o->detail, "%s", e->detail);
  snprintf(o->status, sizeof o->status, "%s", e->status);
  bool streaming = e->state == WIFI_ST_STREAMING && e->present;
  o->bound = e->running && e->present && (e->state == WIFI_ST_STREAMING || e->forced);
  if (o->bound) {
    snprintf(o->source, sizeof o->source, "%s", e->ip);
    o->kind = e->kind;
    snprintf(o->module_name, sizeof o->module_name, "%s", e->kind == WIFI_KIND_ELRS ? e->name : "");
  }
  if (streaming) {
    bool xf = e->kind == WIFI_KIND_XF;
    double hz = xf ? 90.9 : 1e6 / e->cfg.interval_us;
    o->rate_hz = hz * (1.0 - 0.004 * (0.5 + 0.5 * sin(t * 3.1)));
    o->jitter_ms = 0.25 + 0.2 * (0.5 + 0.5 * sin(t * 1.7));
    o->frames_last_s = (int)lround(o->rate_hz);
    int n = e->nch_override ? e->nch_override : xf ? WIFI_XF_CHANNELS : e->cfg.channels;
    o->nch = (uint8_t)n;
    for (int i = 0; i < n; i++) o->raw[i] = (uint16_t)(e->pin[i] >= 0 ? e->pin[i] : animated(i, t));
    double secs = (double)(now - e->stream_t0_us) / 1e6;
    o->frames = e->frames_base + (uint32_t)(secs * hz);
  } else {
    o->frames = e->frames_base;
  }
  o->rx = o->frames + 3;
  o->dropped_filter = 7;                         /* distinct numbers: each shows under its own label */
  o->dropped_other = 11;
  o->beacons = (uint32_t)(t / 5) + 1;
  o->posts = e->posts;
  o->posts_ok = e->posts;
  o->restarts = e->restarts;
  o->joy_enabled = e->joy_enabled;
  o->joy_ok = e->joy_enabled && e->joy_ok;
  o->joy_buttons = e->buttons;
  snprintf(o->joy_name, sizeof o->joy_name, "%s", ENG_DEVICE_NAME);
  if (o->joy_ok) snprintf(o->joy_device, sizeof o->joy_device, "/dev/input/event7");
  if (e->joy_enabled && !e->joy_ok) snprintf(o->joy_error, sizeof o->joy_error, "%s", e->joy_error);
  o->log_seq = e->log_seq;
  o->uptime_us = now - e->t0_us;
  /* changes all the time while streaming */
  o->seq = e->seq + (streaming ? (uint32_t)(t * 100) : 0);
  pthread_mutex_unlock(&e->mu);
}

bool eng_wait(engine_t *e, uint32_t seq, uint32_t log_seq, int timeout_ms) {
  (void)e; (void)seq; (void)log_seq;
  usleep((useconds_t)timeout_ms * 1000);
  return true;
}

int eng_log_read(engine_t *e, uint32_t since, eng_logline_t *out, int max) {
  int k = 0;
  pthread_mutex_lock(&e->mu);
  uint32_t first = e->log_seq > ENG_LOG_LINES ? e->log_seq - ENG_LOG_LINES + 1 : 1;
  for (uint32_t s = since + 1 > first ? since + 1 : first; s <= e->log_seq && k < max; s++)
    out[k++] = e->log[(s - 1) % ENG_LOG_LINES];         /* logl stores seq s at (s - 1) % ENG_LOG_LINES */
  pthread_mutex_unlock(&e->mu);
  return k;
}

void eng_log(engine_t *e, const char *fmt, ...) {
  char buf[ENG_LOG_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  pthread_mutex_lock(&e->mu);
  dlog(e, "%s", buf);
  pthread_mutex_unlock(&e->mu);
}

/* ---------------------------------------------------------------- commands */
static int command(engine_t *e, char *line) {
  char *arg = strchr(line, ' ');
  if (arg) *arg++ = 0;
  else arg = line + strlen(line);
  int rc = 0;
  pthread_mutex_lock(&e->mu);
  if (!strcmp(line, "kind")) {
    if (!strcmp(arg, "elrs")) e->kind = WIFI_KIND_ELRS;
    else if (!strcmp(arg, "xf")) e->kind = WIFI_KIND_XF;
    else if (!strcmp(arg, "wifi")) e->kind = WIFI_KIND_UNKNOWN;
    else rc = -1;
    e->seq++;
  } else if (!strcmp(line, "present")) {
    e->present = !strcmp(arg, "on");
    if (e->present && e->running) dlog(e, "Joystick source locked to %s", e->ip);
    if (!e->present && e->running) dlog(e, "Joystick source %s lost (no data for 3 s)", e->ip);
    settle(e);
  } else if (!strcmp(line, "state")) {
    char *st = arg, *text = strchr(arg, ' ');
    if (text) *text++ = 0;
    int s = !strcmp(st, "streaming") ? WIFI_ST_STREAMING : !strcmp(st, "searching") ? WIFI_ST_SEARCHING :
            !strcmp(st, "stopped") ? WIFI_ST_STOPPED : !strcmp(st, "error") ? WIFI_ST_ERROR : -1;
    if (s < 0) rc = -1;
    else {
      e->forced = true;
      e->state = (wifi_state_t)s;
      snprintf(e->forced_status, sizeof e->forced_status, "%s", text ? text : st);
      settle(e);
    }
  } else if (!strcmp(line, "name")) {
    snprintf(e->name, sizeof e->name, "%s", arg);
    e->seq++;
  } else if (!strcmp(line, "ip")) {
    uint32_t ip;
    if (wifi_parse_ip(arg, &ip) == 1) { snprintf(e->ip, sizeof e->ip, "%s", arg); settle(e); }
    else rc = -1;
  } else if (!strcmp(line, "joy")) {
    char *text = strchr(arg, ' ');
    if (text) *text++ = 0;
    e->joy_enabled = strcmp(arg, "off") != 0;
    e->joy_ok = !strcmp(arg, "ok");
    snprintf(e->joy_error, sizeof e->joy_error, "%s", text ? text : "");
    for (char *q = e->joy_error; (q = strstr(q, "\\n")); q++) {       /* "\n" = a new line (the fix) */
      *q = '\n';
      memmove(q + 1, q + 2, strlen(q + 2) + 1);
    }
    e->seq++;
  } else if (!strcmp(line, "buttons")) {
    e->buttons = !strcmp(arg, "on");
    e->seq++;
  } else if (!strcmp(line, "raw")) {
    int ch = 0, v = -1;
    char word[16] = "";
    if (sscanf(arg, "%d %15s", &ch, word) != 2 || ch < 1 || ch > WIFI_MAX_CH) rc = -1;
    else if (!strcmp(word, "auto")) e->pin[ch - 1] = -1;
    else if (sscanf(word, "%d", &v) == 1 && v >= 0 && v <= WIFI_RAW_MAX) e->pin[ch - 1] = v;
    else rc = -1;
  } else if (!strcmp(line, "nch")) {
    e->nch_override = !strcmp(arg, "auto") ? 0 : atoi(arg);
    if (e->nch_override < 0 || e->nch_override > WIFI_MAX_CH) { e->nch_override = 0; rc = -1; }
  } else if (!strcmp(line, "log")) {
    dlog(e, "%s", arg);
  } else if (!strcmp(line, "warn")) {
    snprintf(e->warn, sizeof e->warn, "%s", arg);
  } else if (!strcmp(line, "quit")) {
    rc = 1;
  } else if (line[0]) {
    rc = -1;
  }
  pthread_mutex_unlock(&e->mu);
  return rc;
}

int main(int argc, char **argv) {
  const char *addr = "127.0.0.1";
  int port = 0;
  engine_t *e = &E;
  pthread_mutex_init(&e->mu, NULL);
  eng_config_default(&e->cfg);
  e->kind = WIFI_KIND_ELRS;
  e->present = true;
  e->joy_enabled = e->joy_ok = e->buttons = true;
  snprintf(e->ip, sizeof e->ip, "127.0.0.2");
  snprintf(e->name, sizeof e->name, "Unified TX");
  for (int i = 0; i < WIFI_MAX_CH; i++) e->pin[i] = -1;
  e->t0_us = wifi_net_now_us();
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--addr") && i + 1 < argc) addr = argv[++i];
    else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--kind") && i + 1 < argc) {
      const char *k = argv[++i];
      e->kind = !strcmp(k, "xf") ? WIFI_KIND_XF : !strcmp(k, "wifi") ? WIFI_KIND_UNKNOWN : WIFI_KIND_ELRS;
    } else if (!strcmp(argv[i], "--no-joystick")) e->joy_enabled = false;
    else if (!strcmp(argv[i], "--joy-error") && i + 1 < argc) { e->joy_ok = false; snprintf(e->joy_error, sizeof e->joy_error, "%s", argv[++i]); }
    else { fprintf(stderr, "usage: %s [--addr A] [--port N] [--kind elrs|xf|wifi] [--no-joystick] [--joy-error TEXT]\n", argv[0]); return 2; }
  }
  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);
  dlog(e, "elrs-wifi-joystick web demo (fake engine)");
  e->running = false;
  eng_start(e);
  char err[200], url[64];
  web_t *w = web_start(e, addr, (uint16_t)port, err, sizeof err);
  if (!w) { fprintf(stderr, "%s\n", err); return 1; }
  web_url(w, url, sizeof url);
  printf("URL %s\n", url);
  fflush(stdout);
  unsigned bound = 0;
  sscanf(strrchr(url, ':') + 1, "%u", &bound);
  char line[512];
  while (!quit_sig && fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    if (!strcmp(line, "restart")) {
      web_stop(w);
      pthread_mutex_lock(&e->mu);
      e->log_seq = 0;
      e->t0_us = wifi_net_now_us();
      dlog(e, "elrs-wifi-joystick web demo (fake engine), restarted");
      pthread_mutex_unlock(&e->mu);
      w = web_start(e, addr, (uint16_t)bound, err, sizeof err);
      if (!w) { fprintf(stderr, "%s\n", err); return 1; }
      printf("ok\n");
      fflush(stdout);
      continue;
    }
    int rc = command(e, line);
    if (rc > 0) break;
    printf(rc ? "error %s\n" : "ok\n", line);
    fflush(stdout);
  }
  while (!quit_sig && feof(stdin)) usleep(100000);       /* end of input: keep serving until a signal */
  web_stop(w);
  return 0;
}

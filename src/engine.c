/* The receiver: see engine.h. One receive thread owns the socket, wifi_proto and the joystick; one
 * worker thread sends the activation POSTs and does what the receive thread must not (file reads,
 * getifaddrs). Everything else reaches them through the command queue (mu), the activation queue
 * (qmu) and the snapshot + log ring (smu).
 *
 * Ported from sticktime-pi host/src/input_wifi.c (the same author's, MIT here): the socket, the drain
 * with kernel timestamps, the worker, activation logging, the status text and the gap log. That follows
 * JoystickEngine.cs of the Windows app. */
#include "engine.h"
#include "hints.h"
#include "joymap.h"
#include "uinput.h"
#include "wifi_net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define DRAIN_MAX 256                 /* datagrams per pass: a flood can't starve the tick */
#define BIND_TRIES 5
#define BIND_RETRY_US 300000
#define BIND_ERROR_RETRY_US 5000000
#define RECV_ERR_LOG_US 60000000
#define TICK_MS 100                   /* longest poll: lock release, hints and re-POSTs need a tick */
#define GAP_US 250000                 /* a pause in the stick data this long is counted (and logged) */
#define GAP_LOG_US 5000000            /* at most one gap line per this */
#define RCVBUF (256 * 1024)
#define SIGNAL_PERIOD_MS 2000         /* the WiFi signal sample for the gap log, while streaming */
#define TS_MAX_AGE_US 2000000         /* an older kernel receive time is not believed */

struct engine {
  eng_options_t opt;
  int64_t t0;

  /* ---- the receive thread's (eng_create's before the thread starts) ---- */
  wifi_proto_t p;
  eng_config_t cfg;               /* applied */
  int sock;
  bool running;
  int bind_tries;
  int64_t bind_at;
  bool bind_error;
  int64_t now;                    /* of the current pass (hooks) */
  int64_t last_arr;
  int last_err;
  int64_t last_err_us;
  bool was_bound;
  ujoy_t joy;
  bool joy_ok;
  bool joy_write_failed;
  char joy_error[256];
  joymap_t jm;
  uint32_t name_ip;
  wifi_kind_t name_kind;
  char name[48];                  /* "ELRS 192.168.1.50" */
  int64_t gap_prev;               /* arrival of the previous frame of gap_src, 0 = none */
  uint32_t gap_src;
  /* our own restart (joystick_end, then joystick_begin) pauses the stream: a pause that overlaps one
   * (queued at rs_at, its result taken at rs_done_at) is no gap */
  uint32_t rs_ip;
  int64_t rs_at, rs_done_at;      /* rs_at 0 = none */
  bool rs_done;
  uint32_t gaps, gaps_unlogged;
  int64_t gap_max_us, gap_logged_at;
  uint32_t posts, posts_ok, posts_failed;
  uint8_t buf[2048];
  eng_snapshot_t work;            /* built each pass, published when it differs */
  uint32_t cmd_taken;             /* cmd_seq of the commands applied in this pass */

  /* ---- commands (mu): what was asked, as state (nothing is ever dropped, however many come) ---- */
  pthread_mutex_t mu;
  bool stop_req;                  /* a stop was asked since the last pass (applied first) */
  int run_req;                    /* the last of eng_start / eng_stop: 1 start, 0 stop, -1 none */
  bool restart_req;
  bool cfg_req_new;               /* cfg_req is to be applied */
  uint32_t cmd_seq;               /* commands given so far (eng_wait_applied) */
  bool quit;
  eng_config_t cfg_req;           /* the latest accepted config */
  pthread_mutex_t set_mu;         /* one config update at a time (merges and saves in order) */
  int wake_rd, wake_wr;

  /* ---- the worker (qmu) ---- */
  pthread_mutex_t qmu;
  pthread_cond_t qcv;
  wifi_actq_t q;
  bool wquit;
  bool want_addrs;                /* a bind happened: log this machine's addresses, look at the firewall */
  bool want_signal;               /* streaming: sample the WiFi signal for the gap log */
  int signal_dbm;                 /* 0 = unknown */
  hints_fw_t fw;
  pthread_t wth, rth;
  bool wth_ok, rth_ok;

  /* ---- snapshot and log (smu) ---- */
  pthread_mutex_t smu;
  pthread_cond_t scv;
  eng_snapshot_t snap;
  uint32_t cmd_done;              /* cmd_seq of the commands applied and published */
  eng_logline_t ring[ENG_LOG_LINES];
  uint32_t log_seq;
};

/* ================================================================ config */
void eng_config_default(eng_config_t *c) {
  memset(c, 0, sizeof *c);
  c->port = WIFI_PORT_DEFAULT;
  c->http_port = WIFI_HTTP_PORT_DEFAULT;
  c->interval_us = WIFI_INTERVAL_DEFAULT;
  c->channels = WIFI_CHANNELS_DEFAULT;
}

int eng_config_check(const eng_config_t *c, char *err, size_t cap) {
  if (cap) err[0] = 0;
  uint32_t ip = 0;
  if (!memchr(c->module_ip, 0, sizeof c->module_ip)) {
    snprintf(err, cap, "module_ip: too long - use a.b.c.d, or leave it empty to find the module by its broadcast");
    return -1;
  }
  int r = wifi_parse_ip(c->module_ip, &ip);
  if (r < 0 || (r > 0 && ip == 0)) {
    char safe[48];
    size_t k = 0;
    for (const char *s = c->module_ip; *s && k < 40; s++) safe[k++] = (*s >= 0x20 && *s < 0x7f) ? *s : '?';
    safe[k] = 0;
    snprintf(err, cap, "module_ip: \"%s\" is not an IPv4 address - use a.b.c.d, or leave it empty to find the "
             "module by its broadcast", safe);
    return -1;
  }
  if (c->port == 0) { snprintf(err, cap, "port: 0 is not supported - use 1 to 65535"); return -1; }
  if (c->http_port == 0) { snprintf(err, cap, "http_port: 0 is not supported - use 1 to 65535"); return -1; }
  if (c->interval_us < WIFI_INTERVAL_MIN || c->interval_us > WIFI_INTERVAL_MAX) {
    snprintf(err, cap, "interval_us: %d is not supported - use %d to %d", c->interval_us, WIFI_INTERVAL_MIN,
             WIFI_INTERVAL_MAX);
    return -1;
  }
  if (c->channels < WIFI_MIN_CH || c->channels > WIFI_MAX_CH) {
    snprintf(err, cap, "channels: %d is not supported - use %d to %d", c->channels, WIFI_MIN_CH, WIFI_MAX_CH);
    return -1;
  }
  return 0;
}

/* the checked config with module_ip in its plain form (" 10.0.0.1 " -> "10.0.0.1") */
static void config_normalize(eng_config_t *c) {
  uint32_t ip = 0;
  char s[16] = "";
  if (wifi_parse_ip(c->module_ip, &ip) == 1) wifi_ip_str(ip, s);
  /* whole-struct compares (snapshot changes) must not see bytes after the string */
  memset(c->module_ip, 0, sizeof c->module_ip);
  memcpy(c->module_ip, s, strlen(s));
}

/* ================================================================ log */
/* Control characters (C0, DEL and C1) and bytes that are not UTF-8 become '?': every reader (a
 * terminal, the JSON of the web page) gets one clean line. */
static void clean_text(char *s, size_t len) {
  for (size_t i = 0; i < len;) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
      if (c < 0x20 || c == 0x7f) s[i] = '?';
      i++;
      continue;
    }
    size_t n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    bool ok = n && i + n <= len;
    for (size_t k = 1; ok && k < n; k++) ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
    if (ok) {
      unsigned char d = (unsigned char)s[i + 1];
      /* overlong forms, UTF-16 surrogates, beyond U+10FFFF */
      if ((c == 0xE0 && d < 0xA0) || (c == 0xED && d >= 0xA0) || (c == 0xF0 && d < 0x90) || (c == 0xF4 && d >= 0x90))
        ok = false;
    }
    if (!ok) {
      s[i++] = '?';
      continue;
    }
    if (c == 0xC2 && (unsigned char)s[i + 1] < 0xA0) s[i] = s[i + 1] = '?';     /* U+0080..U+009F */
    i += n;
  }
}

void eng_log(engine_t *e, const char *fmt, ...) {
  if (!e) return;
  char line[ENG_LOG_LEN * 2];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  size_t len = strlen(line);
  if (len > ENG_LOG_LEN - 1) {
    len = ENG_LOG_LEN - 1;
    /* never half a UTF-8 character: line[len] is the first byte left out */
    while (len > 0 && ((unsigned char)line[len] & 0xC0) == 0x80) len--;
    line[len] = 0;
  }
  clean_text(line, len);
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  int64_t wall = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  pthread_mutex_lock(&e->smu);
  uint32_t seq = ++e->log_seq;
  eng_logline_t *l = &e->ring[seq % ENG_LOG_LINES];
  l->seq = seq;
  l->wall_ms = wall;
  memcpy(l->text, line, len + 1);
  e->snap.log_seq = seq;
  e->snap.seq++;
  pthread_cond_broadcast(&e->scv);
  pthread_mutex_unlock(&e->smu);
}

int eng_log_read(engine_t *e, uint32_t since, eng_logline_t *out, int max) {
  if (!e || max <= 0) return 0;
  pthread_mutex_lock(&e->smu);
  uint32_t newest = e->log_seq;
  uint32_t oldest = newest >= ENG_LOG_LINES ? newest - ENG_LOG_LINES + 1 : 1;
  int k = 0;
  /* since < newest: since + 1 cannot wrap (since = 4294967295 asks for nothing) */
  uint32_t s = since >= newest ? 0 : since + 1 < oldest ? oldest : since + 1;
  for (; s && s <= newest && k < max; s++) out[k++] = e->ring[s % ENG_LOG_LINES];
  pthread_mutex_unlock(&e->smu);
  return k;
}

/* ================================================================ snapshot */
void eng_snapshot(engine_t *e, eng_snapshot_t *out) {
  pthread_mutex_lock(&e->smu);
  memcpy(out, &e->snap, sizeof *out);
  pthread_mutex_unlock(&e->smu);
  out->uptime_us = wifi_net_now_us() - e->t0;
}

static void abs_deadline(struct timespec *ts, int ms) {
  clock_gettime(CLOCK_MONOTONIC, ts);
  ts->tv_sec += ms / 1000;
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

bool eng_wait(engine_t *e, uint32_t seq, uint32_t log_seq, int timeout_ms) {
  if (timeout_ms < 0) timeout_ms = 0;
  struct timespec dl;
  abs_deadline(&dl, timeout_ms);
  pthread_mutex_lock(&e->smu);
  while (e->snap.seq == seq && e->snap.log_seq <= log_seq) {
    if (pthread_cond_timedwait(&e->scv, &e->smu, &dl) == ETIMEDOUT) break;
  }
  bool changed = e->snap.seq != seq || e->snap.log_seq > log_seq;
  pthread_mutex_unlock(&e->smu);
  return changed;
}

/* ================================================================ wake / commands */
static void wake(engine_t *e) {
  char c = 1;
  /* a full pipe already means "wake up" */
  if (write(e->wake_wr, &c, 1) < 0) { /* EAGAIN */ }
}

/* A command is state, not a queue entry: any number of them in a burst is applied, the last start/stop
 * wins (a stop before it still ends the session first), and a config is the latest one. */
void eng_start(engine_t *e) {
  if (!e) return;
  pthread_mutex_lock(&e->mu);
  e->run_req = 1;
  e->cmd_seq++;
  pthread_mutex_unlock(&e->mu);
  wake(e);
}

void eng_stop(engine_t *e) {
  if (!e) return;
  pthread_mutex_lock(&e->mu);
  e->run_req = 0;
  e->stop_req = true;
  e->cmd_seq++;
  pthread_mutex_unlock(&e->mu);
  wake(e);
}

void eng_restart_stream(engine_t *e) {
  if (!e) return;
  pthread_mutex_lock(&e->mu);
  e->restart_req = true;
  e->cmd_seq++;
  pthread_mutex_unlock(&e->mu);
  wake(e);
}

bool eng_wait_applied(engine_t *e, int timeout_ms) {
  pthread_mutex_lock(&e->mu);
  uint32_t want = e->cmd_seq;
  pthread_mutex_unlock(&e->mu);
  if (timeout_ms < 0) timeout_ms = 0;
  struct timespec dl;
  abs_deadline(&dl, timeout_ms);
  pthread_mutex_lock(&e->smu);
  while ((int32_t)(e->cmd_done - want) < 0)
    if (pthread_cond_timedwait(&e->scv, &e->smu, &dl) == ETIMEDOUT) break;
  bool done = (int32_t)(e->cmd_done - want) >= 0;
  pthread_mutex_unlock(&e->smu);
  return done;
}

int eng_update_config(engine_t *e, const eng_config_t *cin, unsigned fields, eng_config_t *out, char *err, size_t cap) {
  if (cap) err[0] = 0;
  /* merged onto the latest accepted config under set_mu: another update (the CLI, the web page) between
   * a caller's snapshot and this call is never undone by fields this one did not give */
  pthread_mutex_lock(&e->set_mu);
  pthread_mutex_lock(&e->mu);
  eng_config_t c = e->cfg_req;
  pthread_mutex_unlock(&e->mu);
  if (fields & ENG_F_MODULE_IP) memcpy(c.module_ip, cin->module_ip, sizeof c.module_ip);
  if (fields & ENG_F_PORT) c.port = cin->port;
  if (fields & ENG_F_HTTP_PORT) c.http_port = cin->http_port;
  if (fields & ENG_F_INTERVAL) c.interval_us = cin->interval_us;
  if (fields & ENG_F_CHANNELS) c.channels = cin->channels;
  if (eng_config_check(&c, err, cap)) {
    pthread_mutex_unlock(&e->set_mu);
    return -1;
  }
  config_normalize(&c);
  pthread_mutex_lock(&e->mu);
  e->cfg_req = c;
  e->cfg_req_new = true;
  e->cmd_seq++;
  pthread_mutex_unlock(&e->mu);
  pthread_mutex_lock(&e->smu);
  if (memcmp(&e->snap.cfg, &c, sizeof c)) {
    memcpy(&e->snap.cfg, &c, sizeof c);
    e->snap.seq++;
    pthread_cond_broadcast(&e->scv);
  }
  pthread_mutex_unlock(&e->smu);
  wake(e);
  if (out) *out = c;
  if (e->opt.save) {
    char serr[256] = "";
    if (e->opt.save(e->opt.save_ud, &c, serr, sizeof serr) != 0) {
      if (!serr[0]) snprintf(serr, sizeof serr, "the settings could not be saved");
      eng_log(e, "Settings not saved: %s", serr);
      snprintf(err, cap, "%s", serr);
    }
  }
  pthread_mutex_unlock(&e->set_mu);
  return 0;
}

int eng_set_config(engine_t *e, const eng_config_t *c, char *err, size_t cap) {
  return eng_update_config(e, c, ENG_F_ALL, NULL, err, cap);
}

/* ================================================================ worker */
typedef struct { engine_t *e; uint32_t session; } abort_ctx_t;

static bool post_aborted(void *ud) {
  abort_ctx_t *a = ud;
  pthread_mutex_lock(&a->e->qmu);
  bool r = a->e->wquit || wifi_actq_stale(&a->e->q, a->session);
  pthread_mutex_unlock(&a->e->qmu);
  return r;
}

/* the WiFi signal in dBm from /proc/net/wireless (first interface listed), 0 = none */
static int wifi_signal_dbm(void) {
  FILE *f = fopen("/proc/net/wireless", "re");
  if (!f) return 0;
  char line[256];
  int dbm = 0;
  for (int i = 0; fgets(line, sizeof line, f); i++) {
    if (i < 2) continue;                                     /* two header lines */
    char ifn[32];
    unsigned st;
    float link, level;
    if (sscanf(line, " %31[^:]: %x %f %f", ifn, &st, &link, &level) == 4) { dbm = (int)level; break; }
  }
  fclose(f);
  return dbm < 0 ? dbm : 0;
}

static void *worker_main(void *arg) {
  engine_t *e = arg;
  struct timespec next_sig = { 0, 0 };
  pthread_mutex_lock(&e->qmu);
  while (!e->wquit) {
    if (e->want_addrs) {
      e->want_addrs = false;
      pthread_mutex_unlock(&e->qmu);
      char a[400];
      int n = wifi_net_local_ipv4(a, sizeof a);
      hints_fw_t fw = hints_detect_firewall(NULL);
      if (n) eng_log(e, "This machine: %s", a);
      if (n > 1) eng_log(e, "Several networks - the module must be on one of them.");
      if (!n) eng_log(e, "This machine has no network address besides loopback - connect it to the module's WiFi.");
      pthread_mutex_lock(&e->qmu);
      e->fw = fw;
      continue;
    }
    wifi_actreq_t rq;
    if (wifi_actq_take(&e->q, &rq)) {
      pthread_mutex_unlock(&e->qmu);
      wifi_actres_t rs;
      abort_ctx_t ac = { e, rq.session };
      wifi_net_post(&rq, &rs, WIFI_POST_TIMEOUT_MS, post_aborted, &ac);
      pthread_mutex_lock(&e->qmu);
      wifi_actq_put(&e->q, &rs);        /* dropped when the session changed meanwhile */
      wake(e);
      continue;
    }
    if (e->want_signal) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec > next_sig.tv_sec || (now.tv_sec == next_sig.tv_sec && now.tv_nsec >= next_sig.tv_nsec)) {
        pthread_mutex_unlock(&e->qmu);
        int dbm = wifi_signal_dbm();
        pthread_mutex_lock(&e->qmu);
        e->signal_dbm = dbm;
        abs_deadline(&next_sig, SIGNAL_PERIOD_MS);
        continue;
      }
      pthread_cond_timedwait(&e->qcv, &e->qmu, &next_sig);
    } else {
      pthread_cond_wait(&e->qcv, &e->qmu);
    }
  }
  pthread_mutex_unlock(&e->qmu);
  return NULL;
}

/* ================================================================ joystick (receive thread) */
static void joy_send(engine_t *e, const joy_event_t *ev, int n) {
  if (!e->joy_ok || n <= 0) return;
  if (ujoy_write(&e->joy, ev, n) == 0) {
    if (e->joy_write_failed) {
      e->joy_write_failed = false;
      e->joy_error[0] = 0;
      eng_log(e, "The virtual joystick takes the sticks again");
    }
    return;
  }
  if (e->joy_write_failed) return;              /* said once */
  e->joy_write_failed = true;
  snprintf(e->joy_error, sizeof e->joy_error, "writing to the virtual joystick failed: %s", strerror(e->joy.write_errno));
  eng_log(e, "!! Writing to the virtual joystick failed: %s - it may not follow the sticks",
          strerror(e->joy.write_errno));
}

static void joy_center(engine_t *e) {
  if (!e->joy_ok) return;
  joy_event_t ev[JOY_MAX_EVENTS];
  int k = joymap_reset(&e->jm, ev);
  /* after a failed write the device may lack earlier changes: send everything until one gets through */
  if (e->joy_write_failed) k = joymap_full(&e->jm, ev);
  joy_send(e, ev, k);
}

/* ================================================================ protocol hooks (receive thread) */
static void h_log(void *ud, const char *msg) { eng_log(ud, "%s", msg); }

static void h_state(void *ud, wifi_state_t s, const char *detail) {
  engine_t *e = ud;
  /* the core's "activated, but no data" hint: on Linux the firewall is the usual suspect */
  if (s == WIFI_ST_SEARCHING && !strncmp(detail, "activated", 9)) {
    pthread_mutex_lock(&e->qmu);
    hints_fw_t fw = e->fw;
    pthread_mutex_unlock(&e->qmu);
    char line[300];
    hints_firewall(fw, e->cfg.port, line, sizeof line);
    eng_log(e, "!! %s", line);
  }
}

/* A pause in the stream: a WiFi scan, interference, or the module. The log says when, so it can be
 * matched with what else happened; the signal comes from the worker's last sample (no file I/O here). */
static void note_gap(engine_t *e, int64_t gap_us, int64_t now) {
  e->gaps++;
  if (gap_us > e->gap_max_us) e->gap_max_us = gap_us;
  if (e->gap_logged_at && now - e->gap_logged_at < GAP_LOG_US) { e->gaps_unlogged++; return; }
  char more[64] = "", sig[48] = "";
  if (e->gaps_unlogged) snprintf(more, sizeof more, ", %u shorter or earlier ones not logged", e->gaps_unlogged);
  pthread_mutex_lock(&e->qmu);
  int dbm = e->signal_dbm;
  pthread_mutex_unlock(&e->qmu);
  if (dbm) snprintf(sig, sizeof sig, ", this machine's WiFi signal %d dBm", dbm);
  eng_log(e, "The stick data paused for %.2f s (%s%s%s)", (double)gap_us / 1e6, e->name, sig, more);
  e->gaps_unlogged = 0;
  e->gap_logged_at = now;
}

static void h_frame(void *ud, uint32_t src, const uint16_t *raw, int n) {
  engine_t *e = ud;
  /* the joystick first: nothing else stands between the datagram and the device */
  if (e->joy_ok) {
    joy_event_t ev[JOY_MAX_EVENTS];
    int k = joymap_frame(&e->jm, raw, n, ev);
    if (e->joy_write_failed) k = joymap_full(&e->jm, ev);    /* see joy_center */
    joy_send(e, ev, k);
  }
  int64_t arr = e->p.last_arrival_us;           /* this frame's (set just before the hook) */
  /* a pause that overlaps our own restart of this module (it ends after the restart was queued and began
   * before its result was in) is that restart's, not the WiFi's: at a slow rate its last frame often came
   * before the restart was even sent (ELRS stops at once on joystick_end) */
  bool ours = src == e->rs_ip && e->rs_at && arr >= e->rs_at && (!e->rs_done || e->gap_prev <= e->rs_done_at);
  if (e->gap_prev && src == e->gap_src && !ours && arr - e->gap_prev >= GAP_US) note_gap(e, arr - e->gap_prev, e->now);
  e->gap_prev = arr;
  e->gap_src = src;
  wifi_kind_t k = wifi_proto_kind(&e->p, src);
  if (src != e->name_ip || k != e->name_kind || !e->name[0]) {
    char ip[16];
    wifi_ip_str(src, ip);
    snprintf(e->name, sizeof e->name, "%s %s", wifi_kind_name(k), ip);
    e->name_ip = src;
    e->name_kind = k;
  }
}

static int h_activate(void *ud, const wifi_act_t *a) {
  engine_t *e = ud;
  if (!e->wth_ok) return WIFI_ACT_FAILED;
  if (e->sock < 0) return WIFI_ACT_BUSY;        /* not listening: POST after the bind */
  char body[64];
  if (wifi_build_body(body, sizeof body, "joystick_begin", a->interval_us, a->channels) < 0) return WIFI_ACT_FAILED;
  pthread_mutex_lock(&e->qmu);
  bool was_pending = wifi_actq_is_pending(&e->q, a->ip);
  int r = wifi_actq_push(&e->q, a->ip, e->cfg.http_port, body, a->restart);
  if (r == WIFI_ACT_QUEUED) pthread_cond_signal(&e->qcv);
  pthread_mutex_unlock(&e->qmu);
  /* a restart that took over a queued request is still one POST */
  if (r == WIFI_ACT_QUEUED && !was_pending) e->posts++;
  if (r == WIFI_ACT_QUEUED && a->restart) {
    e->rs_ip = a->ip;                             /* under way until its result is in (collect_results) */
    e->rs_at = e->now;
    e->rs_done = false;
  }
  return r;
}

/* ================================================================ socket (receive thread) */
static void log_bind_error(engine_t *e, int err) {
  char why[64], detail[64];
  wifi_net_err_text(err, why, sizeof why);
  if (err == EADDRINUSE) {
    eng_log(e, "ERROR: cannot listen on UDP port %u - it is already in use (another instance, or another app on "
               "this port). Trying again every 5 s.", e->cfg.port);
    snprintf(detail, sizeof detail, "UDP port %u in use", e->cfg.port);
  } else if (err == EACCES) {
    eng_log(e, "ERROR: cannot listen on UDP port %u: permission denied (ports below 1024 need root). "
               "Trying again every 5 s.", e->cfg.port);
    snprintf(detail, sizeof detail, "no permission for UDP port %u", e->cfg.port);
  } else {
    eng_log(e, "ERROR: cannot listen on UDP port %u: %s. Trying again every 5 s.", e->cfg.port, why[0] ? why : "failed");
    snprintf(detail, sizeof detail, "cannot open UDP port %u", e->cfg.port);
  }
  wifi_proto_set_state(&e->p, WIFI_ST_ERROR, detail);
}

static void try_bind(engine_t *e, int64_t now) {
  int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_UDP);
  int err = 0;
  if (s >= 0) {
    int rb = RCVBUF, one = 1;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
    setsockopt(s, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof one);
    /* no SO_REUSEADDR: a second instance must fail here, not share the stream */
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(e->cfg.port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
      err = errno;
      close(s);
      s = -1;
    }
  } else {
    err = errno;
  }
  if (s < 0) {
    if (++e->bind_tries < BIND_TRIES) { e->bind_at = now + BIND_RETRY_US; return; }
    e->bind_at = now + BIND_ERROR_RETRY_US;
    if (!e->bind_error) {
      e->bind_error = true;
      log_bind_error(e, err);
    }
    return;
  }
  e->sock = s;
  e->bind_tries = 0;
  e->bind_error = false;
  eng_log(e, "Listening for joystick data on UDP %u", e->cfg.port);
  pthread_mutex_lock(&e->qmu);
  e->want_addrs = true;
  pthread_cond_signal(&e->qcv);
  pthread_mutex_unlock(&e->qmu);
  wifi_proto_set_state(&e->p, WIFI_ST_SEARCHING, "waiting for module");
  if (e->p.target) wifi_proto_ensure(&e->p, e->p.target, now);
}

static void recv_error(engine_t *e, int err, int64_t now) {
  if (err == e->last_err && now - e->last_err_us < RECV_ERR_LOG_US) return;
  char t[64];
  wifi_net_err_text(err, t, sizeof t);
  eng_log(e, "Receive error: %s", t);
  e->last_err = err;
  e->last_err_us = now;
}

/* Drain what is queued, in order (the newest valid frame wins). Each datagram carries its kernel
 * receive time: stamping a whole batch with one poll time would make the rate and jitter meaningless. */
static void drain(engine_t *e, int64_t now) {
  int64_t mono = 0, real_ns = 0;
  bool clocks = false;
  for (int i = 0; i < DRAIN_MAX && e->sock >= 0; i++) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    struct iovec iov = { e->buf, sizeof e->buf };
    /* room for two timestamps: on 32-bit, musl appends a 64-bit copy of the kernel's old one */
    union { struct cmsghdr h; char b[128]; } ctl;
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_name = &sa;
    m.msg_namelen = sizeof sa;
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl.b;
    m.msg_controllen = sizeof ctl.b;
    ssize_t n = recvmsg(e->sock, &m, MSG_DONTWAIT);
    if (n < 0) {
      int err = errno;
      if (err == EAGAIN || err == EWOULDBLOCK) break;
      if (err == EINTR || err == ECONNREFUSED) continue;
      recv_error(e, err, now);
      break;
    }
    int64_t arrival = now;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"      /* musl's CMSG_NXTHDR */
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
#pragma GCC diagnostic pop
      if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_TIMESTAMPNS ||
          c->cmsg_len < CMSG_LEN(sizeof(struct timespec)))
        continue;
      struct timespec ts;
      memcpy(&ts, CMSG_DATA(c), sizeof ts);
      if (!clocks) {
        struct timespec rn;
        clock_gettime(CLOCK_REALTIME, &rn);
        mono = wifi_net_now_us();
        real_ns = (int64_t)rn.tv_sec * 1000000000 + rn.tv_nsec;
        clocks = true;
      }
      int64_t age_us = (real_ns - ((int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec)) / 1000;
      if (age_us < 0) age_us = 0;
      /* a wall clock step, or a time that can't be right (emulators): the poll time it is */
      if (age_us <= TS_MAX_AGE_US) arrival = mono - age_us;
      break;
    }
    if (sa.sin_family != AF_INET) continue;
    if (arrival < e->last_arr) arrival = e->last_arr;
    e->last_arr = arrival;
    wifi_proto_packet(&e->p, e->buf, (size_t)n, sa.sin_addr.s_addr, now, arrival);
  }
}

static void collect_results(engine_t *e, int64_t now) {
  wifi_actres_t res[WIFI_ACTQ];
  int n = 0;
  pthread_mutex_lock(&e->qmu);
  while (n < WIFI_ACTQ && wifi_actq_get(&e->q, &res[n])) n++;
  pthread_mutex_unlock(&e->qmu);
  for (int i = 0; i < n; i++) {
    const wifi_actres_t *r = &res[i];
    char ip[16];
    wifi_ip_str(r->ip, ip);
    /* a restart: its joystick_end first (a failure there still lets the joystick_begin go out). It only took
     * when both got a 2xx: a streaming module that missed the joystick_end just moved its stream and kept its
     * old request (r->ok is the joystick_begin's alone); the core sends a failed restart again */
    bool ok = r->ok;
    if (r->restart) {
      bool end_ok = r->end_status >= 200 && r->end_status <= 299;
      if (end_ok) eng_log(e, "Stream end POST -> %s: %d", ip, r->end_status);
      else if (r->end_status > 0) eng_log(e, "Stream end POST failed for %s: HTTP %d", ip, r->end_status);
      else eng_log(e, "Stream end POST failed for %s: %s", ip, r->end_err[0] ? r->end_err : "failed");
      ok = ok && end_ok;
    }
    if (r->status > 0)
      eng_log(e, "Activation POST -> %s: %d%s%s", ip, r->status, r->reason[0] ? " " : "", r->reason);
    else
      eng_log(e, "Activation failed for %s: %s", ip, r->err[0] ? r->err : "failed");
    if (ok) e->posts_ok++;
    else e->posts_failed++;
    if (r->restart && r->ip == e->rs_ip && !e->rs_done) {
      e->rs_done = true;
      e->rs_done_at = now;
    }
    wifi_proto_activation_done(&e->p, r->ip, ok, now);
  }
}

/* ================================================================ lifecycle (receive thread) */
static void do_start(engine_t *e, int64_t now) {
  if (e->running) return;
  pthread_mutex_lock(&e->qmu);
  wifi_actq_new_session(&e->q);
  pthread_mutex_unlock(&e->qmu);
  wifi_proto_reset(&e->p);
  e->running = true;
  e->bind_tries = 0;
  e->bind_error = false;
  e->last_arr = 0;
  e->gap_prev = 0;
  e->rs_at = 0;
  /* running, not listening yet: searching (the quick bind retries), not the stopped state of before */
  char d[64];
  snprintf(d, sizeof d, "opening UDP port %u", e->cfg.port);
  wifi_proto_set_state(&e->p, WIFI_ST_SEARCHING, d);
  try_bind(e, now);
}

static void do_stop(engine_t *e, bool quiet) {
  if (!e->running) return;
  e->running = false;
  if (e->sock >= 0) close(e->sock);
  e->sock = -1;
  pthread_mutex_lock(&e->qmu);
  wifi_actq_new_session(&e->q);       /* the worker aborts a request in flight */
  e->want_signal = false;
  pthread_mutex_unlock(&e->qmu);
  wifi_proto_reset(&e->p);
  wifi_proto_set_state(&e->p, WIFI_ST_STOPPED, "");
  joy_center(e);
  e->was_bound = false;
  e->gap_prev = 0;
  e->rs_at = 0;
  if (!quiet) eng_log(e, "Stopped. (The module keeps broadcasting until powered off.)");
}

/* the module IP of e->cfg as the target: at once while running, else at the next bind (rebinding: the
 * listening that starts right after this, with the new port) */
static void apply_target(engine_t *e, int64_t now, bool rebinding) {
  uint32_t ip = 0;
  bool set = wifi_parse_ip(e->cfg.module_ip, &ip) == 1 && ip;
  if (e->running) {
    if (set) wifi_proto_set_target(&e->p, e->cfg.module_ip, now);
    else {
      wifi_proto_set_target(&e->p, NULL, now);
      eng_log(e, "Module IP cleared - finding the module by its broadcast");
    }
    return;
  }
  e->p.target = set ? ip : 0;
  if (rebinding) {
    if (set) eng_log(e, "Module IP set to %s", e->cfg.module_ip);
    else eng_log(e, "Module IP cleared - finding the module by its broadcast");
  } else {
    if (set) eng_log(e, "Module IP set to %s (used when listening starts)", e->cfg.module_ip);
    else eng_log(e, "Module IP cleared - the module will be found by its broadcast");
  }
}

static void apply_config(engine_t *e, const eng_config_t *n, int64_t now) {
  eng_config_t old = e->cfg;
  bool port = n->port != old.port, rebind = port && e->running;
  if (rebind) do_stop(e, true);
  e->cfg = *n;
  if (n->interval_us != old.interval_us || n->channels != old.channels) {
    eng_log(e, "Asking for %d Hz (%d us) and %d channels", wifi_interval_hz(n->interval_us), n->interval_us,
            n->channels);
    /* a new module IP while locked to another module: that one is being left, so it is not restarted for
     * nothing; the new target's joystick_begin (apply_target, below) asks for the request */
    uint32_t nip = 0;
    bool leaving = strcmp(n->module_ip, old.module_ip) && e->running && e->p.bound &&
                   wifi_parse_ip(n->module_ip, &nip) == 1 && nip && nip != e->p.bound_ip;
    if (leaving) wifi_proto_store_request(&e->p, n->interval_us, n->channels);
    else wifi_proto_set_request(&e->p, n->interval_us, n->channels, now);
  }
  if (strcmp(n->module_ip, old.module_ip)) apply_target(e, now, rebind);
  if (n->http_port != old.http_port) eng_log(e, "The module's web server port is %u now", n->http_port);
  if (port) {
    eng_log(e, "UDP port %u%s", n->port, rebind ? "" : " (used when listening starts)");
    if (rebind) do_start(e, now);
  }
}

/* true: quit. Order: a stop (the session ends), the config, a start, a restart. */
static bool run_commands(engine_t *e, int64_t now) {
  pthread_mutex_lock(&e->mu);
  bool stop = e->stop_req, cfg_new = e->cfg_req_new, restart = e->restart_req;
  int run = e->run_req;
  e->stop_req = e->cfg_req_new = e->restart_req = false;
  e->run_req = -1;
  e->cmd_taken = e->cmd_seq;
  bool quit = e->quit;
  eng_config_t req = e->cfg_req;
  pthread_mutex_unlock(&e->mu);
  if (stop) do_stop(e, false);
  if (cfg_new && memcmp(&req, &e->cfg, sizeof req)) apply_config(e, &req, now);
  if (run == 1 && !e->running) do_start(e, now);
  if (restart) {
    /* stopped: the Windows app's line; running but the UDP port not open yet (the bind is retried) */
    if (!e->running) eng_log(e, "Not running - nothing to restart");
    else if (e->sock < 0) eng_log(e, "Not listening - nothing to restart");
    else wifi_proto_restart(&e->p, now);
  }
  return quit;
}

/* ================================================================ status */
/* pending_ip: the module shown as "activating" (activating_ip), 0 = none */
static void status_text(engine_t *e, uint32_t pending_ip, char *out, size_t cap) {
  const wifi_proto_t *p = &e->p;
  char ip[16];
  if (!e->running) { snprintf(out, cap, "stopped"); return; }
  if (p->state == WIFI_ST_ERROR) { snprintf(out, cap, "%s", p->detail); return; }
  if (e->sock < 0) { snprintf(out, cap, "opening UDP port %u", e->cfg.port); return; }
  if (p->state == WIFI_ST_STREAMING) { snprintf(out, cap, "streaming"); return; }
  if (!strncmp(p->detail, "module found", 12)) {
    wifi_ip_str(p->nostick_ip, ip);
    snprintf(out, cap, "no stick data from %s", ip);
  } else if (pending_ip) {
    wifi_ip_str(pending_ip, ip);
    snprintf(out, cap, "activating %s", ip);
  } else if (!strncmp(p->detail, "activated", 9)) {
    wifi_ip_str(p->first_act_ip, ip);
    snprintf(out, cap, "no data from %s", ip);
  } else if (p->target) {
    wifi_ip_str(p->target, ip);
    snprintf(out, cap, "connecting to %s", ip);
  } else {
    snprintf(out, cap, "listening");
  }
}

/* the module shown as "activating": with a module IP set, only its own POST (one still in flight to a module
 * just left behind is not what the user waits for: the state is "connecting to <ip>"), else the first queued
 * or in flight; 0 = none. As sticktime-pi's input_wifi.c. */
static uint32_t activating_ip(engine_t *e) {
  uint32_t ip = 0;
  pthread_mutex_lock(&e->qmu);
  if (e->p.target) {
    if (wifi_actq_is_pending(&e->q, e->p.target)) ip = e->p.target;
  } else if (e->q.npending) {
    ip = e->q.pending[0];
  }
  pthread_mutex_unlock(&e->qmu);
  return ip;
}

/* the receive thread's view -> the snapshot (only when something changed) */
static void publish(engine_t *e) {
  eng_snapshot_t *w = &e->work;
  const wifi_proto_t *p = &e->p;
  uint32_t pend = activating_ip(e);
  w->running = e->running;
  w->listening = e->sock >= 0;
  w->state = p->state;
  snprintf(w->detail, sizeof w->detail, "%s", p->detail);
  status_text(e, pend, w->status, sizeof w->status);
  w->bound = p->bound;
  if (p->bound) {
    wifi_ip_str(p->bound_ip, w->source);
    w->kind = wifi_proto_kind(p, p->bound_ip);
    snprintf(w->module_name, sizeof w->module_name, "%s", wifi_proto_module_name(p, p->bound_ip));
    w->rate_hz = p->last_hz;
    w->jitter_ms = p->last_jitter;
    w->frames_last_s = p->last_frames;
    w->nch = p->nch;
    memset(w->raw, 0, sizeof w->raw);
    memcpy(w->raw, p->raw, sizeof w->raw[0] * p->nch);
  } else {
    /* not streaming: no module, no sticks (like the Windows app's view) */
    memset(w->source, 0, sizeof w->source);
    w->kind = WIFI_KIND_UNKNOWN;
    memset(w->module_name, 0, sizeof w->module_name);
    w->rate_hz = w->jitter_ms = 0;
    w->frames_last_s = 0;
    w->nch = 0;
    memset(w->raw, 0, sizeof w->raw);
  }
  w->frames = p->seq;
  w->rx = p->rx;
  w->beacons = p->beacons;
  w->malformed = p->malformed;
  w->dropped_filter = p->dropped_filter;
  w->dropped_other = p->dropped_other;
  w->posts = e->posts;
  w->posts_ok = e->posts_ok;
  w->posts_failed = e->posts_failed;
  w->restarts = p->restarts;
  w->gaps = e->gaps;
  w->gap_max_us = e->gap_max_us;
  w->joy_ok = e->joy_ok;
  if (e->joy_ok) snprintf(w->joy_device, sizeof w->joy_device, "%s", e->joy.device);
  snprintf(w->joy_error, sizeof w->joy_error, "%s", e->joy_error);
  pthread_mutex_lock(&e->smu);
  /* the fields this thread does not own, as they are */
  w->seq = e->snap.seq;
  w->log_seq = e->snap.log_seq;
  w->uptime_us = e->snap.uptime_us;
  memcpy(&w->cfg, &e->snap.cfg, sizeof w->cfg);
  bool changed = e->cmd_done != e->cmd_taken;   /* eng_wait_applied waits for this */
  e->cmd_done = e->cmd_taken;
  if (memcmp(w, &e->snap, sizeof *w)) {
    memcpy(&e->snap, w, sizeof *w);
    e->snap.seq++;
    changed = true;
  }
  if (changed) pthread_cond_broadcast(&e->scv);
  pthread_mutex_unlock(&e->smu);
}

/* ================================================================ the receive thread */
static void lock_changes(engine_t *e) {
  if (e->p.bound == e->was_bound) return;
  e->was_bound = e->p.bound;
  pthread_mutex_lock(&e->qmu);
  e->want_signal = e->p.bound;
  if (e->p.bound) pthread_cond_signal(&e->qcv);
  pthread_mutex_unlock(&e->qmu);
  if (!e->p.bound) {
    /* lock lost: the sticks go to the center (vJoy's ResetVJD in the Windows app); a later lock is no gap */
    joy_center(e);
    e->gap_prev = 0;
  }
}

static void *rx_main(void *arg) {
  engine_t *e = arg;
  for (;;) {
    int64_t now = wifi_net_now_us();
    int timeout = TICK_MS;
    if (e->running && e->sock < 0) {
      int64_t d = e->bind_at - now;
      int ms = d <= 0 ? 0 : (int)((d + 999) / 1000);
      if (ms < timeout) timeout = ms;
    }
    struct pollfd pf[2] = { { e->wake_rd, POLLIN, 0 }, { e->sock, POLLIN, 0 } };
    int r = poll(pf, e->sock >= 0 ? 2 : 1, timeout);
    now = wifi_net_now_us();
    e->now = now;
    if (r > 0 && pf[0].revents) {
      char b[64];
      while (read(e->wake_rd, b, sizeof b) > 0) {}
    }
    if (e->sock >= 0 && r > 0 && pf[1].revents) drain(e, now);
    collect_results(e, now);
    bool quit = run_commands(e, now);
    if (quit) break;
    if (e->running && e->sock < 0 && now >= e->bind_at) try_bind(e, now);
    if (e->sock >= 0) wifi_proto_tick(&e->p, now);
    if (e->running) wifi_proto_poll_stats(&e->p, now);
    lock_changes(e);
    /* a centering that did not get through is retried (frames retry by themselves) */
    if (e->joy_write_failed && !e->p.bound) joy_center(e);
    publish(e);
  }
  do_stop(e, false);
  publish(e);
  return NULL;
}

/* ================================================================ create / destroy */
static void login_name(char *out, size_t cap) {
  const char *u = getenv("USER");
  if (!u || !*u) {
    struct passwd *pw = getpwuid(getuid());
    u = pw ? pw->pw_name : NULL;
  }
  size_t k = 0;
  /* it goes into a shell command line: only what a user name normally is */
  for (const char *s = u ? u : ""; *s && k + 1 < cap; s++) {
    char c = *s;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
      k = 0;
      break;
    }
    out[k++] = c;
  }
  out[k] = 0;
}

static int start_thread(pthread_t *t, void *(*fn)(void *), void *arg) {
  /* the program's signals (Ctrl+C, SIGTERM, ...) belong to the main thread */
  sigset_t all, old;
  sigfillset(&all);
  sigdelset(&all, SIGSEGV);
  sigdelset(&all, SIGBUS);
  sigdelset(&all, SIGFPE);
  sigdelset(&all, SIGILL);
  sigdelset(&all, SIGABRT);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  int r = pthread_create(t, NULL, fn, arg);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  return r;
}

static void free_engine(engine_t *e) {
  if (e->wake_rd >= 0) close(e->wake_rd);
  if (e->wake_wr >= 0) close(e->wake_wr);
  pthread_mutex_destroy(&e->mu);
  pthread_mutex_destroy(&e->set_mu);
  pthread_mutex_destroy(&e->qmu);
  pthread_mutex_destroy(&e->smu);
  pthread_cond_destroy(&e->qcv);
  pthread_cond_destroy(&e->scv);
  free(e);
}

static void stop_worker(engine_t *e) {
  if (!e->wth_ok) return;
  pthread_mutex_lock(&e->qmu);
  e->wquit = true;
  wifi_actq_new_session(&e->q);
  pthread_cond_broadcast(&e->qcv);
  pthread_mutex_unlock(&e->qmu);
  pthread_join(e->wth, NULL);           /* a POST in flight notices within 100 ms */
  e->wth_ok = false;
}

engine_t *eng_create(const eng_config_t *cfg, const eng_options_t *opt, char *err, size_t cap) {
  if (cap) err[0] = 0;
  eng_config_t c;
  if (cfg) c = *cfg;
  else eng_config_default(&c);
  if (eng_config_check(&c, err, cap)) return NULL;
  config_normalize(&c);
  engine_t *e = calloc(1, sizeof *e);
  if (!e) { snprintf(err, cap, "out of memory"); return NULL; }
  if (opt) e->opt = *opt;
  else e->opt.joystick = e->opt.buttons = true;
  e->opt.device_name[sizeof e->opt.device_name - 1] = 0;
  if (!e->opt.device_name[0]) snprintf(e->opt.device_name, sizeof e->opt.device_name, "%s", ENG_DEVICE_NAME);
  e->t0 = wifi_net_now_us();
  e->sock = -1;
  e->joy.fd = -1;                     /* no device (calloc's 0 would be stdin) */
  e->wake_rd = e->wake_wr = -1;
  pthread_mutex_init(&e->mu, NULL);
  pthread_mutex_init(&e->set_mu, NULL);
  pthread_mutex_init(&e->qmu, NULL);
  pthread_mutex_init(&e->smu, NULL);
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
  pthread_cond_init(&e->qcv, &ca);
  pthread_cond_init(&e->scv, &ca);
  pthread_condattr_destroy(&ca);
  int pfd[2];
  if (pipe2(pfd, O_NONBLOCK | O_CLOEXEC) != 0) {
    snprintf(err, cap, "cannot create a pipe: %s", strerror(errno));
    free_engine(e);
    return NULL;
  }
  e->wake_rd = pfd[0];
  e->wake_wr = pfd[1];
  e->cfg = c;
  e->cfg_req = c;
  e->run_req = -1;
  memcpy(&e->snap.cfg, &c, sizeof c);
  wifi_actq_init(&e->q);
  wifi_net_init();
  wifi_hooks_t h = { e, h_log, h_state, h_frame, NULL, h_activate };
  wifi_proto_init(&e->p, &h);
  wifi_proto_set_request(&e->p, c.interval_us, c.channels, 0);
  uint32_t ip = 0;
  if (wifi_parse_ip(c.module_ip, &ip) == 1) e->p.target = ip;

  /* the joystick: not fatal when it can't be made (monitor mode) */
  e->work.joy_enabled = e->opt.joystick;
  e->work.joy_buttons = e->opt.buttons;
  snprintf(e->work.joy_name, sizeof e->work.joy_name, "%s", e->opt.device_name);
  if (e->opt.joystick) {
    char user[64], jerr[256];
    login_name(user, sizeof user);
    if (ujoy_open(&e->joy, NULL, e->opt.device_name, e->opt.buttons, user, jerr, sizeof jerr) == 0) {
      e->joy_ok = true;
      joymap_init(&e->jm, e->opt.buttons);
      eng_log(e, "Virtual joystick \"%s\" ready%s%s (8 axes%s)", e->opt.device_name, e->joy.device[0] ? ": " : "",
              e->joy.device, e->opt.buttons ? ", 8 buttons" : "");
    } else {
      snprintf(e->joy_error, sizeof e->joy_error, "%s", jerr);
      eng_log(e, "!! No virtual joystick: %s", jerr);
      eng_log(e, "Running as a monitor: the sticks show here and on the web page, but no game sees them.");
    }
  } else {
    eng_log(e, "No virtual joystick (--no-joystick, or joystick = off in the settings file): monitor only");
  }

  if (start_thread(&e->wth, worker_main, e) != 0) {
    snprintf(err, cap, "cannot start the activation thread");
    ujoy_close(&e->joy);
    free_engine(e);
    return NULL;
  }
  e->wth_ok = true;
  /* the first bind here, before the receive thread exists: the caller sees "listening" at once */
  do_start(e, wifi_net_now_us());
  lock_changes(e);
  publish(e);
  if (start_thread(&e->rth, rx_main, e) != 0) {
    snprintf(err, cap, "cannot start the receive thread");
    do_stop(e, true);
    stop_worker(e);
    ujoy_close(&e->joy);
    free_engine(e);
    return NULL;
  }
  e->rth_ok = true;
  return e;
}

void eng_destroy(engine_t *e) {
  if (!e) return;
  pthread_mutex_lock(&e->mu);
  e->quit = true;
  pthread_mutex_unlock(&e->mu);
  wake(e);
  if (e->rth_ok) pthread_join(e->rth, NULL);    /* it stops: socket closed, joystick centered */
  stop_worker(e);
  ujoy_close(&e->joy);
  free_engine(e);
}

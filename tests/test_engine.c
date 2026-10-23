/* The engine over real loopback sockets: an in-process fake module (ELRS or Crossfire: beacons, the
 * POST /udpcontrol web server, a streaming thread; on 127.0.0.2 / 127.0.0.3 like the real ones would
 * be separate IPs) and the virtual joystick replaced by a sink (ujoy_sys: the writes the device would
 * get, checked and decoded). Covers discovery, the lock, frames to the joystick, the request and its
 * restarts, Crossfire, the module IP, stop/start, port changes, bind errors, every status text,
 * hints, gaps, the log ring, eng_wait, shutdown with a POST in flight, and the release (R14: joystick_end
 * on stop, port change and exit, also to a module left by a switch; the exit's wait for it, also line by line; a
 * release nobody answers holds the next start 2 s at most). */
#include "tests/t.h"
#include "engine.h"
#include "uinput.h"
#include "wifi_net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int64_t now_us(void) { return wifi_net_now_us(); }
static void sleep_ms(int ms) {
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}
static uint32_t IP(const char *s) { uint32_t ip = 0; wifi_parse_ip(s, &ip); return ip; }

static uint16_t free_port(int type) {
  int s = socket(AF_INET, type, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  socklen_t l = sizeof a;
  if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) != 0 || getsockname(s, (struct sockaddr *)&a, &l) != 0) {
    perror("free_port");
    exit(2);
  }
  close(s);
  return ntohs(a.sin_port);
}
/* a port free for both UDP and TCP (the engine's UDP port; the fakes' TCP ports) */
static uint16_t free_udp(void) { return free_port(SOCK_DGRAM); }
static uint16_t free_tcp(void) { return free_port(SOCK_STREAM); }

/* ================================================================ the fake module */
#define MAXPOSTS 64
typedef struct {
  char ip[16];
  bool xf;
  uint16_t http_port, dest_port;
  int udp, ls;
  pthread_t http_th, stream_th;
  pthread_mutex_t mu;
  bool quit;
  /* protocol state */
  bool active;
  uint32_t remote;
  int interval_us, channels;
  /* knobs */
  uint16_t values[16];
  int mode;                       /* 0 normal, 1 placeholder, 2 silent */
  int http_delay_ms;
  bool act_first;                 /* apply a POST, then wait http_delay_ms to answer (ELRS: it stops/starts, then answers) */
  int http_status;
  int end_status;                 /* != 0: the answer to joystick_end (not 200: not applied either) */
  /* observations */
  char posts[MAXPOSTS][128];
  uint32_t post_from[MAXPOSTS];
  int nposts;
  int frames_sent;
} fake_t;

static void fk_lock(fake_t *f) { pthread_mutex_lock(&f->mu); }
static void fk_unlock(fake_t *f) { pthread_mutex_unlock(&f->mu); }

static int param(const char *body, const char *key, char *out, size_t cap) {
  size_t kl = strlen(key);
  for (const char *p = body; p && *p;) {
    const char *amp = strchr(p, '&');
    size_t len = amp ? (size_t)(amp - p) : strlen(p);
    if (len > kl && p[kl] == '=' && !strncmp(p, key, kl)) {
      size_t vl = len - kl - 1;
      if (vl >= cap) vl = cap - 1;
      memcpy(out, p + kl + 1, vl);
      out[vl] = 0;
      return 1;
    }
    p = amp ? amp + 1 : NULL;
  }
  out[0] = 0;
  return 0;
}

/* what the module does with a POST it took */
static void fk_apply(fake_t *f, const char *body, uint32_t from) {
  char action[32], v[32];
  param(body, "action", action, sizeof action);
  fk_lock(f);
  if (!strcmp(action, "joystick_begin")) {
    f->remote = from;
    if (f->xf) {
      f->active = true;
    } else if (!f->active) {           /* ELRS: the request only applies while idle */
      param(body, "interval", v, sizeof v);
      int iv = atoi(v);
      f->interval_us = iv < 1000 ? 10000 : iv;
      param(body, "channels", v, sizeof v);
      int ch = atoi(v) & 0xFF;
      f->channels = ch == 0 ? 8 : ch > 16 ? 16 : ch;
      f->active = true;
    }
  } else if (!strcmp(action, "joystick_end")) {
    if (!f->xf) f->active = false;
  }
  fk_unlock(f);
}

static void *fk_http(void *arg) {
  fake_t *f = arg;
  for (;;) {
    fk_lock(f);
    bool q = f->quit;
    fk_unlock(f);
    if (q) break;
    struct pollfd p = { f->ls, POLLIN, 0 };
    if (poll(&p, 1, 20) <= 0) continue;
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    int c = accept(f->ls, (struct sockaddr *)&peer, &pl);
    if (c < 0) continue;
    char req[2048];
    size_t got = 0;
    for (;;) {
      struct pollfd q2 = { c, POLLIN, 0 };
      if (poll(&q2, 1, 2000) <= 0) break;
      ssize_t k = recv(c, req + got, sizeof req - 1 - got, 0);
      if (k <= 0) break;
      got += (size_t)k;
      req[got] = 0;
      char *he = strstr(req, "\r\n\r\n");
      if (he) {
        long cl = wifi_http_content_length(req, (size_t)(he + 4 - req));
        if (cl < 0 || got >= (size_t)(he + 4 - req) + (size_t)cl) break;
      }
      if (got >= sizeof req - 1) break;
    }
    req[got] = 0;
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : req + got;
    fk_lock(f);
    int delay = f->http_delay_ms, status = f->http_status;
    if (f->end_status && strstr(body, "action=joystick_end")) status = f->end_status;
    bool first = f->act_first;
    if (f->nposts < MAXPOSTS) {
      snprintf(f->posts[f->nposts], sizeof f->posts[0], "%s", body);
      f->post_from[f->nposts] = peer.sin_addr.s_addr;
    }
    f->nposts++;
    fk_unlock(f);
    if (first && status == 200) fk_apply(f, body, peer.sin_addr.s_addr);
    bool gone = false;
    for (int t = 0; t < delay && !gone; t += 10) {
      fk_lock(f);
      bool q = f->quit;
      fk_unlock(f);
      if (q) break;
      /* the client gave up (aborted): next connection */
      struct pollfd cp = { c, POLLIN, 0 };
      char tmp[16];
      if (poll(&cp, 1, 10) > 0 && recv(c, tmp, sizeof tmp, MSG_DONTWAIT) <= 0) gone = true;
    }
    if (gone) { close(c); continue; }
    if (!first && status == 200) fk_apply(f, body, peer.sin_addr.s_addr);
    char resp[256];
    int n = snprintf(resp, sizeof resp, "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n"
                     "Connection: close\r\n\r\nok", status, status == 200 ? "OK" : "Error");
    send(c, resp, (size_t)n, MSG_NOSIGNAL);
    close(c);
  }
  return NULL;
}

static void fk_send(fake_t *f, const void *d, size_t n) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  pthread_mutex_lock(&f->mu);                     /* a test may move it (a port change) */
  a.sin_port = htons(f->dest_port);
  pthread_mutex_unlock(&f->mu);
  a.sin_addr.s_addr = htonl(0x7f000001);
  ssize_t r = sendto(f->udp, d, n, 0, (struct sockaddr *)&a, sizeof a);
  if (r < 0 && getenv("TEST_VERBOSE")) fprintf(stderr, "fake %s sendto port %u: %s\n", f->ip, f->dest_port, strerror(errno));
}

static void *fk_stream(void *arg) {
  fake_t *f = arg;
  int64_t next = 0;
  for (;;) {
    fk_lock(f);
    bool q = f->quit, act = f->active && f->remote && f->mode != 2;
    int period = f->xf ? 11000 : f->interval_us, count = f->xf ? 16 : f->channels, mode = f->mode;
    uint16_t v[16];
    memcpy(v, f->values, sizeof v);
    fk_unlock(f);
    if (q) break;
    int64_t t = now_us();
    if (!act) { next = 0; sleep_ms(2); continue; }
    if (!next || t - next > 100000) next = t;
    if (t < next) {
      int64_t d = next - t;
      struct timespec ts = { 0, (long)(d > 20000 ? 20000 : d) * 1000L };
      nanosleep(&ts, NULL);
      continue;
    }
    uint8_t b[64];
    b[0] = 1;
    b[1] = (uint8_t)count;
    for (int i = 0; i < count; i++) {
      uint16_t x = mode == 1 ? 0xF26A : v[i];
      b[2 + 2 * i] = (uint8_t)(x & 0xFF);
      b[3 + 2 * i] = (uint8_t)(x >> 8);
    }
    fk_send(f, b, 2 + 2 * (size_t)count);
    fk_lock(f);
    f->frames_sent++;
    fk_unlock(f);
    next += period;
  }
  return NULL;
}

static void fk_start(fake_t *f, const char *ip, bool xf, uint16_t http_port, uint16_t dest_port) {
  memset(f, 0, sizeof *f);
  snprintf(f->ip, sizeof f->ip, "%s", ip);
  f->xf = xf;
  f->http_port = http_port;
  f->dest_port = dest_port;
  f->interval_us = 10000;
  f->channels = xf ? 16 : 8;
  f->http_status = 200;
  for (int i = 0; i < 16; i++) f->values[i] = 16393;
  pthread_mutex_init(&f->mu, NULL);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = IP(ip);
  /* its own (random) UDP port must not be the engine's: 127.0.0.2:U would keep the engine off *:U */
  for (int tries = 0;; tries++) {
    f->udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in got;
    socklen_t gl = sizeof got;
    if (f->udp < 0 || bind(f->udp, (struct sockaddr *)&a, sizeof a) != 0 ||
        getsockname(f->udp, (struct sockaddr *)&got, &gl) != 0) { perror("fake udp"); exit(2); }
    if (ntohs(got.sin_port) != dest_port || tries > 20) break;
    close(f->udp);
  }
  f->ls = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(f->ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  a.sin_port = htons(http_port);
  if (f->ls < 0 || bind(f->ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(f->ls, 8) != 0) {
    perror("fake http");
    exit(2);
  }
  pthread_create(&f->http_th, NULL, fk_http, f);
  pthread_create(&f->stream_th, NULL, fk_stream, f);
}

static void fk_stop(fake_t *f) {
  fk_lock(f);
  f->quit = true;
  fk_unlock(f);
  pthread_join(f->http_th, NULL);
  pthread_join(f->stream_th, NULL);
  close(f->udp);
  close(f->ls);
  pthread_mutex_destroy(&f->mu);
}

static void fk_beacon(fake_t *f) {
  if (f->xf) {
    fk_send(f, "VELOCIDRONE", 11);
    return;
  }
  static const uint8_t b[] = { 'E', 'L', 'R', 'S', 1, 0xf8, 0x2a, 10, 'U', 'n', 'i', 'f', 'i', 'e', 'd', ' ', 'T', 'X' };
  fk_send(f, b, sizeof b);
}

static int fk_nposts(fake_t *f) { fk_lock(f); int n = f->nposts; fk_unlock(f); return n; }
static void fk_post(fake_t *f, int i, char *out, size_t cap) {
  fk_lock(f);
  snprintf(out, cap, "%s", i >= 0 && i < f->nposts && i < MAXPOSTS ? f->posts[i] : "");
  fk_unlock(f);
}
static int fk_count(fake_t *f, const char *sub) {
  fk_lock(f);
  int n = 0;
  for (int i = 0; i < f->nposts && i < MAXPOSTS; i++) n += strstr(f->posts[i], sub) != NULL;
  fk_unlock(f);
  return n;
}
static void fk_set(fake_t *f, int idx, uint16_t v) { fk_lock(f); f->values[idx] = v; fk_unlock(f); }
static int fk_interval(fake_t *f) { fk_lock(f); int v = f->interval_us; fk_unlock(f); return v; }
static void fk_mode(fake_t *f, int m) { fk_lock(f); f->mode = m; fk_unlock(f); }

/* ================================================================ the joystick sink (ujoy_sys) */
typedef struct {
  int rd, wr;
  int fail_writes;                /* > 0: the next writes fail with EIO */
  int writes, bad_writes, events;
  int abs_center;                 /* axis events to the center (16384) */
  int32_t axis[8];
  bool btn[8];
  bool created, destroyed;
  int open_errno;
} sink_t;
static sink_t K = { .rd = -1, .wr = -1 };
static pthread_mutex_t kmu = PTHREAD_MUTEX_INITIALIZER;

static int k_open(const char *p, int fl) {
  (void)p;
  (void)fl;
  if (K.open_errno) { errno = K.open_errno; return -1; }
  return K.wr;
}
static int k_ioctl(int fd, unsigned long req, void *arg) {
  (void)fd;
  if (req == UI_GET_SYSNAME(64)) { snprintf(arg, 64, "input%d", 999); return 0; }
  pthread_mutex_lock(&kmu);
  if (req == UI_DEV_CREATE) K.created = true;
  if (req == UI_DEV_DESTROY) K.destroyed = true;
  pthread_mutex_unlock(&kmu);
  return 0;
}
/* each write the engine makes: some events and exactly one SYN_REPORT, at the end */
static ssize_t k_write(int fd, const void *b, size_t n) {
  const ujoy_event_t *ev = b;
  size_t k = n / sizeof *ev;
  bool ok = n % sizeof *ev == 0 && k >= 2 && ev[k - 1].type == EV_SYN && ev[k - 1].code == SYN_REPORT;
  for (size_t i = 0; ok && i + 1 < k; i++) ok = ev[i].type == EV_ABS || ev[i].type == EV_KEY;
  pthread_mutex_lock(&kmu);
  K.writes++;
  if (!ok) K.bad_writes++;
  bool fail = K.fail_writes > 0;
  if (fail) K.fail_writes--;
  pthread_mutex_unlock(&kmu);
  if (fail) { errno = EIO; return -1; }
  return write(fd, b, n);
}
static int k_close(int fd) { return close(fd); }
static ujoy_sys_t KSYS = { k_open, k_ioctl, k_write, k_close, "/nonexistent" };

static void sink_new(void) {
  int p[2];
  if (pipe(p) != 0) { perror("pipe"); exit(2); }
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  fcntl(p[1], F_SETFL, O_NONBLOCK);
  fcntl(p[0], F_SETPIPE_SZ, 1 << 20);
  pthread_mutex_lock(&kmu);
  memset(&K, 0, sizeof K);
  K.rd = p[0];
  K.wr = p[1];
  for (int i = 0; i < 8; i++) K.axis[i] = 16384;
  pthread_mutex_unlock(&kmu);
  ujoy_sys = &KSYS;
}

/* read what the device got so far into K.axis / K.btn */
static void sink_pump(void) {
  if (K.rd < 0) return;
  static const int axes[8] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_RZ, ABS_Z, ABS_THROTTLE, ABS_RUDDER };
  ujoy_event_t ev[256];
  for (;;) {
    ssize_t n = read(K.rd, ev, sizeof ev);
    if (n <= 0) break;
    for (size_t i = 0; i < (size_t)n / sizeof ev[0]; i++) {
      K.events++;
      if (ev[i].type == EV_ABS) {
        for (int a = 0; a < 8; a++) if (axes[a] == ev[i].code) K.axis[a] = ev[i].value;
        if (ev[i].value == 16384) K.abs_center++;
      }
      if (ev[i].type == EV_KEY && ev[i].code >= BTN_TRIGGER && ev[i].code <= BTN_BASE2) K.btn[ev[i].code - BTN_TRIGGER] = ev[i].value;
    }
  }
}
static void sink_close(void) {
  sink_pump();
  close(K.rd);
  K.rd = -1;
  ujoy_sys = NULL;
}
static bool sink_centered(void) {
  sink_pump();
  for (int i = 0; i < 8; i++) if (K.axis[i] != 16384 || K.btn[i]) return false;
  return true;
}

/* ================================================================ engine helpers */
static engine_t *E;
static char logs[4096][ENG_LOG_LEN];
static int nlogs;
static uint32_t log_since;

static void pull_logs(void) {
  eng_logline_t l[64];
  int n;
  while (E && (n = eng_log_read(E, log_since, l, 64)) > 0) {
    for (int i = 0; i < n; i++) {
      if (getenv("TEST_VERBOSE")) fprintf(stderr, "    | %lld %s\n", (long long)(l[i].wall_ms % 100000), l[i].text);
      if (nlogs < 4096) snprintf(logs[nlogs], sizeof logs[0], "%s", l[i].text);
      nlogs++;
      log_since = l[i].seq;
    }
  }
}
static int logged(const char *sub) {
  pull_logs();
  int c = 0;
  for (int i = 0; i < nlogs && i < 4096; i++) c += strstr(logs[i], sub) != NULL;
  return c;
}
static eng_snapshot_t S;
static void snap(void) { eng_snapshot(E, &S); pull_logs(); sink_pump(); }

#define WAIT(ms, cond) ({ bool ok_ = false; int64_t end_ = now_us() + (int64_t)(ms) * 1000; \
  for (;;) { snap(); if (cond) { ok_ = true; break; } if (now_us() > end_) break; \
             eng_wait(E, S.seq, S.log_seq, 20); } ok_; })

static int saves;
static eng_config_t saved;
static int save_fail;
static int on_save(void *ud, const eng_config_t *c, char *err, size_t cap) {
  (void)ud;
  saves++;
  saved = *c;
  if (save_fail) { snprintf(err, cap, "disk full"); return -1; }
  return 0;
}

static engine_t *start(uint16_t port, uint16_t http, const char *module_ip, bool joystick) {
  eng_config_t c;
  eng_config_default(&c);
  c.port = port;
  c.http_port = http;
  snprintf(c.module_ip, sizeof c.module_ip, "%s", module_ip ? module_ip : "");
  eng_options_t o;
  memset(&o, 0, sizeof o);
  o.joystick = joystick;
  o.buttons = true;
  o.save = on_save;
  if (joystick) sink_new();
  nlogs = 0;
  log_since = 0;
  saves = 0;
  save_fail = 0;
  char err[256];
  E = eng_create(&c, &o, err, sizeof err);
  if (!E) { fprintf(stderr, "eng_create: %s\n", err); exit(2); }
  return E;
}
static void finish(void) {
  eng_destroy(E);
  E = NULL;
  if (ujoy_sys) sink_close();
}
static int set_cfg(void (*edit)(eng_config_t *), char *err, size_t cap) {
  eng_snapshot(E, &S);
  eng_config_t c = S.cfg;
  edit(&c);
  return eng_set_config(E, &c, err, cap);
}

static bool udp_port_free(uint16_t port) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  bool ok = bind(s, (struct sockaddr *)&a, sizeof a) == 0;
  close(s);
  return ok;
}

/* ================================================================ tests */
static void t_config_check(void) {
  eng_config_t c;
  char err[256];
  eng_config_default(&c);
  CHECK_STR(c.module_ip, "");
  CHECK_INT(c.port, 11000);
  CHECK_INT(c.http_port, 80);
  CHECK_INT(c.interval_us, 4000);
  CHECK_INT(c.channels, 16);
  CHECK_INT(eng_config_check(&c, err, sizeof err), 0);
  CHECK_STR(err, "");
  c.channels = 3;
  CHECK_INT(eng_config_check(&c, err, sizeof err), -1);
  CHECK_STR(err, "channels: 3 is not supported - use 4 to 16");
  c.channels = 17;
  CHECK_INT(eng_config_check(&c, err, sizeof err), -1);
  eng_config_default(&c);
  c.interval_us = 999;
  CHECK_INT(eng_config_check(&c, err, sizeof err), -1);
  CHECK_STR(err, "interval_us: 999 is not supported - use 1000 to 100000");
  c.interval_us = 100001;
  CHECK_INT(eng_config_check(&c, err, sizeof err), -1);
  eng_config_default(&c);
  c.port = 0;
  CHECK_INT(eng_config_check(&c, err, sizeof err), -1);
  CHECK_STR(err, "port: 0 is not supported - use 1 to 65535");
  eng_config_default(&c);
  c.http_port = 0;
  CHECK_INT(eng_config_check(&c, err, sizeof err), -1);
  CHECK_STR(err, "http_port: 0 is not supported - use 1 to 65535");
  eng_config_default(&c);
  snprintf(c.module_ip, sizeof c.module_ip, "10.0.0");
  CHECK_INT(eng_config_check(&c, err, sizeof err), -1);
  CHECK_STR(err, "module_ip: \"10.0.0\" is not an IPv4 address - use a.b.c.d, or leave it empty to find the module by its broadcast");
  snprintf(c.module_ip, sizeof c.module_ip, "0.0.0.0");
  CHECK_INT(eng_config_check(&c, err, sizeof err), -1);
  snprintf(c.module_ip, sizeof c.module_ip, "a\nb");
  CHECK_INT(eng_config_check(&c, err, sizeof err), -1);
  CHECK(strchr(err, '\n') == NULL);
  memset(c.module_ip, 'x', sizeof c.module_ip);        /* not terminated */
  CHECK_INT(eng_config_check(&c, err, sizeof err), -1);
  snprintf(c.module_ip, sizeof c.module_ip, " 10.0.0.1 ");
  CHECK_INT(eng_config_check(&c, err, sizeof err), 0);
  /* an invalid config: no engine */
  eng_config_default(&c);
  c.channels = 2;
  CHECK(eng_create(&c, NULL, err, sizeof err) == NULL);
  CHECK_STR(err, "channels: 2 is not supported - use 4 to 16");
}

static uint16_t g_port;
static void set_port(eng_config_t *c) { c->port = g_port; }
static void inc_rate(eng_config_t *c) { c->interval_us = 2000; }
static void set_8ch(eng_config_t *c) { c->channels = 8; }
static void set_16ch(eng_config_t *c) { c->channels = 16; }
static int g_iv;
static void set_iv(eng_config_t *c) { c->interval_us = g_iv; }
static char g_ip[64];
static void set_ip(eng_config_t *c) { snprintf(c->module_ip, sizeof c->module_ip, "%s", g_ip); }
static int g_http;
static void set_http(eng_config_t *c) { c->http_port = (uint16_t)g_http; }
static void *logger(void *arg) {
  sleep_ms(100);
  eng_log(arg, "from another thread");
  return NULL;
}

static void t_flow(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  for (int i = 0; i < 16; i++) m.values[i] = (uint16_t)(1000 + i * 100);
  m.values[8] = 32767;                                  /* button 1 */
  m.values[9] = 22000;                                  /* in the band: stays released */
  m.values[15] = 24576;                                 /* button 8, at the threshold */
  start(U, H, "", true);
  /* listening at once, the joystick made */
  eng_snapshot(E, &S);
  CHECK(S.running && S.listening);
  CHECK_STR(S.status, "listening");
  CHECK_INT(S.state, WIFI_ST_SEARCHING);
  CHECK(S.joy_enabled && S.joy_ok && S.joy_buttons);
  CHECK_STR(S.joy_name, ENG_DEVICE_NAME);
  CHECK_STR(S.joy_error, "");
  CHECK(K.created);
  CHECK_INT(S.cfg.port, U);
  CHECK_INT(S.nch, 0);
  CHECK(!S.bound);
  char want[64];
  snprintf(want, sizeof want, "Listening for joystick data on UDP %u", U);
  CHECK_INT(logged(want), 1);
  CHECK_INT(logged("Virtual joystick \"ELRS Crossfire WiFi Joystick\" ready (8 axes, 8 buttons)"), 1);
  CHECK(WAIT(2000, logged("This machine") > 0));       /* the worker lists the addresses */
  /* beacon -> POST with the default request -> stream -> lock */
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound));
  CHECK_INT(logged("Discovery beacon from ELRS module 127.0.0.2 - activating"), 1);
  CHECK(WAIT(2000, logged("Activation POST -> 127.0.0.2: 200 OK") == 1));
  CHECK_INT(logged("Joystick source locked to 127.0.0.2"), 1);
  char body[128];
  fk_post(&m, 0, body, sizeof body);
  CHECK_STR(body, "action=joystick_begin&interval=4000&channels=16");
  CHECK_INT(fk_nposts(&m), 1);
  CHECK(m.post_from[0] == IP("127.0.0.1"));
  CHECK(WAIT(2000, S.nch == 16 && S.raw[0] == 1000));
  CHECK_STR(S.status, "streaming");
  CHECK_INT(S.state, WIFI_ST_STREAMING);
  CHECK_STR(S.detail, "127.0.0.2");
  CHECK_STR(S.source, "127.0.0.2");
  CHECK_INT(S.kind, WIFI_KIND_ELRS);
  CHECK_STR(S.module_name, "Unified TX");
  for (int i = 0; i < 16; i++) CHECK_INT(S.raw[i], m.values[i]);
  CHECK(WAIT(2000, S.posts_ok == 1));                   /* (the result's log line comes before its snapshot) */
  CHECK_INT(S.posts, 1);
  CHECK_INT(S.posts_failed, 0);
  CHECK_INT(S.restarts, 0);
  CHECK(S.beacons >= 1);
  CHECK(S.frames >= 1);
  /* the joystick follows: axes = CH1..8 raw, buttons from CH9..16 */
  CHECK(WAIT(1000, K.axis[0] == 1000));
  for (int i = 0; i < 8; i++) CHECK_INT(K.axis[i], 1000 + i * 100);
  CHECK(K.btn[0]);
  CHECK(!K.btn[1]);
  for (int i = 2; i < 7; i++) CHECK(!K.btn[i]);
  CHECK(K.btn[7]);
  /* a stick moves: within a frame or two */
  fk_set(&m, 3, 30000);
  CHECK(WAIT(1000, K.axis[3] == 30000));
  CHECK(WAIT(1000, S.raw[3] == 30000));
  fk_set(&m, 8, 20000);                                 /* below 62.5 %: released */
  CHECK(WAIT(1000, !K.btn[0]));
  /* the rate after a second or two of 250 Hz */
  CHECK(WAIT(3000, S.frames_last_s > 0 && S.rate_hz > 0));
  fprintf(stderr, "  250 Hz asked: %.1f Hz measured, jitter %.2f ms, %d frames in the last second\n", S.rate_hz,
          S.jitter_ms, S.frames_last_s);
  CHECK(S.rate_hz > 150 && S.rate_hz < 350);
  CHECK_INT(S.gaps, 0);
  /* every write the device got: events + one SYN_REPORT */
  pthread_mutex_lock(&kmu);
  CHECK(K.writes >= 3);                                 /* only changes are written: 3 so far */
  CHECK_INT(K.bad_writes, 0);
  pthread_mutex_unlock(&kmu);
  /* the snapshot's seq moves while streaming; eng_wait sees it */
  eng_snapshot(E, &S);
  CHECK(eng_wait(E, S.seq, S.log_seq, 1000));

  /* ---- the request: a change restarts the ELRS stream so it applies ---- */
  char err[256];
  int posts0 = fk_nposts(&m);
  CHECK_INT(set_cfg(inc_rate, err, sizeof err), 0);
  CHECK_STR(err, "");
  CHECK_INT(saves, 1);
  CHECK_INT(saved.interval_us, 2000);
  eng_snapshot(E, &S);
  CHECK_INT(S.cfg.interval_us, 2000);                   /* at once */
  CHECK(WAIT(3000, fk_nposts(&m) >= posts0 + 2 && logged("Activation POST -> 127.0.0.2") == 2));
  fk_post(&m, posts0, body, sizeof body);
  CHECK_STR(body, "action=joystick_end");
  fk_post(&m, posts0 + 1, body, sizeof body);
  CHECK_STR(body, "action=joystick_begin&interval=2000&channels=16");
  CHECK_INT(logged("Asking for 500 Hz (2000 us) and 16 channels"), 1);
  CHECK_INT(logged("Restarting the stream of 127.0.0.2: 500 Hz, 16 channels"), 1);
  CHECK_INT(logged("Stream end POST -> 127.0.0.2: 200"), 1);
  fk_lock(&m);
  CHECK_INT(m.interval_us, 2000);
  fk_unlock(&m);
  CHECK(WAIT(2000, S.restarts == 1));
  CHECK(S.bound);                                       /* the lock held through it */
  CHECK(WAIT(4000, S.rate_hz > 350));
  fprintf(stderr, "  500 Hz asked: %.1f Hz measured\n", S.rate_hz);
  /* channels: 8 -> the stream carries 8; CH9..16 are no longer driven: their buttons are released */
  CHECK_INT(set_cfg(set_8ch, err, sizeof err), 0);
  CHECK(WAIT(3000, S.nch == 8));
  CHECK_INT(fk_count(&m, "action=joystick_end"), 2);
  CHECK_INT(fk_count(&m, "action=joystick_begin&interval=2000&channels=8"), 1);
  for (int i = 8; i < 16; i++) CHECK_INT(S.raw[i], 0);
  CHECK(WAIT(1000, !K.btn[7]));                         /* button 8 was pressed: released now */
  CHECK_INT(S.restarts, 2);
  /* an explicit restart re-sends the request */
  eng_restart_stream(E);
  CHECK(WAIT(3000, fk_count(&m, "action=joystick_end") == 3 && S.restarts == 3));
  /* the same config again: saved again, nothing restarted */
  int ends = fk_count(&m, "action=joystick_end");
  eng_snapshot(E, &S);
  eng_config_t same = S.cfg;
  CHECK_INT(eng_set_config(E, &same, err, sizeof err), 0);
  sleep_ms(300);
  CHECK_INT(fk_count(&m, "action=joystick_end"), ends);
  /* an invalid config: -1, nothing changes, nothing saved */
  int saves0 = saves;
  same.channels = 99;
  CHECK_INT(eng_set_config(E, &same, err, sizeof err), -1);
  CHECK_STR(err, "channels: 99 is not supported - use 4 to 16");
  CHECK_INT(saves, saves0);
  eng_snapshot(E, &S);
  CHECK_INT(S.cfg.channels, 8);
  /* a save that fails: applied, logged, reported */
  save_fail = 1;
  CHECK_INT(set_cfg(set_16ch, err, sizeof err), 0);
  CHECK_STR(err, "disk full");
  CHECK(WAIT(1000, logged("Settings not saved: disk full") == 1));
  CHECK(WAIT(3000, S.nch == 16));
  save_fail = 0;

  /* ---- a pause in the stream is counted and logged ---- */
  /* (no restart of ours under way, and the stream flowing again after the last one: a pause that begins
   * while our restart is under way is the restart's) */
  CHECK(WAIT(3000, S.posts_ok + S.posts_failed == S.posts));
  uint32_t f0 = S.frames;
  CHECK(WAIT(3000, S.frames > f0 + 20));
  snap();
  uint32_t g0 = S.gaps;
  int gl0 = logged("The stick data paused for");
  fk_mode(&m, 2);
  sleep_ms(500);
  fk_mode(&m, 0);
  CHECK(WAIT(2000, S.gaps == g0 + 1));
  CHECK(S.gap_max_us >= 400000 && S.gap_max_us < 2500000);
  CHECK(WAIT(1000, logged("The stick data paused for") == gl0 + 1));
  CHECK(logged("(ELRS 127.0.0.2") >= 1);
  g0 = S.gaps;

  /* ---- silence: held, then after 3 s the lock goes and the joystick centers ---- */
  fk_mode(&m, 2);
  sleep_ms(1500);
  snap();
  CHECK(S.bound);
  CHECK(!sink_centered());                              /* short gaps hold the last values */
  CHECK(WAIT(4000, !S.bound));
  CHECK_INT(logged("Source 127.0.0.2 went quiet - releasing lock"), 1);
  CHECK(WAIT(1000, sink_centered()));
  CHECK_INT(S.nch, 0);                                  /* no sticks without a module */
  CHECK_STR(S.source, "");
  CHECK(S.rate_hz == 0);
  CHECK_STR(S.status, "listening");
  /* back: relocks at once (the module never stopped "streaming") */
  fk_mode(&m, 0);
  CHECK(WAIT(2000, S.bound && S.nch == 16));
  CHECK(WAIT(1000, K.axis[3] == 30000));
  /* the "lock lost" gap is no gap */
  CHECK_INT(S.gaps, g0);

  /* ---- stop: closed, centered, the port free, the module released (R14); start: found by its beacon ---- */
  posts0 = fk_nposts(&m);
  snap();
  uint32_t ap0 = S.posts;
  eng_stop(E);
  CHECK(WAIT(1000, !S.running));
  CHECK(!S.listening);
  CHECK_STR(S.status, "stopped");
  CHECK_INT(S.state, WIFI_ST_STOPPED);
  CHECK(sink_centered());
  CHECK(udp_port_free(U));
  CHECK_INT(logged("Stopped."), 1);
  CHECK_INT(logged("keeps broadcasting"), 0);
  CHECK(WAIT(2000, logged("Released 127.0.0.2: it broadcasts again, any app can connect to it") == 1));
  CHECK_INT(fk_nposts(&m), posts0 + 1);
  fk_post(&m, posts0, body, sizeof body);
  CHECK_STR(body, "action=joystick_end");               /* alone */
  fk_lock(&m);
  CHECK(!m.active);                                     /* it streams no more */
  fk_unlock(&m);
  CHECK(WAIT(1000, S.releasing == 0));
  CHECK_INT(S.posts, ap0);                              /* not an activation */
  eng_stop(E);                                          /* twice: nothing */
  sleep_ms(100);
  CHECK_INT(logged("Stopped."), 1);
  CHECK_INT(logged("Releasing 127.0.0.2"), 1);
  eng_restart_stream(E);                                /* stopped: nothing to restart (the Windows app's line) */
  CHECK(WAIT(1000, logged("Not running - nothing to restart") == 1));
  CHECK_INT(logged("Not listening"), 0);
  fk_beacon(&m);                                        /* stopped: nobody listens, nothing sent */
  sleep_ms(200);
  CHECK_INT(fk_nposts(&m), posts0 + 1);
  eng_start(E);
  eng_start(E);
  CHECK(WAIT(1000, logged(want) == 2));
  fk_beacon(&m);                                        /* released, it beacons: activated again */
  CHECK(WAIT(2000, S.bound));
  CHECK_INT(fk_nposts(&m), posts0 + 2);
  /* stop + start at once: a new session. The frames that come before the joystick_end lands may lock it again
   * for the 3 s of the timeout (its beacon is not taken then); after that its beacon activates it */
  eng_stop(E);
  eng_start(E);
  CHECK(WAIT(2000, logged("Stopped.") == 2 && logged(want) == 3));
  CHECK(WAIT(2000, logged("Released 127.0.0.2") == 2));
  CHECK(WAIT(4000, !S.bound));
  fk_beacon(&m);
  CHECK(WAIT(2000, S.bound));

  /* ---- the port changes: rebind, the old one free; a stop and a start, so the module is released (R14) ---- */
  uint16_t U2 = free_udp();
  g_port = U2;
  CHECK_INT(set_cfg(set_port, err, sizeof err), 0);
  char want2[64];
  snprintf(want2, sizeof want2, "Listening for joystick data on UDP %u", U2);
  CHECK(WAIT(2000, logged(want2) == 1));
  CHECK(WAIT(1000, S.listening));
  CHECK(udp_port_free(U));
  CHECK(!udp_port_free(U2));
  CHECK_INT(S.cfg.port, U2);
  CHECK(WAIT(2000, logged("Released 127.0.0.2") == 3));
  CHECK_INT(logged("Stopped."), 2);                     /* (quiet: a port change is no Stop for the user) */

  finish();
  CHECK(K.destroyed);
  fk_stop(&m);
}


static void t_crossfire_and_target(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t e, x;
  fk_start(&e, "127.0.0.2", false, H, U);
  fk_start(&x, "127.0.0.3", true, H, U);
  start(U, H, "", false);
  eng_snapshot(E, &S);
  CHECK(!S.joy_enabled && !S.joy_ok);
  CHECK_STR(S.joy_error, "");
  CHECK_INT(logged("No virtual joystick (--no-joystick, or joystick = off in the settings file): monitor only"), 1);
  /* Crossfire first: it locks; ELRS is ignored while the lock is fresh */
  fk_beacon(&x);
  CHECK(WAIT(5000, S.bound));
  CHECK_STR(S.source, "127.0.0.3");
  CHECK_INT(S.kind, WIFI_KIND_XF);
  CHECK(WAIT(2000, S.nch == 16));
  fk_beacon(&e);                                        /* a second module's beacon: no POST while locked */
  sleep_ms(300);
  CHECK_INT(fk_nposts(&e), 0);
  /* the request changes: Crossfire ignores it, never gets joystick_end */
  char err[256];
  CHECK_INT(set_cfg(inc_rate, err, sizeof err), 0);
  CHECK(WAIT(2000, logged("ignores the rate and channel count") == 1));
  CHECK_INT(set_cfg(set_8ch, err, sizeof err), 0);
  sleep_ms(300);
  eng_restart_stream(E);
  CHECK(WAIT(2000, logged("nothing to restart") == 1));
  sleep_ms(300);
  CHECK_INT(fk_count(&x, "joystick_end"), 0);
  CHECK_INT(fk_nposts(&x), 1);
  snap();
  CHECK_INT(S.restarts, 0);
  CHECK_INT(S.nch, 16);                                 /* still 16: Crossfire sends what it sends */
  CHECK(WAIT(3000, S.rate_hz > 0));                     /* a full stats second */
  CHECK(S.rate_hz > 50 && S.rate_hz < 130);             /* ~90 Hz */

  fprintf(stderr, "  Crossfire: %.1f Hz\n", S.rate_hz);

  /* the module IP: the ELRS module, at once */
  snprintf(g_ip, sizeof g_ip, " 127.0.0.2 ");
  CHECK_INT(set_cfg(set_ip, err, sizeof err), 0);
  eng_snapshot(E, &S);
  CHECK_STR(S.cfg.module_ip, "127.0.0.2");              /* in its plain form */
  CHECK(WAIT(2000, logged("Target module set to 127.0.0.2 - connecting...") == 1));
  CHECK(WAIT(3000, S.bound && !strcmp(S.source, "127.0.0.2")));
  /* it asked for what the request is now: 500 Hz, 8 channels */
  CHECK_INT(fk_count(&e, "action=joystick_begin&interval=2000&channels=8"), 1);
  CHECK(WAIT(2000, S.nch == 8));
  /* the Crossfire stream goes on, but only the target counts */
  uint32_t d0 = S.dropped_other;
  CHECK(WAIT(2000, S.dropped_other > d0 + 10));
  CHECK_STR(S.source, "127.0.0.2");
  /* back to discovery */
  g_ip[0] = 0;
  CHECK_INT(set_cfg(set_ip, err, sizeof err), 0);
  CHECK(WAIT(1000, logged("Module IP cleared - finding the module by its broadcast") == 1));
  /* the stop releases (R14) the ELRS module locked now; the Crossfire one still streaming to us (its frames
   * dropped) is left as it is: it broadcasts while streaming, and what joystick_end does to it is unknown */
  eng_stop(E);
  CHECK(WAIT(1000, !S.running));
  CHECK(WAIT(2000, logged("Released 127.0.0.2: it broadcasts again, any app can connect to it") == 1));
  CHECK_INT(logged("Releasing 127.0.0.2: joystick_end, so it stops streaming and broadcasts again"), 1);
  CHECK_INT(logged("Not releasing 127.0.0.3: Crossfire broadcasts while streaming, other apps find it as it is"), 1);
  CHECK_INT(logged("Releasing 127.0.0.3"), 0);
  CHECK_INT(fk_count(&e, "action=joystick_end"), 1);
  CHECK_INT(fk_count(&x, "joystick_end"), 0);
  /* while stopped a module IP waits for the start */
  snprintf(g_ip, sizeof g_ip, "127.0.0.3");
  CHECK_INT(set_cfg(set_ip, err, sizeof err), 0);
  CHECK(WAIT(1000, logged("Module IP set to 127.0.0.3 (used when listening starts)") == 1));
  int xp = fk_nposts(&x);
  sleep_ms(200);
  CHECK_INT(fk_nposts(&x), xp);
  snap();
  CHECK_STR(S.status, "stopped");
  eng_start(E);
  CHECK(WAIT(3000, fk_nposts(&x) == xp + 1));
  CHECK(WAIT(3000, S.bound && !strcmp(S.source, "127.0.0.3")));
  g_ip[0] = 0;
  eng_stop(E);
  CHECK(WAIT(1000, logged("Not releasing 127.0.0.3") == 2));      /* locked Crossfire: not released either */
  CHECK_INT(fk_count(&x, "joystick_end"), 0);
  CHECK_INT(logged("Releasing 127.0.0.2"), 1);                    /* (released before: it streams no more) */
  CHECK_INT(set_cfg(set_ip, err, sizeof err), 0);
  CHECK(WAIT(1000, logged("Module IP cleared - the module will be found by its broadcast") == 1));
  /* the module's web port */
  g_http = 8080;
  CHECK_INT(set_cfg(set_http, err, sizeof err), 0);
  CHECK(WAIT(1000, logged("The module's web server port is 8080 now") == 1));
  finish();
  fk_stop(&e);
  fk_stop(&x);
}

/* a module still streaming another app's request (8 channels) is restarted once */
static void t_mismatch(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  fk_lock(&m);
  m.active = true;
  m.remote = IP("127.0.0.1");
  m.channels = 8;
  m.interval_us = 10000;
  fk_unlock(&m);
  start(U, H, "", true);
  CHECK(WAIT(3000, S.bound));
  CHECK(WAIT(3000, logged("127.0.0.2 streams 8 channels, 16 asked for (another app's request?) - restarting its stream") == 1));
  CHECK(WAIT(3000, S.nch == 16));
  CHECK_INT(fk_count(&m, "action=joystick_end"), 1);
  CHECK_INT(fk_count(&m, "action=joystick_begin&interval=4000&channels=16"), 1);
  CHECK_INT(S.restarts, 1);
  finish();
  fk_stop(&m);
}

static void t_status_texts(void) {
  uint16_t U = free_udp(), H = free_tcp(), Hdead = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  /* activating: a POST that takes a while */
  m.http_delay_ms = 800;
  start(U, H, "127.0.0.2", false);
  CHECK(WAIT(500, !strcmp(S.status, "activating 127.0.0.2")));
  CHECK(WAIT(3000, logged("Activation POST -> 127.0.0.2: 200 OK") == 1));
  CHECK(WAIT(2000, S.bound));
  /* no stick data: placeholder frames */
  fk_mode(&m, 1);
  CHECK(WAIT(5000, !strcmp(S.status, "no stick data from 127.0.0.2")));
  CHECK_INT(logged("127.0.0.2 is streaming placeholder frames - no stick data"), 1);
  CHECK(S.dropped_filter > 0);
  CHECK(!S.bound);
  finish();
  /* connecting: the POST was refused (and the module streams no more) */
  fk_mode(&m, 0);
  fk_lock(&m);
  m.active = false;
  fk_unlock(&m);
  start(U, Hdead, "127.0.0.2", false);
  CHECK(WAIT(3000, logged("Activation failed for 127.0.0.2: connection refused") == 1));
  CHECK(WAIT(1000, !strcmp(S.status, "connecting to 127.0.0.2")));
  CHECK_INT(S.posts_failed, 1);
  finish();
  /* no data: activated, but nothing arrives (the stream goes elsewhere) -> the hint + the firewall line */
  fk_lock(&m);
  m.http_delay_ms = 0;
  fk_unlock(&m);
  fk_mode(&m, 2);
  start(U, H, "", false);
  fk_beacon(&m);
  CHECK(WAIT(3000, logged("Activation POST -> 127.0.0.2: 200 OK") == 1));
  CHECK(WAIT(6000, !strcmp(S.status, "no data from 127.0.0.2")));
  CHECK_INT(logged("!! Activated OK but NO data arriving"), 1);
  char fw[64];
  snprintf(fw, sizeof fw, "UDP port %u", U);
  CHECK(logged(fw) >= 1 || logged("ufw allow") >= 1 || logged("firewall-cmd") >= 1);
  CHECK(logged("sudo ") >= 1);
  finish();
  fk_stop(&m);
}

/* The module IP switched while a POST to the module being left is still in flight (a slow module): the status
 * is about the new module ("activating" its own POST, then "connecting to"), never "activating <the old one>" */
static void t_status_switch_in_flight(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  fk_lock(&m);
  m.http_delay_ms = 1500;
  fk_unlock(&m);
  start(U, H, "127.0.0.2", false);
  CHECK(WAIT(500, !strcmp(S.status, "activating 127.0.0.2")));
  snprintf(g_ip, sizeof g_ip, "127.0.0.3");          /* nothing answers there: refused */
  char err[256];
  CHECK_INT(set_cfg(set_ip, err, sizeof err), 0);
  CHECK(WAIT(500, strstr(S.status, "127.0.0.3") != NULL));
  bool old_shown = false;
  int64_t end = now_us() + 1000000;                  /* the POST to 127.0.0.2 is still in flight */
  while (now_us() < end) {
    snap();
    if (strstr(S.status, "127.0.0.2")) old_shown = true;
    eng_wait(E, S.seq, S.log_seq, 20);
  }
  CHECK(!old_shown);
  CHECK(WAIT(3000, logged("Activation POST -> 127.0.0.2: 200 OK") == 1));
  CHECK(WAIT(3000, logged("Activation failed for 127.0.0.3") >= 1));
  CHECK(WAIT(1000, !strcmp(S.status, "connecting to 127.0.0.3")));
  finish();
  fk_stop(&m);
}

static void t_bind_error(void) {
  uint16_t U = free_udp();
  int blocker = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(U);
  CHECK(bind(blocker, (struct sockaddr *)&a, sizeof a) == 0);
  start(U, 80, "", false);
  eng_snapshot(E, &S);
  CHECK(S.running && !S.listening);
  char st[64];
  snprintf(st, sizeof st, "opening UDP port %u", U);
  CHECK_STR(S.status, st);
  CHECK_INT(S.state, WIFI_ST_SEARCHING);                /* running: never "stopped" (the web banner) */
  CHECK_STR(S.detail, st);
  CHECK_INT(logged("ERROR"), 0);                        /* 5 quick tries first */
  sleep_ms(700);
  snap();
  CHECK(S.running && !S.listening && S.state == WIFI_ST_SEARCHING);
  snprintf(st, sizeof st, "UDP port %u in use", U);
  CHECK(WAIT(3000, !strcmp(S.status, st)));
  CHECK_INT(S.state, WIFI_ST_ERROR);
  char want[96];
  snprintf(want, sizeof want, "ERROR: cannot listen on UDP port %u - it is already in use", U);
  CHECK_INT(logged(want), 1);
  sleep_ms(1000);
  CHECK_INT(logged("ERROR"), 1);                        /* said once */
  /* start while retrying: nothing new */
  eng_start(E);
  sleep_ms(100);
  CHECK_INT(logged("ERROR"), 1);
  /* a restart while running but not listening: nothing could be sent */
  eng_restart_stream(E);
  CHECK(WAIT(1000, logged("Not listening - nothing to restart") == 1));
  CHECK_INT(logged("Not running"), 0);
  close(blocker);
  CHECK(WAIT(7000, S.listening));                       /* the next try, within 5 s */
  CHECK_STR(S.status, "listening");
  /* stop while in error, start again */
  finish();
}

static void t_joystick_fails(void) {
  uint16_t U = free_udp();
  sink_new();
  K.open_errno = EACCES;
  eng_config_t c;
  eng_config_default(&c);
  c.port = U;
  eng_options_t o;
  memset(&o, 0, sizeof o);
  o.joystick = true;
  o.buttons = false;
  snprintf(o.device_name, sizeof o.device_name, "My Sim Stick");
  char err[256];
  nlogs = 0;
  log_since = 0;
  E = eng_create(&c, &o, err, sizeof err);
  CHECK(E != NULL);
  if (!E) return;
  snap();
  CHECK(S.joy_enabled);
  CHECK(!S.joy_ok);
  CHECK(!S.joy_buttons);
  CHECK_STR(S.joy_name, "My Sim Stick");
  CHECK(strstr(S.joy_error, "no permission to use /dev/uinput") != NULL);
  CHECK(strstr(S.joy_error, "run the install script with sudo") != NULL);
  CHECK(strstr(S.joy_error, "usermod -aG input") != NULL);
  CHECK_INT(logged("!! No virtual joystick: no permission"), 1);
  CHECK_INT(logged("Running as a monitor"), 1);
  CHECK(S.listening);                                  /* not fatal */
  eng_destroy(E);
  E = NULL;
  close(K.wr);
  sink_close();
}

static void t_log_ring(void) {
  uint16_t U = free_udp();
  start(U, 80, "", false);
  eng_logline_t l[ENG_LOG_LINES];
  CHECK(WAIT(2000, logged("This machine") >= 1));
  snap();
  uint32_t base = S.log_seq;
  CHECK(base >= 2);
  /* control characters, long lines cut at a character */
  eng_log(E, "tab\there\nnewline\x1b[31m");
  char longl[1000];
  memset(longl, 'a', sizeof longl);
  memcpy(longl + ENG_LOG_LEN - 2, "\xc3\xa9", 2);    /* an "é" across the cut */
  longl[sizeof longl - 1] = 0;
  eng_log(E, "%s", longl);
  /* not UTF-8, C1 controls, overlong forms, surrogates: '?' each; good characters stay */
  eng_log(E, "a\xe9" "b \xc2\x9b[31m \xc0\xaf \xed\xa0\x80 \xf4\x90\x80\x80 \xc3\xa9\xe2\x96\x81\xf0\x9f\x98\x80 \xe2\x96");
  int n = eng_log_read(E, base + 2, l, 10);
  CHECK_INT(n, 1);
  CHECK_STR(l[0].text, "a?b ??[31m ?? ??? ???? \xc3\xa9\xe2\x96\x81\xf0\x9f\x98\x80 ??");
  n = eng_log_read(E, base, l, 10);
  CHECK_INT(n, 3);
  CHECK_STR(l[0].text, "tab?here?newline?[31m");
  CHECK_INT(l[0].seq, base + 1);
  CHECK_INT((int)strlen(l[1].text), ENG_LOG_LEN - 2);  /* the half "é" is left out */
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  int64_t wall = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  CHECK(l[0].wall_ms <= wall && l[0].wall_ms > wall - 5000);
  /* more than the ring: the oldest are gone, a gap shows it */
  for (int i = 0; i < ENG_LOG_LINES + 100; i++) eng_log(E, "line %d", i);
  snap();
  uint32_t newest = S.log_seq;
  n = eng_log_read(E, 0, l, ENG_LOG_LINES);
  CHECK_INT(n, ENG_LOG_LINES);
  CHECK_INT(l[0].seq, newest - ENG_LOG_LINES + 1);
  CHECK_INT(l[n - 1].seq, newest);
  char last[32];
  snprintf(last, sizeof last, "line %d", ENG_LOG_LINES + 99);
  CHECK_STR(l[n - 1].text, last);
  for (int i = 1; i < n; i++) CHECK_INT(l[i].seq, l[i - 1].seq + 1);
  /* oldest first, at most max: a reader that keeps up gets everything in order */
  n = eng_log_read(E, newest - 10, l, 4);
  CHECK_INT(n, 4);
  CHECK_INT(l[0].seq, newest - 9);
  CHECK_INT(eng_log_read(E, newest, l, 4), 0);
  CHECK_INT(eng_log_read(E, newest + 50, l, 4), 0);   /* a reader from the future: nothing */
  CHECK_INT(eng_log_read(E, UINT32_MAX, l, 4), 0);     /* the largest since: nothing (since + 1 must not wrap) */
  CHECK_INT(eng_log_read(E, UINT32_MAX - 1, l, 4), 0);
  CHECK_INT(eng_log_read(E, 0, l, 0), 0);
  /* eng_wait: a log line wakes it, nothing times out */
  snap();
  int64_t t0 = now_us();
  eng_stop(E);
  CHECK(WAIT(1000, !S.running));
  sleep_ms(200);
  snap();
  t0 = now_us();
  CHECK(!eng_wait(E, S.seq, S.log_seq, 200));
  int64_t waited = now_us() - t0;
  CHECK(waited >= 190000 && waited < 1000000);
  CHECK(eng_wait(E, S.seq - 1, S.log_seq, 1000));       /* already different: at once */
  t0 = now_us();
  CHECK(!eng_wait(E, S.seq, S.log_seq, 0));
  CHECK(now_us() - t0 < 100000);
  /* a line from another thread wakes a waiter */
  pthread_t th;
  pthread_create(&th, NULL, logger, E);
  t0 = now_us();
  CHECK(eng_wait(E, S.seq, S.log_seq, 3000));
  CHECK(now_us() - t0 < 2000000);
  pthread_join(th, NULL);
  snap();
  CHECK_INT(logged("from another thread"), 1);
  /* uptime */
  sleep_ms(20);
  eng_snapshot_t s2;
  eng_snapshot(E, &s2);
  CHECK(s2.uptime_us > 0);
  finish();
}

/* stop + start while a POST hangs: it is aborted and its result never shows; the stop releases the module IP it
 * went to (R14: activated less than 3 s ago and not locked since, it may be about to stream to us), then the new
 * session POSTs again. Destroy while a restart hangs: aborted at once, but the exit's release gets 1.5 s. */
static void t_shutdown_and_stale(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  m.http_delay_ms = 3000;
  start(U, H, "127.0.0.2", true);
  CHECK(WAIT(1000, fk_nposts(&m) == 1));
  /* stop + start: the POST in flight is aborted, its result dropped */
  eng_stop(E);
  eng_start(E);
  fk_lock(&m);
  m.http_delay_ms = 0;                                  /* (the next ones are answered at once) */
  fk_unlock(&m);
  CHECK(WAIT(3000, fk_nposts(&m) == 3));
  char body[128];
  fk_post(&m, 1, body, sizeof body);
  CHECK_STR(body, "action=joystick_end");
  fk_post(&m, 2, body, sizeof body);
  CHECK_STR(body, "action=joystick_begin&interval=4000&channels=16");   /* the new session's */
  CHECK(WAIT(2000, S.bound));
  CHECK(WAIT(1000, logged("Released 127.0.0.2: it broadcasts again, any app can connect to it") == 1));
  CHECK_INT(logged("Activation POST -> 127.0.0.2: 200 OK"), 1);        /* the new session's only */
  CHECK_INT(logged("aborted"), 0);
  /* destroy while a restart hangs: the restart is aborted at once; the release after it gets no answer either,
   * and the exit waits for it 1.5 s (WIFI_RELEASE_WAIT_US), not the 5 s of a POST */
  fk_beacon(&m);                                        /* known as ELRS: restarted at once */
  sleep_ms(100);
  fk_lock(&m);
  m.http_delay_ms = 4000;
  fk_unlock(&m);
  int n0 = fk_nposts(&m);
  eng_restart_stream(E);
  CHECK(WAIT(1000, fk_nposts(&m) == n0 + 1));
  int64_t t0 = now_us();
  finish();
  int64_t took = now_us() - t0;
  fprintf(stderr, "  destroy with a POST in flight, its release unanswered: %.0f ms\n", (double)took / 1000);
  CHECK(took >= WIFI_RELEASE_WAIT_US - 100000 && took < WIFI_RELEASE_WAIT_US + 800000);
  CHECK_INT(fk_nposts(&m), n0 + 2);                     /* the restart's joystick_end, then the release's */
  fk_post(&m, n0 + 1, body, sizeof body);
  CHECK_STR(body, "action=joystick_end");
  CHECK(udp_port_free(U));
  fk_stop(&m);
}

/* R14: a stop releases the module (joystick_end alone, on the worker), logged as it goes; eng_wait_released
 * waits for the answers, and cuts short what is left after its timeout (logged); a refused release says so (the
 * module streams on); a port change releases too; a release nobody answers (refused) says why */
static void t_release(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  start(U, H, "", false);
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound && S.nch == 16));
  CHECK(WAIT(2000, S.posts_ok == 1));
  /* no answer within eng_wait_released's timeout: cut short, logged, the module streams on */
  fk_lock(&m);
  m.http_delay_ms = 1000;
  fk_unlock(&m);
  int64_t t0 = now_us();
  eng_stop(E);
  CHECK(eng_wait_applied(E, 1000));
  snap();
  CHECK_INT(S.releasing, 1);
  CHECK_INT(logged("Releasing 127.0.0.2: joystick_end, so it stops streaming and broadcasts again"), 1);
  CHECK(!eng_wait_released(E, 200));
  int64_t took = now_us() - t0;
  CHECK(took >= 190000 && took < 700000);
  CHECK_INT(logged("Releasing 127.0.0.2 failed: no answer in time - it may stream on until its WiFi or power is cycled"), 1);
  CHECK_INT(logged("Released"), 0);
  snap();
  CHECK_INT(S.releasing, 0);
  fk_lock(&m);
  CHECK(m.active);
  m.http_delay_ms = 300;
  fk_unlock(&m);
  /* an answer in 300 ms: waited for (the cut is over: this one is not cut) */
  eng_start(E);
  CHECK(WAIT(3000, S.bound));                           /* (it streamed on: locked at once) */
  t0 = now_us();
  eng_stop(E);
  CHECK(eng_wait_released(E, 3000));
  CHECK(now_us() - t0 >= 250000);
  snap();
  CHECK_INT(S.releasing, 0);
  CHECK_INT(logged("Released 127.0.0.2: it broadcasts again, any app can connect to it"), 1);
  CHECK_INT(logged("failed"), 1);
  CHECK_INT(S.posts, 1);                                /* a release is no activation: not counted */
  CHECK_INT(S.posts_ok, 1);
  CHECK_INT(S.posts_failed, 0);
  CHECK_INT(fk_count(&m, "action=joystick_end"), 2);
  fk_lock(&m);
  CHECK(!m.active);
  fk_unlock(&m);
  t0 = now_us();
  CHECK(eng_wait_released(E, 1000));                    /* none left: at once */
  CHECK(now_us() - t0 < 100000);
  /* refused: said so, and the module streams on (a start locks it again at once) */
  fk_lock(&m);
  m.http_delay_ms = 0;
  m.end_status = 500;
  fk_unlock(&m);
  eng_start(E);
  CHECK(eng_wait_applied(E, 1000));                     /* (listening before the beacon) */
  fk_beacon(&m);
  CHECK(WAIT(3000, S.bound));
  eng_stop(E);
  CHECK(eng_wait_applied(E, 1000));
  CHECK(eng_wait_released(E, 3000));
  CHECK_INT(logged("Releasing 127.0.0.2 failed: HTTP 500 - it may stream on until its WiFi or power is cycled"), 1);
  fk_lock(&m);
  CHECK(m.active);
  m.end_status = 0;
  fk_unlock(&m);
  eng_start(E);
  CHECK(WAIT(3000, S.bound));
  /* a port change: a stop and a start, with its release (the modules send to 11000 only: on another port no
   * frame of theirs comes, and a later stop could not release them) */
  int ends = fk_count(&m, "action=joystick_end");
  g_port = free_udp();
  char err[256];
  CHECK_INT(set_cfg(set_port, err, sizeof err), 0);
  CHECK(eng_wait_applied(E, 2000));
  CHECK(eng_wait_released(E, 3000));
  CHECK_INT(fk_count(&m, "action=joystick_end"), ends + 1);
  CHECK_INT(logged("Released 127.0.0.2"), 2);
  CHECK_INT(logged("Stopped."), 3);                     /* (the port change's stop is quiet) */
  fk_lock(&m);
  CHECK(!m.active);
  fk_unlock(&m);
  /* a module IP that refuses the connection: its activation failed, its release too (sent anyway: an activation
   * that timed out may have reached it) */
  snprintf(g_ip, sizeof g_ip, "127.0.0.3");
  CHECK_INT(set_cfg(set_ip, err, sizeof err), 0);
  CHECK(WAIT(3000, logged("Activation failed for 127.0.0.3: connection refused") == 1));
  eng_stop(E);
  CHECK(eng_wait_applied(E, 1000));
  CHECK(eng_wait_released(E, 3000));
  CHECK_INT(logged("Releasing 127.0.0.3 failed: connection refused - it may stream on until its WiFi or power is cycled"), 1);
  CHECK_INT(logged("Releasing 127.0.0.2: joystick_end"), 4);   /* (it streamed no more: not released again) */
  snap();
  CHECK_INT(S.posts_failed, 1);                         /* the activation's; the releases count nowhere */
  finish();
  fk_stop(&m);
}

/* R14 at exit: eng_destroy releases and waits for the answers, at most WIFI_RELEASE_WAIT_US after the stop:
 * an answer in time is waited for (the module took it); none in time is cut short; a stop's release still under
 * way when the exit comes gets the rest of its 1.5 s, not 1.5 s more; Crossfire is not released */
static void t_release_exit(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m, x;
  fk_start(&m, "127.0.0.2", false, H, U);
  fk_start(&x, "127.0.0.3", true, H, U);
  /* answered in 400 ms */
  start(U, H, "", false);
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound));
  fk_lock(&m);
  m.http_delay_ms = 400;
  fk_unlock(&m);
  int64_t t0 = now_us();
  finish();
  int64_t took = now_us() - t0;
  fprintf(stderr, "  exit, its release answered in 400 ms: %.0f ms\n", (double)took / 1000);
  CHECK(took >= 350000 && took < 1300000);
  CHECK_INT(fk_count(&m, "action=joystick_end"), 1);
  fk_lock(&m);
  CHECK(!m.active);                                     /* the exit did not cut it short */
  m.http_delay_ms = 0;
  fk_unlock(&m);
  /* no answer in time: 1.5 s, then aborted (the module streams on) */
  start(U, H, "", false);
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound));
  fk_lock(&m);
  m.http_delay_ms = 5000;
  fk_unlock(&m);
  t0 = now_us();
  finish();
  took = now_us() - t0;
  fprintf(stderr, "  exit, its release unanswered: %.0f ms\n", (double)took / 1000);
  CHECK(took >= WIFI_RELEASE_WAIT_US - 100000 && took < WIFI_RELEASE_WAIT_US + 800000);
  CHECK_INT(fk_count(&m, "action=joystick_end"), 2);
  fk_lock(&m);
  CHECK(m.active);
  m.http_delay_ms = 0;
  fk_unlock(&m);
  sleep_ms(100);                                        /* (the fake notices the abort) */
  /* a stop 1 s before the exit, its release unanswered: the exit waits the 0.5 s left */
  start(U, H, "", false);
  CHECK(WAIT(3000, S.bound));                           /* (it streamed on) */
  fk_lock(&m);
  m.http_delay_ms = 5000;
  fk_unlock(&m);
  eng_stop(E);
  CHECK(eng_wait_applied(E, 1000));
  sleep_ms(1000);
  t0 = now_us();
  finish();
  took = now_us() - t0;
  fprintf(stderr, "  exit 1 s after a stop whose release is unanswered: %.0f ms\n", (double)took / 1000);
  CHECK(took >= 250000 && took < 1000000);
  CHECK_INT(fk_count(&m, "action=joystick_end"), 3);
  fk_lock(&m);
  m.http_delay_ms = 0;
  m.active = false;                                     /* (powered off and on: out of the way) */
  fk_unlock(&m);
  sleep_ms(100);
  /* Crossfire, known by its beacon: never sent joystick_end, nothing to wait for */
  start(U, H, "", false);
  fk_beacon(&x);
  CHECK(WAIT(5000, S.bound && S.kind == WIFI_KIND_XF));
  eng_stop(E);
  CHECK(eng_wait_applied(E, 1000));
  CHECK_INT(logged("Not releasing 127.0.0.3: Crossfire broadcasts while streaming, other apps find it as it is"), 1);
  snap();
  CHECK_INT(S.releasing, 0);
  eng_start(E);
  CHECK(WAIT(3000, S.bound));
  t0 = now_us();
  finish();
  took = now_us() - t0;
  CHECK(took < 500000);
  CHECK_INT(fk_count(&x, "joystick_end"), 0);
  CHECK_INT(fk_nposts(&x), 1);
  fk_stop(&m);
  fk_stop(&x);
}

/* R14: eng_wait_released counts from the stop, not from its call: a wait that starts 1 s after a stop whose
 * release is unanswered has 0.5 s left, one that starts later none (the CLI's quit after a stop, Ctrl+Z after a
 * stop). What it cuts short says how: the one in flight "no answer in time", one queued behind it (a 2nd source
 * here) "not sent, ..." and it is never sent, not even after the cut */
static void t_release_wait_from_stop(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m, b;
  fk_start(&m, "127.0.0.2", false, H, U);
  fk_start(&b, "127.0.0.4", false, H, U);
  start(U, H, "", false);
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound && !strcmp(S.source, "127.0.0.2")));
  fk_lock(&m);
  m.http_delay_ms = 5000;
  fk_unlock(&m);
  eng_stop(E);
  CHECK(eng_wait_applied(E, 1000));
  sleep_ms(1000);
  int64_t t0 = now_us();
  CHECK(!eng_wait_released(E, 3000));
  int64_t took = now_us() - t0;
  fprintf(stderr, "  wait for a release 1 s after its stop: %.0f ms\n", (double)took / 1000);
  CHECK(took >= 350000 && took < 900000);
  CHECK_INT(logged("Releasing 127.0.0.2 failed: no answer in time - it may stream on until its WiFi or power is cycled"), 1);
  t0 = now_us();
  CHECK(eng_wait_released(E, 3000));                    /* none left: at once */
  CHECK(now_us() - t0 < 100000);
  fk_lock(&m);
  m.http_delay_ms = 0;
  m.active = false;                                     /* (powered off and on) */
  fk_unlock(&m);
  sleep_ms(100);                                        /* (the fake notices the abort) */
  /* a 2nd source behind a locked module that does not answer: cut before its turn, never sent */
  eng_start(E);
  CHECK(eng_wait_applied(E, 1000));
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound && !strcmp(S.source, "127.0.0.2")));
  fk_lock(&b);
  b.active = true;
  b.remote = IP("127.0.0.1");
  b.channels = 16;
  fk_unlock(&b);
  CHECK(WAIT(2000, logged("Ignoring 2nd source 127.0.0.4") >= 1));
  fk_lock(&m);
  m.http_delay_ms = 5000;
  fk_unlock(&m);
  eng_stop(E);
  CHECK(eng_wait_applied(E, 1000));
  CHECK_INT(logged("Releasing 127.0.0.4: joystick_end, so it stops streaming and broadcasts again"), 1);
  t0 = now_us();
  CHECK(!eng_wait_released(E, 200));
  took = now_us() - t0;
  CHECK(took >= 190000 && took < 700000);
  CHECK_INT(logged("Releasing 127.0.0.2 failed: no answer in time - it may stream on until its WiFi or power is cycled"), 2);
  CHECK_INT(logged("Releasing 127.0.0.4 failed: not sent, the wait ran out before its turn - it may stream on until its "
                   "WiFi or power is cycled"), 1);
  snap();
  CHECK_INT(S.releasing, 0);
  sleep_ms(300);
  CHECK_INT(fk_count(&b, "joystick_end"), 0);
  fk_lock(&b);
  CHECK(b.active);
  b.active = false;
  fk_unlock(&b);
  fk_lock(&m);
  m.http_delay_ms = 0;
  fk_unlock(&m);
  t0 = now_us();
  finish();                                             /* (nothing left: the exit waits for nothing) */
  CHECK(now_us() - t0 < 500000);
  CHECK_INT(fk_count(&b, "joystick_end"), 0);
  fk_stop(&m);
  fk_stop(&b);
}

/* R14: eng_wait_releasing, the CLI's wait that shows the release's lines as they come: 0 at a new log line, 1 once
 * no release is under way, -1 when the 1.5 s after the stop ran out, and it cuts nothing short (eng_wait_released
 * does, after it) */
static void t_wait_releasing(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  start(U, H, "", false);
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound));
  /* answered in 300 ms: the "Released" line, then 1 */
  fk_lock(&m);
  m.http_delay_ms = 300;
  fk_unlock(&m);
  snap();
  uint32_t since = S.log_seq;
  eng_stop(E);
  int r, lines = 0;
  int64_t t0 = now_us();
  while ((r = eng_wait_releasing(E, since, 3000)) == 0) {
    snap();
    since = S.log_seq;
    lines++;
  }
  CHECK_INT(r, 1);
  CHECK(lines >= 1);                                    /* ("Releasing ...", "Stopped.": one call or two) */
  CHECK(now_us() - t0 >= 250000 && now_us() - t0 < 1000000);
  CHECK_INT(logged("Released 127.0.0.2: it broadcasts again, any app can connect to it"), 1);
  CHECK_INT(eng_wait_releasing(E, since, 1000), 1);     /* none left: at once */
  CHECK_INT(eng_wait_releasing(E, 0, 1000), 1);
  /* unanswered: -1 when the 1.5 s after the stop ran out, nothing cut short; a line not read yet: 0 at once */
  eng_start(E);
  CHECK(eng_wait_applied(E, 1000));
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound));
  fk_lock(&m);
  m.http_delay_ms = 5000;
  fk_unlock(&m);
  t0 = now_us();
  eng_stop(E);
  CHECK(eng_wait_applied(E, 1000));
  snap();
  CHECK_INT(eng_wait_releasing(E, S.log_seq - 1, 3000), 0);
  CHECK(now_us() - t0 < 1000000);
  snap();
  CHECK_INT(eng_wait_releasing(E, S.log_seq, 200), -1);         /* (its own timeout) */
  snap();
  CHECK_INT(eng_wait_releasing(E, S.log_seq, 3000), -1);        /* (the stop's 1.5 s) */
  int64_t took = now_us() - t0;
  CHECK(took >= WIFI_RELEASE_WAIT_US - 50000 && took < WIFI_RELEASE_WAIT_US + 300000);
  snap();
  CHECK_INT(S.releasing, 1);
  CHECK_INT(logged("failed"), 0);
  CHECK(!eng_wait_released(E, 3000));                   /* the cut */
  CHECK(now_us() - t0 < WIFI_RELEASE_WAIT_US + 400000);
  CHECK_INT(logged("Releasing 127.0.0.2 failed: no answer in time - it may stream on until its WiFi or power is cycled"), 1);
  CHECK_INT(eng_wait_releasing(E, UINT32_MAX, 1000), 1);
  fk_lock(&m);
  m.http_delay_ms = 0;
  fk_unlock(&m);
  finish();
  fk_stop(&m);
}

/* R14: a release nobody answers (a mistyped module IP, a module that just lost power) gets 2 s, not the 5 s of an
 * activation: the one worker sends what queues behind it, a start's joystick_begin to the right module IP, that
 * much later at most */
static void t_release_unanswered(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t a, b;
  fk_start(&a, "127.0.0.2", false, H, U);
  fk_start(&b, "127.0.0.3", false, H, U);
  fk_lock(&b);
  b.http_delay_ms = 7000;                               /* takes the connection, never answers in time */
  fk_unlock(&b);
  start(U, H, "127.0.0.3", false);
  CHECK(WAIT(2000, fk_nposts(&b) == 1));
  sleep_ms(500);
  int64_t t0 = now_us();
  eng_stop(E);
  CHECK(eng_wait_applied(E, 1000));
  CHECK_INT(logged("Releasing 127.0.0.3: joystick_end, so it stops streaming and broadcasts again"), 1);
  snprintf(g_ip, sizeof g_ip, "127.0.0.2");
  char err[256];
  CHECK_INT(set_cfg(set_ip, err, sizeof err), 0);
  eng_start(E);
  CHECK(WAIT(3000, fk_nposts(&a) >= 1));
  int64_t took = now_us() - t0;
  fprintf(stderr, "  a start right after a stop whose release is unanswered: joystick_begin %.0f ms after the stop\n",
          (double)took / 1000);
  CHECK(took >= 1900000 && took < 2500000);
  CHECK(WAIT(1000, S.bound && !strcmp(S.source, "127.0.0.2")));
  CHECK_INT(logged("Releasing 127.0.0.3 failed: timed out after 2 s - it may stream on until its WiFi or power is cycled"), 1);
  sleep_ms(300);
  CHECK_INT(logged("Activation POST -> 127.0.0.2: 200 OK"), 1);   /* (its throttle did not run out while it waited) */
  CHECK_INT(fk_count(&b, "action=joystick_end"), 1);
  CHECK_INT(fk_count(&a, "joystick_begin"), 1);
  finish();
  fk_stop(&a);
  fk_stop(&b);
}

/* the device refuses writes for a while: said once (log + joy_error), then recovered */
static void t_joy_write_error(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  start(U, H, "127.0.0.2", true);
  CHECK(WAIT(3000, S.bound));
  CHECK(WAIT(1000, K.axis[0] == 16393));
  /* one change, then the same values: the failed writes must not leave the device behind */
  pthread_mutex_lock(&kmu);
  K.fail_writes = 3;
  pthread_mutex_unlock(&kmu);
  fk_set(&m, 0, 5555);
  fk_set(&m, 8, 30000);                                 /* button 1 */
  CHECK(WAIT(2000, logged("!! Writing to the virtual joystick failed: ") == 1));
  CHECK(WAIT(2000, logged("The virtual joystick takes the sticks again") == 1));
  CHECK(WAIT(1000, S.joy_error[0] == 0));
  CHECK(S.joy_ok);
  CHECK(WAIT(1000, K.axis[0] == 5555 && K.btn[0]));
  /* the same during a lock loss: the centering gets through after the failures */
  pthread_mutex_lock(&kmu);
  K.fail_writes = 2;
  pthread_mutex_unlock(&kmu);
  fk_mode(&m, 2);
  CHECK(WAIT(5000, !S.bound));
  CHECK(WAIT(1000, logged("!! Writing to the virtual joystick failed: ") == 2));
  CHECK(WAIT(1000, sink_centered()));
  fk_mode(&m, 0);
  CHECK(WAIT(3000, S.bound && K.axis[0] == 5555));
  pthread_mutex_lock(&kmu);
  K.fail_writes = 1000000;
  pthread_mutex_unlock(&kmu);
  fk_set(&m, 0, 7000);
  CHECK(WAIT(2000, !strncmp(S.joy_error, "writing to the virtual joystick failed: ", 40)));
  CHECK_INT(logged("!! Writing to the virtual joystick failed: "), 3);
  pthread_mutex_lock(&kmu);
  K.fail_writes = 0;
  pthread_mutex_unlock(&kmu);
  finish();
  fk_stop(&m);
}

/* A burst of commands (a script, several web clients): none is dropped. A config in the middle of hundreds
 * of stop/start pairs is applied; the last of start/stop wins. */
static void t_command_burst(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  start(U, H, "", false);
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound && S.nch == 16));
  char err[256];
  for (int i = 0; i < 300; i++) { eng_stop(E); eng_start(E); eng_restart_stream(E); }
  CHECK_INT(set_cfg(set_8ch, err, sizeof err), 0);
  for (int i = 0; i < 300; i++) { eng_stop(E); eng_start(E); }
  eng_stop(E);
  eng_start(E);
  CHECK(eng_wait_applied(E, 3000));
  CHECK(WAIT(3000, logged("Asking for 250 Hz (4000 us) and 8 channels") == 1));
  CHECK_INT(logged("Too many commands"), 0);
  /* the stops released the module (R14): its beacon gets it a joystick_begin with the request (one taken while
   * frames that came before its joystick_end hold the lock is ignored: beacons until it is in) */
  int64_t end = now_us() + 8000000;
  while (now_us() < end) {
    snap();
    if (S.running && S.bound && S.nch == 8) break;
    fk_beacon(&m);
    sleep_ms(250);
  }
  CHECK(S.running && S.bound && S.nch == 8);           /* applied: the module streams 8 now */
  CHECK_INT(S.cfg.channels, 8);
  /* the last one wins: a stop at the end of a burst stops */
  for (int i = 0; i < 100; i++) { eng_start(E); eng_stop(E); }
  CHECK(eng_wait_applied(E, 3000));
  snap();
  CHECK(!S.running);
  finish();
  fk_stop(&m);
}

/* t_update_config's threads: 0 sets channels 4..16 (last 5), 1 sets the interval (last 3000), each from a
 * snapshot taken before the other's changes */
static void *one_field(void *arg) {
  int who = (int)(intptr_t)arg;
  char err[256];
  for (int i = 0; i < 200; i++) {
    eng_snapshot_t s;
    eng_snapshot(E, &s);
    eng_config_t c = s.cfg;
    if (who == 0) c.channels = i == 199 ? 5 : 4 + i % 13;
    else c.interval_us = i == 199 ? 3000 : 1000 + 1000 * (i % 50);
    sched_yield();
    eng_update_config(E, &c, who == 0 ? ENG_F_CHANNELS : ENG_F_INTERVAL, NULL, err, sizeof err);
  }
  return NULL;
}

/* eng_update_config changes only the fields it is given, merged under the engine's lock: a change made by
 * someone else (the CLI) after this caller's snapshot is kept; out = the config now asked for */
static void t_update_config(void) {
  uint16_t U = free_udp();
  start(U, 80, "", false);
  char err[256];
  eng_snapshot(E, &S);
  eng_config_t web = S.cfg;                             /* the web page's (stale) view */
  eng_config_t cli = S.cfg;
  cli.channels = 8;
  CHECK_INT(eng_update_config(E, &cli, ENG_F_CHANNELS, NULL, err, sizeof err), 0);
  web.interval_us = 2000;
  eng_config_t out;
  CHECK_INT(eng_update_config(E, &web, ENG_F_INTERVAL, &out, err, sizeof err), 0);
  CHECK_INT(out.interval_us, 2000);
  CHECK_INT(out.channels, 8);                           /* the CLI's change stays */
  eng_snapshot(E, &S);
  CHECK_INT(S.cfg.interval_us, 2000);
  CHECK_INT(S.cfg.channels, 8);
  CHECK_INT(saved.channels, 8);                         /* and is saved with it */
  CHECK_INT(saved.interval_us, 2000);
  /* out holds the config in effect: the module IP in its plain form */
  eng_config_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.module_ip, sizeof c.module_ip, "\r\n 127.0.0.2\t\n");
  CHECK_INT(eng_update_config(E, &c, ENG_F_MODULE_IP, &out, err, sizeof err), 0);
  CHECK_STR(out.module_ip, "127.0.0.2");
  CHECK_INT(out.port, U);
  eng_snapshot(E, &S);
  CHECK_STR(S.cfg.module_ip, "127.0.0.2");
  /* an invalid field: -1, nothing changed (the other fields of c are not looked at) */
  c.channels = 3;
  c.port = 0;
  CHECK_INT(eng_update_config(E, &c, ENG_F_CHANNELS, &out, err, sizeof err), -1);
  CHECK_STR(err, "channels: 3 is not supported - use 4 to 16");
  CHECK_INT(eng_update_config(E, &c, 0, &out, err, sizeof err), 0);    /* no field: nothing changes */
  CHECK_INT(out.channels, 8);
  CHECK_INT(out.port, U);
  /* two threads at once, each changing only its own field from a stale view of the rest (the CLI and the
   * web page): the last value of each lands, none is undone by the other */
  pthread_t th[2];
  for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, one_field, (void *)(intptr_t)i);
  for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
  eng_snapshot(E, &S);
  CHECK_INT(S.cfg.channels, 5);
  CHECK_INT(S.cfg.interval_us, 3000);
  CHECK_INT(saved.channels, 5);
  CHECK_INT(saved.interval_us, 3000);
  finish();
}

/* scripted commands in a row see each other: s twice = stop, then start; status after stop says stopped */
static void t_wait_applied(void) {
  uint16_t U = free_udp();
  start(U, 80, "", false);
  eng_stop(E);
  CHECK(eng_wait_applied(E, 2000));
  eng_snapshot(E, &S);
  CHECK(!S.running);
  CHECK_STR(S.status, "stopped");
  eng_start(E);
  CHECK(eng_wait_applied(E, 2000));
  eng_snapshot(E, &S);
  CHECK(S.running && S.listening);
  uint16_t U2 = free_udp();
  g_port = U2;
  char err[256];
  CHECK_INT(set_cfg(set_port, err, sizeof err), 0);
  CHECK(eng_wait_applied(E, 2000));
  eng_snapshot(E, &S);
  CHECK(S.listening && S.cfg.port == U2);
  CHECK(udp_port_free(U));
  CHECK(eng_wait_applied(E, 0));                        /* nothing pending: at once */
  finish();
}

/* A restart whose joystick_end is refused: the joystick_begin got its 2xx, but a streaming module only moved
 * its stream and kept its old request. The restart failed (logged so, counted as failed) and the core sends
 * it again 5 s after the first send while the module streams to us (R6). */
static void t_restart_end_refused(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  start(U, H, "", false);
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound && S.nch == 16));
  CHECK(WAIT(2000, S.posts_ok == 1));
  CHECK_INT(fk_interval(&m), 4000);
  fk_lock(&m);
  m.end_status = 500;
  fk_unlock(&m);
  char err[256];
  int64_t t0 = now_us();
  CHECK_INT(set_cfg(inc_rate, err, sizeof err), 0);
  CHECK(WAIT(3000, logged("Stream end POST failed for 127.0.0.2: HTTP 500") == 1));
  CHECK(WAIT(2000, logged("The restart of 127.0.0.2 failed - sending it again in") == 1));
  CHECK_INT(logged("Activation POST -> 127.0.0.2: 200 OK"), 2);
  CHECK_INT(logged("Stream end POST -> 127.0.0.2"), 0);
  CHECK(WAIT(2000, S.posts_failed == 1));
  CHECK_INT(S.posts_ok, 1);
  CHECK_INT(S.restarts, 1);
  CHECK_INT(fk_interval(&m), 4000);                     /* the module kept its old request */
  CHECK(S.bound);
  /* the module takes joystick_end again: the second send (5 s after the first) does it */
  fk_lock(&m);
  m.end_status = 0;
  fk_unlock(&m);
  CHECK(WAIT(8000, fk_count(&m, "action=joystick_end") == 2 && S.posts_ok == 2));
  CHECK(now_us() - t0 >= 4900000);
  CHECK_INT(fk_interval(&m), 2000);
  CHECK_INT(S.restarts, 2);
  CHECK_INT(S.posts_failed, 1);
  CHECK_INT(logged("Stream end POST -> 127.0.0.2: 200"), 1);
  CHECK(WAIT(3000, S.bound && S.rate_hz > 350));
  sleep_ms(300);
  CHECK_INT(fk_count(&m, "action=joystick_end"), 2);
  CHECK_INT(logged("The restart of"), 1);
  finish();
  fk_stop(&m);
}

/* Our own restart pauses the stream (joystick_end, then joystick_begin): not a gap, not logged as one */
static void t_own_restart_no_gap(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  start(U, H, "", false);
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound && S.nch == 16));
  CHECK(WAIT(2000, S.posts_ok == 1));
  fk_lock(&m);
  m.http_delay_ms = 300;                                /* a slow module web server: the pause > 250 ms */
  fk_unlock(&m);
  char err[256];
  CHECK_INT(set_cfg(inc_rate, err, sizeof err), 0);
  CHECK(WAIT(4000, S.restarts == 1 && logged("Activation POST -> 127.0.0.2: 200 OK") == 2));
  CHECK(WAIT(3000, S.rate_hz > 350));
  CHECK_INT(fk_interval(&m), 2000);
  uint32_t g0 = 0;
  int gl0 = 0;
  CHECK_INT(S.gaps, 0);
  CHECK_INT(logged("The stick data paused"), 0);
  /* a real pause afterwards still counts */
  fk_mode(&m, 2);
  sleep_ms(500);
  fk_mode(&m, 0);
  CHECK(WAIT(2000, S.gaps == 1));
  /* a slow rate (50 Hz, 40 Hz: a frame every 20-25 ms) and a module that stops at once on joystick_end and
   * answers later (ELRS stops, then answers): the pause begins with a frame that came before the restart was
   * even sent - still ours */
  fk_lock(&m);
  m.act_first = true;
  fk_unlock(&m);
  static const int IV[] = { 20000, 25000, 20000, 25000 };
  uint32_t r0 = S.restarts;
  for (int i = 0; i < 4; i++) {
    g_iv = IV[i];
    CHECK_INT(set_cfg(set_iv, err, sizeof err), 0);
    CHECK(WAIT(5000, S.restarts == r0 + (uint32_t)i + 1 && fk_interval(&m) == IV[i]));
    CHECK(WAIT(5000, S.posts_ok + S.posts_failed == S.posts));
    uint32_t f1 = S.frames;
    CHECK(WAIT(3000, S.frames > f1 + 5));
    if (i == 0) {
      snap();
      g0 = S.gaps;                                      /* (from 500 Hz to 50 Hz: frames came after it was sent) */
      gl0 = logged("The stick data paused");
    }
  }
  CHECK_INT(S.gaps, g0);
  CHECK_INT(logged("The stick data paused"), gl0);
  finish();
  fk_stop(&m);
}

/* The module IP and the rate in one change: the module being left gets no joystick_end; the new one gets
 * a joystick_begin with the new request, and when it already streams what it was asked for before, a
 * restart at its lock. A stop then releases both (R14): the locked one, and the one left, which streams on */
static void t_switch_module_and_rate(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t a, b;
  fk_start(&a, "127.0.0.2", false, H, U);
  fk_start(&b, "127.0.0.3", false, H, U);
  start(U, H, "", false);
  fk_beacon(&a);
  CHECK(WAIT(5000, S.bound && !strcmp(S.source, "127.0.0.2")));
  /* B was not streaming: one joystick_begin with the new request */
  eng_config_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.module_ip, sizeof c.module_ip, "127.0.0.3");
  c.interval_us = 2000;
  char err[256];
  CHECK_INT(eng_update_config(E, &c, ENG_F_MODULE_IP | ENG_F_INTERVAL, NULL, err, sizeof err), 0);
  CHECK(WAIT(5000, S.bound && !strcmp(S.source, "127.0.0.3")));
  CHECK(WAIT(3000, S.rate_hz > 350));
  CHECK_INT(fk_count(&a, "joystick_end"), 0);
  CHECK_INT(fk_nposts(&a), 1);
  CHECK_INT(fk_count(&b, "action=joystick_begin&interval=2000&channels=16"), 1);
  CHECK_INT(fk_count(&b, "joystick_end"), 0);
  /* back to A (it streams on to us at 250 Hz, as first asked) with 200 Hz: A is restarted when it locks */
  snprintf(c.module_ip, sizeof c.module_ip, "127.0.0.2");
  c.interval_us = 5000;
  CHECK_INT(eng_update_config(E, &c, ENG_F_MODULE_IP | ENG_F_INTERVAL, NULL, err, sizeof err), 0);
  CHECK(WAIT(5000, S.bound && !strcmp(S.source, "127.0.0.2")));
  CHECK(WAIT(5000, fk_count(&a, "action=joystick_end") == 1));
  CHECK(WAIT(3000, fk_count(&a, "action=joystick_begin&interval=5000&channels=16") >= 1));
  CHECK(WAIT(3000, fk_interval(&a) == 5000));
  CHECK(WAIT(4000, S.rate_hz > 150 && S.rate_hz < 250));
  CHECK_INT(fk_count(&b, "joystick_end"), 0);           /* the one left now: nothing */
  CHECK(WAIT(1000, logged("127.0.0.2 still streams 250 Hz, 16 channels from before the change") == 1));
  /* R14: a stop releases the locked module, then the one left by the switch, which streams to us still (its
   * frames dropped) */
  uint32_t d0 = S.dropped_other;
  CHECK(WAIT(2000, S.dropped_other > d0 + 10));
  int ends_a = fk_count(&a, "action=joystick_end");
  eng_stop(E);
  CHECK(eng_wait_applied(E, 1000));
  CHECK_INT(logged("Releasing 127.0.0.2: joystick_end, so it stops streaming and broadcasts again"), 1);
  CHECK_INT(logged("Releasing 127.0.0.3: joystick_end, so it stops streaming and broadcasts again"), 1);
  CHECK(eng_wait_released(E, 3000));
  CHECK_INT(logged("Released 127.0.0.2: it broadcasts again, any app can connect to it"), 1);
  CHECK_INT(logged("Released 127.0.0.3: it broadcasts again, any app can connect to it"), 1);
  CHECK_INT(fk_count(&a, "action=joystick_end"), ends_a + 1);
  CHECK_INT(fk_count(&b, "action=joystick_end"), 1);
  fk_lock(&a);
  CHECK(!a.active);
  fk_unlock(&a);
  fk_lock(&b);
  CHECK(!b.active);
  fk_unlock(&b);
  finish();
  fk_stop(&a);
  fk_stop(&b);
}

/* Setting the module IP to the module streaming now: the joystick is not centered (no blip), no relock */
static void t_same_target_no_center(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  for (int i = 0; i < 16; i++) m.values[i] = 1000;
  m.values[8] = 32767;                                  /* button 1 held */
  start(U, H, "", true);
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound));
  CHECK(WAIT(2000, K.axis[0] == 1000 && K.btn[0]));
  sink_pump();
  int centers = K.abs_center, locks = logged("Joystick source locked");
  snprintf(g_ip, sizeof g_ip, "127.0.0.2");
  char err[256];
  CHECK_INT(set_cfg(set_ip, err, sizeof err), 0);
  CHECK(WAIT(1000, logged("Target module set to 127.0.0.2 - streaming from it already") == 1));
  sleep_ms(300);
  snap();
  CHECK_INT(K.abs_center, centers);
  CHECK(K.btn[0]);
  CHECK_INT(logged("Joystick source locked"), locks);
  CHECK_INT(fk_nposts(&m), 1);                          /* nothing sent */
  CHECK(S.bound);
  finish();
  fk_stop(&m);
}

/* Stop and start (or a port change) keep what the beacons said: an ELRS module streaming on (it sends no
 * beacon while it streams) stays "ELRS", and a rate change restarts it at once, not after 20 s */
static void t_kind_after_stop_start(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  start(U, H, "", false);
  fk_beacon(&m);
  CHECK(WAIT(5000, S.bound && S.kind == WIFI_KIND_ELRS));
  /* it streams on when its release is refused (R14; or when the program was killed) */
  fk_lock(&m);
  m.end_status = 500;
  fk_unlock(&m);
  eng_stop(E);
  eng_start(E);
  CHECK(eng_wait_applied(E, 3000));                     /* (the snapshot is of the new session from here) */
  CHECK_INT(logged("Stopped."), 1);
  CHECK(WAIT(2000, logged("Releasing 127.0.0.2 failed: HTTP 500") == 1));
  fk_lock(&m);
  m.end_status = 0;
  fk_unlock(&m);
  CHECK(WAIT(3000, S.bound));
  CHECK_INT(S.kind, WIFI_KIND_ELRS);
  CHECK_STR(S.module_name, "Unified TX");
  char err[256];
  CHECK_INT(set_cfg(inc_rate, err, sizeof err), 0);
  CHECK(WAIT(2000, S.restarts == 1));
  CHECK_INT(logged("may be Crossfire"), 0);
  /* a port change too (a new session on the new port; its release refused again) */
  uint16_t U2 = free_udp();
  fk_lock(&m);
  m.dest_port = U2;
  m.end_status = 500;
  fk_unlock(&m);
  g_port = U2;
  CHECK_INT(set_cfg(set_port, err, sizeof err), 0);
  CHECK(WAIT(2000, logged("Releasing 127.0.0.2 failed: HTTP 500") == 2));
  fk_lock(&m);
  m.end_status = 0;
  fk_unlock(&m);
  CHECK(WAIT(3000, S.bound && S.cfg.port == U2));
  CHECK_INT(S.kind, WIFI_KIND_ELRS);
  CHECK_INT(set_cfg(set_8ch, err, sizeof err), 0);
  CHECK(WAIT(2000, S.restarts == 2));
  CHECK_INT(logged("may be Crossfire"), 0);
  /* the port and the module IP in one change while listening: used at once, said so (released, then asked
   * again at once: the module IP gets its joystick_begin when listening starts) */
  uint16_t U3 = free_udp();
  fk_lock(&m);
  m.dest_port = U3;
  fk_unlock(&m);
  eng_config_t c;
  memset(&c, 0, sizeof c);
  c.port = U3;
  snprintf(c.module_ip, sizeof c.module_ip, "127.0.0.2");
  CHECK_INT(eng_update_config(E, &c, ENG_F_PORT | ENG_F_MODULE_IP, NULL, err, sizeof err), 0);
  CHECK(WAIT(3000, logged("Module IP set to 127.0.0.2") == 1));
  CHECK_INT(logged("used when listening starts"), 0);
  CHECK(WAIT(3000, S.bound && S.cfg.port == U3));
  finish();
  fk_stop(&m);
}

/* many threads poking the API while frames stream (for the sanitizers) */
static void *poker(void *arg) {
  (void)arg;
  char err[256];
  eng_logline_t l[16];
  for (int i = 0; i < 200; i++) {
    eng_snapshot_t s;
    eng_snapshot(E, &s);
    eng_config_t c = s.cfg;
    c.channels = 8 + (i % 9);
    eng_set_config(E, &c, err, sizeof err);
    if (i % 50 == 0) eng_restart_stream(E);
    if (i % 70 == 0) { eng_stop(E); eng_start(E); }
    if (i % 3 == 0) {                                   /* each thread its own field: merged, never undone */
      eng_config_t u;
      memset(&u, 0, sizeof u);
      u.interval_us = 1000 + 1000 * (i % 5);
      eng_update_config(E, &u, ENG_F_INTERVAL, NULL, err, sizeof err);
    }
    eng_log_read(E, s.log_seq > 10 ? s.log_seq - 10 : 0, l, 16);
    eng_log(E, "poke %d", i);
    eng_wait(E, s.seq, s.log_seq, 1);
  }
  return NULL;
}

static void t_concurrency(void) {
  uint16_t U = free_udp(), H = free_tcp();
  fake_t m;
  fk_start(&m, "127.0.0.2", false, H, U);
  start(U, H, "127.0.0.2", true);
  CHECK(WAIT(3000, S.bound));
  pthread_t th[4];
  for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, poker, NULL);
  for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
  snap();
  CHECK(S.running);
  pthread_mutex_lock(&kmu);
  CHECK_INT(K.bad_writes, 0);
  pthread_mutex_unlock(&kmu);
  finish();
  fk_stop(&m);
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  setvbuf(stderr, NULL, _IONBF, 0);
  int64_t t0 = now_us();
  /* one part: test_engine config|flow|xf|mismatch|status|bind|joyfail|log|shutdown|joywrite|concurrency|burst|
   * update|applied|owngap|endrefused|switch|sametarget|kind|statusswitch|release|releaseexit|releasewait|
   * releaseslow|releasing */
  static const struct { const char *name; void (*fn)(void); } PARTS[] = {
    { "config", t_config_check }, { "flow", t_flow }, { "xf", t_crossfire_and_target }, { "mismatch", t_mismatch },
    { "status", t_status_texts }, { "bind", t_bind_error }, { "joyfail", t_joystick_fails }, { "log", t_log_ring },
    { "shutdown", t_shutdown_and_stale }, { "joywrite", t_joy_write_error }, { "concurrency", t_concurrency },
    { "burst", t_command_burst }, { "update", t_update_config }, { "applied", t_wait_applied },
    { "owngap", t_own_restart_no_gap }, { "endrefused", t_restart_end_refused }, { "switch", t_switch_module_and_rate },
    { "sametarget", t_same_target_no_center }, { "kind", t_kind_after_stop_start },
    { "statusswitch", t_status_switch_in_flight }, { "release", t_release }, { "releaseexit", t_release_exit },
    { "releasewait", t_release_wait_from_stop }, { "releaseslow", t_release_unanswered },
    { "releasing", t_wait_releasing },
  };
  if (argc > 1) {
    for (size_t i = 0; i < sizeof PARTS / sizeof PARTS[0]; i++)
      if (!strcmp(argv[1], PARTS[i].name)) { PARTS[i].fn(); T_DONE(); }
    fprintf(stderr, "unknown part %s\n", argv[1]);
    return 2;
  }

  t_config_check();
  t_flow();
  t_crossfire_and_target();
  t_mismatch();
  t_status_texts();
  t_status_switch_in_flight();
  t_bind_error();
  t_joystick_fails();
  t_log_ring();
  t_shutdown_and_stale();
  t_joy_write_error();
  t_concurrency();
  t_command_burst();
  t_update_config();
  t_wait_applied();
  t_own_restart_no_gap();
  t_restart_end_refused();
  t_switch_module_and_rate();
  t_same_target_no_center();
  t_kind_after_stop_start();
  t_release();
  t_release_exit();
  t_release_wait_from_stop();
  t_release_unanswered();
  t_wait_releasing();
  fprintf(stderr, "  test_engine took %.1f s\n", (double)(now_us() - t0) / 1e6);
  T_DONE();
}

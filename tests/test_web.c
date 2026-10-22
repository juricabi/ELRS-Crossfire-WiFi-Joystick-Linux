/* STUBS: engine */
/* The web server (src/web.c, docs/WEB_API.md) over real sockets, with a stub engine: request
 * parsing, the Host / Origin / Content-Type checks, size limits, every endpoint, JSON escaping of
 * hostile strings, POST /api/config validation, slow and partial clients, many connections. */
#include "web.h"
#include "wifi_net.h"
#include "wifi_proto.h"
#include "webpage.h"
#include "tests/t.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ================================================================ stub engine */
struct engine { int unused; };
static struct engine ENG;
static pthread_mutex_t smu = PTHREAD_MUTEX_INITIALIZER;
static eng_snapshot_t S;                       /* what eng_snapshot returns */
static int n_start, n_stop, n_restart, n_set;
static eng_config_t last_set;
static unsigned last_fields;                   /* eng_update_config's fields */
static char set_warning[160];                  /* eng_set_config: 0 with this in err */
#define NLOGMAX 1200
static eng_logline_t LOGS[NLOGMAX];
static int nlog;

void eng_config_default(eng_config_t *c) {
  memset(c, 0, sizeof *c);
  c->port = WIFI_PORT_DEFAULT;
  c->http_port = WIFI_HTTP_PORT_DEFAULT;
  c->interval_us = WIFI_INTERVAL_DEFAULT;
  c->channels = WIFI_CHANNELS_DEFAULT;
}

int eng_config_check(const eng_config_t *c, char *err, size_t cap) {
  uint32_t ip = 0;
  int r = wifi_parse_ip(c->module_ip, &ip);       /* as the engine: blank (spaces, line ends) = discovery */
  if (r < 0 || (r > 0 && !ip)) {
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

engine_t *eng_create(const eng_config_t *cfg, const eng_options_t *opt, char *err, size_t cap) { return &ENG; }
void eng_destroy(engine_t *e) {}
void eng_start(engine_t *e) { pthread_mutex_lock(&smu); n_start++; S.running = true; pthread_mutex_unlock(&smu); }
void eng_stop(engine_t *e) { pthread_mutex_lock(&smu); n_stop++; S.running = false; pthread_mutex_unlock(&smu); }
void eng_restart_stream(engine_t *e) { pthread_mutex_lock(&smu); n_restart++; pthread_mutex_unlock(&smu); }

/* like the engine: the given fields onto the current config, checked, module_ip in its plain form */
int eng_update_config(engine_t *e, const eng_config_t *c, unsigned fields, eng_config_t *out, char *err, size_t cap) {
  pthread_mutex_lock(&smu);
  eng_config_t n = S.cfg;
  pthread_mutex_unlock(&smu);
  if (fields & ENG_F_MODULE_IP) memcpy(n.module_ip, c->module_ip, sizeof n.module_ip);
  if (fields & ENG_F_PORT) n.port = c->port;
  if (fields & ENG_F_HTTP_PORT) n.http_port = c->http_port;
  if (fields & ENG_F_INTERVAL) n.interval_us = c->interval_us;
  if (fields & ENG_F_CHANNELS) n.channels = c->channels;
  if (eng_config_check(&n, err, cap)) return -1;
  uint32_t ip;
  char plain[16] = "";
  if (wifi_parse_ip(n.module_ip, &ip) == 1) wifi_ip_str(ip, plain);
  memset(n.module_ip, 0, sizeof n.module_ip);
  memcpy(n.module_ip, plain, strlen(plain));
  pthread_mutex_lock(&smu);
  S.cfg = n;
  last_set = n;
  last_fields = fields;
  n_set++;
  snprintf(err, cap, "%s", set_warning);
  pthread_mutex_unlock(&smu);
  if (out) *out = n;
  return 0;
}

int eng_set_config(engine_t *e, const eng_config_t *c, char *err, size_t cap) {
  return eng_update_config(e, c, ENG_F_ALL, NULL, err, cap);
}
bool eng_wait_applied(engine_t *e, int timeout_ms) { return true; }

void eng_snapshot(engine_t *e, eng_snapshot_t *out) {
  pthread_mutex_lock(&smu);
  *out = S;
  pthread_mutex_unlock(&smu);
}

bool eng_wait(engine_t *e, uint32_t seq, uint32_t log_seq, int timeout_ms) { return false; }

int eng_log_read(engine_t *e, uint32_t since, eng_logline_t *out, int max) {
  int k = 0;
  pthread_mutex_lock(&smu);
  for (int i = 0; i < nlog && k < max; i++)       /* the ring keeps the newest ENG_LOG_LINES */
    if (LOGS[i].seq > since && i >= nlog - ENG_LOG_LINES) out[k++] = LOGS[i];
  pthread_mutex_unlock(&smu);
  return k;
}

void eng_log(engine_t *e, const char *fmt, ...) {
  pthread_mutex_lock(&smu);
  if (nlog < NLOGMAX) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(LOGS[nlog].text, sizeof LOGS[nlog].text, fmt, ap);
    va_end(ap);
    LOGS[nlog].seq = (uint32_t)nlog + 1;
    LOGS[nlog].wall_ms = 1729590000000LL + nlog;
    nlog++;
    S.log_seq = (uint32_t)nlog;
  }
  pthread_mutex_unlock(&smu);
}

static void log_raw(const char *text) {          /* bypasses printf: any bytes */
  pthread_mutex_lock(&smu);
  if (nlog < NLOGMAX) {
    snprintf(LOGS[nlog].text, sizeof LOGS[nlog].text, "%s", text);
    LOGS[nlog].seq = (uint32_t)nlog + 1;
    LOGS[nlog].wall_ms = 1729590000000LL + nlog;
    nlog++;
    S.log_seq = (uint32_t)nlog;
  }
  pthread_mutex_unlock(&smu);
}

static void reset_snapshot(void) {
  pthread_mutex_lock(&smu);
  memset(&S, 0, sizeof S);
  eng_config_default(&S.cfg);
  S.seq = 7;
  S.running = S.listening = true;
  S.state = WIFI_ST_STREAMING;
  snprintf(S.detail, sizeof S.detail, "192.168.1.50");
  snprintf(S.status, sizeof S.status, "streaming");
  S.bound = true;
  snprintf(S.source, sizeof S.source, "192.168.1.50");
  S.kind = WIFI_KIND_ELRS;
  snprintf(S.module_name, sizeof S.module_name, "Unified TX");
  S.rate_hz = 249.84;
  S.jitter_ms = 0.416;
  S.frames_last_s = 250;
  S.nch = 16;
  for (int i = 0; i < WIFI_MAX_CH; i++) S.raw[i] = (uint16_t)(i * 2000 + 7);
  S.frames = 3120; S.rx = 3125; S.beacons = 2; S.malformed = 1; S.dropped_filter = 3; S.dropped_other = 4;
  S.posts = 5; S.posts_ok = 4; S.posts_failed = 1; S.restarts = 6; S.gaps = 2; S.gap_max_us = 312600;
  S.joy_enabled = S.joy_ok = S.joy_buttons = true;
  snprintf(S.joy_name, sizeof S.joy_name, "%s", ENG_DEVICE_NAME);
  snprintf(S.joy_device, sizeof S.joy_device, "/dev/input/event7");
  S.log_seq = (uint32_t)nlog;
  S.uptime_us = 12549999;
  pthread_mutex_unlock(&smu);
}

/* the stub's state as the tests see it (under its lock: the server thread writes it) */
static int geti(const int *v) { pthread_mutex_lock(&smu); int x = *v; pthread_mutex_unlock(&smu); return x; }
static unsigned getfields(void) { pthread_mutex_lock(&smu); unsigned x = last_fields; pthread_mutex_unlock(&smu); return x; }
static eng_config_t last(void) { pthread_mutex_lock(&smu); eng_config_t c = last_set; pthread_mutex_unlock(&smu); return c; }
static void set_warn(const char *w) { pthread_mutex_lock(&smu); snprintf(set_warning, sizeof set_warning, "%s", w); pthread_mutex_unlock(&smu); }

/* ================================================================ a strict JSON validator */
typedef struct { const char *p, *e; int depth; } jv_t;
static bool jv_value(jv_t *j);
static void jv_ws(jv_t *j) { while (j->p < j->e && strchr(" \t\r\n", *j->p) && *j->p) j->p++; }
static bool jv_lit(jv_t *j, const char *w) {
  size_t n = strlen(w);
  if ((size_t)(j->e - j->p) < n || memcmp(j->p, w, n)) return false;
  j->p += n;
  return true;
}
static bool jv_string(jv_t *j) {
  if (j->p >= j->e || *j->p != '"') return false;
  j->p++;
  while (j->p < j->e) {
    unsigned char c = (unsigned char)*j->p;
    if (c == '"') { j->p++; return true; }
    if (c < 0x20) return false;
    if (c == '\\') {
      if (j->e - j->p < 2) return false;
      char x = j->p[1];
      if (x == 'u') {
        if (j->e - j->p < 6) return false;
        for (int i = 2; i < 6; i++) if (!strchr("0123456789abcdefABCDEF", j->p[i]) || !j->p[i]) return false;
        j->p += 6;
      } else if (x && strchr("\"\\/bfnrt", x)) {
        j->p += 2;
      } else {
        return false;
      }
      continue;
    }
    if (c < 0x80) { j->p++; continue; }
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
    if (!n || j->e - j->p < n) return false;
    for (int i = 1; i < n; i++) if (((unsigned char)j->p[i] & 0xC0) != 0x80) return false;
    j->p += n;
  }
  return false;
}
static bool jv_number(jv_t *j) {
  const char *s = j->p;
  if (j->p < j->e && *j->p == '-') j->p++;
  if (j->p >= j->e || *j->p < '0' || *j->p > '9') return false;
  if (*j->p == '0') j->p++;
  else while (j->p < j->e && *j->p >= '0' && *j->p <= '9') j->p++;
  if (j->p < j->e && *j->p == '.') {
    j->p++;
    if (j->p >= j->e || *j->p < '0' || *j->p > '9') return false;
    while (j->p < j->e && *j->p >= '0' && *j->p <= '9') j->p++;
  }
  return j->p > s;
}
static bool jv_value(jv_t *j) {
  if (++j->depth > 32) return false;
  jv_ws(j);
  bool ok = false;
  if (j->p >= j->e) ok = false;
  else if (*j->p == '{' || *j->p == '[') {
    char close = *j->p == '{' ? '}' : ']';
    bool obj = close == '}';
    j->p++;
    jv_ws(j);
    if (j->p < j->e && *j->p == close) { j->p++; ok = true; }
    else for (;;) {
      if (obj) {
        jv_ws(j);
        if (!jv_string(j)) break;
        jv_ws(j);
        if (j->p >= j->e || *j->p != ':') break;
        j->p++;
      }
      if (!jv_value(j)) break;
      jv_ws(j);
      if (j->p < j->e && *j->p == ',') { j->p++; continue; }
      if (j->p < j->e && *j->p == close) { j->p++; ok = true; }
      break;
    }
  } else if (*j->p == '"') ok = jv_string(j);
  else if (*j->p == 't') ok = jv_lit(j, "true");
  else if (*j->p == 'f') ok = jv_lit(j, "false");
  else if (*j->p == 'n') ok = jv_lit(j, "null");
  else ok = jv_number(j);
  j->depth--;
  return ok;
}
static bool json_valid(const char *s, size_t n) {
  jv_t j = { s, s + n, 0 };
  if (!jv_value(&j)) return false;
  jv_ws(&j);
  return j.p == j.e;
}

/* ================================================================ an HTTP client */
static uint16_t PORT;
static char HOSTHDR[64];                        /* "127.0.0.1:<port>" */
static char INST[64];                           /* "instance" of /api/status and /api/log */
static int acao_seen;                           /* responses with Access-Control-* headers */

typedef struct {
  int status;
  char head[8192];
  char *body;
  size_t body_len;
  long clen;                                    /* Content-Length, -1 when absent */
} resp_t;

static int64_t mono_ms(void) { return wifi_net_now_us() / 1000; }

static int connect_to(uint16_t port, int rcvbuf) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (rcvbuf) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
  return fd;
}

static bool send_all(int fd, const char *p, size_t n) {
  while (n) {
    ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    p += r;
    n -= (size_t)r;
  }
  return true;
}

/* everything until EOF (or timeout_ms without data): malloc'd, NUL-terminated */
static char *read_all(int fd, size_t *len, int timeout_ms, bool *timed_out) {
  size_t cap = 4096, n = 0;
  char *b = malloc(cap);
  if (timed_out) *timed_out = false;
  for (;;) {
    struct pollfd p = { fd, POLLIN, 0 };
    int r = poll(&p, 1, timeout_ms);
    if (r == 0) { if (timed_out) *timed_out = true; break; }
    if (r < 0) { if (errno == EINTR) continue; break; }
    if (n + 2048 > cap) { cap *= 2; b = realloc(b, cap); }
    ssize_t k = recv(fd, b + n, cap - n - 1, 0);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) break;
    n += (size_t)k;
  }
  b[n] = 0;
  *len = n;
  return b;
}

static bool starts(const char *s, const char *prefix) { return !strncmp(s, prefix, strlen(prefix)); }

static void resp_free(resp_t *r) { free(r->body); r->body = NULL; }

/* parse a whole response held in buf (taken over) */
static void resp_parse(resp_t *r, char *buf, size_t n, bool head_req) {
  memset(r, 0, sizeof *r);
  r->status = -1;
  r->clen = -1;
  char *he = strstr(buf, "\r\n\r\n");
  if (!he || sscanf(buf, "HTTP/1.1 %d", &r->status) != 1) { r->status = -1; free(buf); r->body = calloc(1, 1); return; }
  size_t hl = (size_t)(he - buf) + 4;
  if (hl >= sizeof r->head) hl = sizeof r->head - 1;
  memcpy(r->head, buf, hl);
  r->head[hl] = 0;
  for (char *p = r->head; *p; p++) {
    if (!strncasecmp(p, "\r\naccess-control", 16)) acao_seen++;
    if (!strncasecmp(p, "\r\ncontent-length:", 17)) r->clen = strtol(p + 17, NULL, 10);
  }
  r->body_len = n - ((size_t)(he - buf) + 4);
  r->body = malloc(r->body_len + 1);
  memcpy(r->body, he + 4, r->body_len);
  r->body[r->body_len] = 0;
  free(buf);
  (void)head_req;
}

/* the value of a response header (first one), "" when absent */
static const char *hdr(const resp_t *r, const char *name, char *out, size_t cap) {
  size_t nl = strlen(name);
  out[0] = 0;
  for (const char *p = strstr(r->head, "\r\n"); p && p[2]; p = strstr(p + 2, "\r\n")) {
    if (!strncasecmp(p + 2, name, nl) && p[2 + nl] == ':') {
      const char *v = p + 3 + nl, *e = strstr(v, "\r\n");
      while (*v == ' ') v++;
      size_t k = (size_t)(e - v);
      if (k >= cap) k = cap - 1;
      memcpy(out, v, k);
      out[k] = 0;
      break;
    }
  }
  return out;
}

/* send raw bytes, read the answer */
static void raw(const char *req, size_t n, resp_t *r) {
  int fd = connect_to(PORT, 0);
  size_t len = 0;
  if (fd < 0 || !send_all(fd, req, n)) { memset(r, 0, sizeof *r); r->status = -2; r->body = calloc(1, 1); if (fd >= 0) close(fd); return; }
  char *buf = read_all(fd, &len, 3000, NULL);
  close(fd);
  resp_parse(r, buf, len, starts(req, "HEAD "));
}

static void raws(const char *req, resp_t *r) { raw(req, strlen(req), r); }

/* a request with the right Host; extra headers end with \r\n; body NULL = none (no Content-Length) */
static void rq(const char *method, const char *path, const char *extra, const char *body, resp_t *r) {
  char buf[16384];
  int n = snprintf(buf, sizeof buf, "%s %s HTTP/1.1\r\nHost: %s\r\n%s", method, path, HOSTHDR, extra ? extra : "");
  if (body) n += snprintf(buf + n, sizeof buf - (size_t)n, "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
  else n += snprintf(buf + n, sizeof buf - (size_t)n, "\r\n");
  raw(buf, (size_t)n, r);
}

#define JSON_HDR "Content-Type: application/json\r\n"
static void post_json(const char *path, const char *body, resp_t *r) { rq("POST", path, JSON_HDR, body, r); }

/* the common headers of every answer */
static void check_common(const resp_t *r) {
  char v[256];
  CHECK_STR(hdr(r, "Cache-Control", v, sizeof v), "no-store");
  CHECK_STR(hdr(r, "Connection", v, sizeof v), "close");
  CHECK_STR(hdr(r, "X-Content-Type-Options", v, sizeof v), "nosniff");
}

/* status + JSON {"ok":false,"error":<msg>} */
static void check_err(const resp_t *r, int status, const char *msg) {
  char want[1024];
  CHECK_INT(r->status, status);
  snprintf(want, sizeof want, "{\"ok\":false,\"error\":\"%s\"}", msg);
  CHECK_STR(r->body, want);
  char v[128];
  CHECK_STR(hdr(r, "Content-Type", v, sizeof v), "application/json; charset=utf-8");
  CHECK_INT(r->clen, (long)r->body_len);
  check_common(r);
}

/* ================================================================ tests */
static void test_page(void) {
  resp_t r;
  char v[512];
  rq("GET", "/", NULL, NULL, &r);
  CHECK_INT(r.status, 200);
  CHECK_INT(r.body_len, WEBPAGE_LEN);
  CHECK(r.body_len == WEBPAGE_LEN && !memcmp(r.body, WEBPAGE, WEBPAGE_LEN));
  CHECK_STR(hdr(&r, "Content-Type", v, sizeof v), "text/html; charset=utf-8");
  CHECK_INT(r.clen, (long)WEBPAGE_LEN);
  CHECK(strstr(hdr(&r, "Content-Security-Policy", v, sizeof v), "default-src 'none'") != NULL);
  CHECK(strstr(v, "connect-src 'self'") != NULL);
  CHECK_STR(hdr(&r, "X-Frame-Options", v, sizeof v), "DENY");
  check_common(&r);
  resp_free(&r);

  rq("GET", "/index.html", NULL, NULL, &r);
  CHECK_INT(r.status, 200);
  CHECK_INT(r.body_len, WEBPAGE_LEN);
  resp_free(&r);
  rq("GET", "/?x=1&y", NULL, NULL, &r);           /* a query does not change the path */
  CHECK_INT(r.status, 200);
  resp_free(&r);

  rq("HEAD", "/", NULL, NULL, &r);
  CHECK_INT(r.status, 200);
  CHECK_INT(r.body_len, 0);
  CHECK_INT(r.clen, (long)WEBPAGE_LEN);
  check_common(&r);
  resp_free(&r);
  rq("HEAD", "/index.html", NULL, NULL, &r);
  CHECK_INT(r.status, 200);
  CHECK_INT(r.body_len, 0);
  resp_free(&r);
  rq("HEAD", "/api/status", NULL, NULL, &r);      /* HEAD wherever GET is */
  CHECK_INT(r.status, 200);
  CHECK_INT(r.body_len, 0);
  CHECK(r.clen > 100);
  resp_free(&r);

  rq("GET", "/nope", NULL, NULL, &r);
  CHECK_INT(r.status, 404);
  CHECK_STR(hdr(&r, "Content-Type", v, sizeof v), "text/plain; charset=utf-8");
  CHECK_STR(r.body, "not found\n");
  check_common(&r);
  resp_free(&r);
  rq("GET", "/api/nope", NULL, NULL, &r);
  check_err(&r, 404, "not found");
  resp_free(&r);
  rq("GET", "/api", NULL, NULL, &r);
  CHECK_INT(r.status, 404);
  resp_free(&r);
  rq("GET", "/API/STATUS", NULL, NULL, &r);       /* paths are case-sensitive */
  CHECK_INT(r.status, 404);
  resp_free(&r);
  rq("GET", "/api/status/", NULL, NULL, &r);
  check_err(&r, 404, "not found");
  resp_free(&r);
}

static void test_methods(void) {
  resp_t r;
  char v[128];
  static const struct { const char *m, *path, *allow; } T[] = {
    { "POST", "/", "GET, HEAD" }, { "PUT", "/index.html", "GET, HEAD" }, { "DELETE", "/api/status", "GET, HEAD" },
    { "POST", "/api/status", "GET, HEAD" }, { "PUT", "/api/config", "GET, HEAD, POST" },
    { "GET", "/api/start", "POST" }, { "HEAD", "/api/stop", "POST" }, { "GET", "/api/restart", "POST" },
    { "POST", "/api/log", "GET, HEAD" }, { "OPTIONS", "/api/config", "GET, HEAD, POST" },
    { "get", "/", "GET, HEAD" }, { "PATCH", "/api/config", "GET, HEAD, POST" },
  };
  for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
    rq(T[i].m, T[i].path, JSON_HDR, NULL, &r);
    CHECK_INT(r.status, 405);
    CHECK_STR(hdr(&r, "Allow", v, sizeof v), T[i].allow);
    if (strcmp(T[i].m, "HEAD")) CHECK_STR(r.body, "{\"ok\":false,\"error\":\"method not allowed\"}");
    else CHECK_INT(r.body_len, 0);
    check_common(&r);
    resp_free(&r);
  }
  /* a CORS preflight gets nothing that would allow the request */
  rq("OPTIONS", "/api/config", "Origin: http://127.0.0.1:1\r\nAccess-Control-Request-Method: POST\r\n"
     "Access-Control-Request-Headers: content-type\r\n", NULL, &r);
  CHECK(r.status == 403 || r.status == 405);
  resp_free(&r);
}

static void test_host(void) {
  resp_t r;
  char req[512];
  static const char *const OK[] = {
    "localhost", "LOCALHOST", "LocalHost:11000", "127.0.0.1", "127.0.0.1:80", "127.0.0.1:65535", "[::1]",
    "[::1]:11000", "192.168.1.5", "192.168.1.5:11000", "10.0.0.1:1", "0.0.0.0", "255.255.255.255:8080",
  };
  static const char *const BAD[] = {
    "evil.example", "evil.example:11000", "localhost.evil.example", "evil.localhost", "localhost.",
    "127.0.0.1.nip.io", "127.0.0.1:", "127.0.0.1:99999", "127.0.0.1:1x", "127.0.0.1:-1", "256.1.1.1",
    "1.2.3", "1.2.3.4.5", "01.2.3.4", "1.2.3.04", "1..2.3", "1.2.3.", ".1.2.3", "[::2]", "[::1]x", "[::1]:",
    "[::1]:99999", "[0:0:0:0:0:0:0:1]", "[::ffff:127.0.0.1]", "0x7f.0.0.1", "2130706433", "", "local host",
    "localhost:11000:1", "127.0.0.1 :80",
  };
  for (size_t i = 0; i < sizeof OK / sizeof OK[0]; i++) {
    snprintf(req, sizeof req, "GET /api/status HTTP/1.1\r\nHost: %s\r\n\r\n", OK[i]);
    raws(req, &r);
    if (r.status != 200) fprintf(stderr, "  host %s\n", OK[i]);
    CHECK_INT(r.status, 200);
    resp_free(&r);
  }
  for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
    snprintf(req, sizeof req, "GET /api/status HTTP/1.1\r\nHost: %s\r\n\r\n", BAD[i]);
    raws(req, &r);
    if (r.status != 403) fprintf(stderr, "  host %s\n", BAD[i]);
    check_err(&r, 403, "host not allowed");
    resp_free(&r);
  }
  /* the page and POSTs are checked too, before anything else. The page asked for by a host name (a person
   * who opened http://raspberrypi.local:11000/) is told, in HTML, to open it by its IP address; the name
   * they used is not repeated (hostile text) */
  char v[200], self[64];
  snprintf(self, sizeof self, "http://127.0.0.1:%u/", (unsigned)PORT);
  static const char *const PAGES[] = { "/", "/index.html" };
  for (int i = 0; i < 2; i++) {
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: raspberrypi.local:%u\r\n\r\n", PAGES[i], (unsigned)PORT);
    raws(req, &r);
    CHECK_INT(r.status, 403);
    CHECK_STR(hdr(&r, "Content-Type", v, sizeof v), "text/html; charset=utf-8");
    CHECK(starts(hdr(&r, "Content-Security-Policy", v, sizeof v), "default-src 'none'"));
    CHECK(strstr(r.body, "Open this page by its IP address") != NULL);
    CHECK(strstr(r.body, self) != NULL);
    CHECK(strstr(r.body, "raspberrypi") == NULL);
    CHECK_INT((long)r.body_len, r.clen);
    resp_free(&r);
  }
  raws("HEAD / HTTP/1.1\r\nHost: <script>evil.example\r\n\r\n", &r);
  CHECK_INT(r.status, 403);
  CHECK_INT((long)r.body_len, 0);
  resp_free(&r);
  raws("GET /api/status HTTP/1.1\r\nHost: raspberrypi.local\r\n\r\n", &r);
  check_err(&r, 403, "host not allowed");                /* the API stays JSON */
  resp_free(&r);
  /* the HTML is for a person opening the page (GET, HEAD); anything else to it stays JSON */
  raws("POST / HTTP/1.1\r\nHost: raspberrypi.local\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}", &r);
  check_err(&r, 403, "host not allowed");
  resp_free(&r);
  raws("DELETE /index.html HTTP/1.1\r\nHost: raspberrypi.local\r\n\r\n", &r);
  check_err(&r, 403, "host not allowed");
  resp_free(&r);
  int before = geti(&n_stop);
  raws("POST /api/stop HTTP/1.1\r\nHost: evil.example\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}", &r);
  check_err(&r, 403, "host not allowed");
  CHECK_INT(geti(&n_stop), before);
  resp_free(&r);
  /* missing (HTTP/1.1: malformed; HTTP/1.0: not allowed), duplicated */
  raws("GET /api/status HTTP/1.1\r\n\r\n", &r);
  check_err(&r, 400, "the Host header is missing");
  resp_free(&r);
  raws("GET /api/status HTTP/1.0\r\n\r\n", &r);
  check_err(&r, 403, "host not allowed");
  resp_free(&r);
  raws("GET /api/status HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", &r);
  CHECK_INT(r.status, 200);
  resp_free(&r);
  raws("GET /api/status HTTP/1.1\r\nHost: localhost\r\nHost: localhost\r\n\r\n", &r);
  check_err(&r, 400, "more than one Host header");
  resp_free(&r);
  raws("GET /api/status HTTP/1.1\r\nhOsT:   localhost  \r\n\r\n", &r);   /* case and spaces */
  CHECK_INT(r.status, 200);
  resp_free(&r);
}

static void test_origin(void) {
  resp_t r;
  char req[512];
  struct { const char *host, *origin; int ok; } T[] = {
    { "localhost:11000", "http://localhost:11000", 1 }, { "LOCALHOST:11000", "http://localhost:11000", 1 },
    { "127.0.0.1", "http://127.0.0.1", 1 }, { "127.0.0.1:80", "http://127.0.0.1", 1 },
    { "127.0.0.1", "http://127.0.0.1:80", 1 }, { "[::1]:5", "http://[::1]:5", 1 }, { "localhost", "null", 1 },
    { "localhost", "HTTP://localhost", 1 },
    { "localhost:11000", "http://evil.example", 0 }, { "localhost:11000", "http://localhost:11001", 0 },
    { "localhost:11000", "https://localhost:11000", 0 }, { "localhost:11000", "http://127.0.0.1:11000", 0 },
    { "localhost:11000", "http://localhost:11000/", 0 }, { "localhost:11000", "http://localhost:11000/x", 0 },
    { "localhost", "http://localhost.", 0 }, { "localhost", "", 0 }, { "localhost", "http://", 0 },
    { "localhost", "file://", 0 }, { "localhost", "http://user@localhost", 0 }, { "localhost", "Null", 0 },
    { "localhost:8080", "http://localhost:8080:80", 0 },
  };
  for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
    snprintf(req, sizeof req, "GET /api/status HTTP/1.1\r\nHost: %s\r\nOrigin: %s\r\n\r\n", T[i].host, T[i].origin);
    raws(req, &r);
    if ((r.status == 200) != T[i].ok) fprintf(stderr, "  host %s origin %s -> %d\n", T[i].host, T[i].origin, r.status);
    if (T[i].ok) CHECK_INT(r.status, 200);
    else check_err(&r, 403, "origin not allowed");
    resp_free(&r);
  }
  /* two Origin headers */
  raws("GET /api/status HTTP/1.1\r\nHost: localhost\r\nOrigin: http://localhost\r\nOrigin: http://localhost\r\n\r\n", &r);
  check_err(&r, 403, "origin not allowed");
  resp_free(&r);
  /* a cross-site POST changes nothing */
  int before = geti(&n_start);
  snprintf(req, sizeof req, "POST /api/start HTTP/1.1\r\nHost: %s\r\nOrigin: http://evil.example\r\n"
           "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}", HOSTHDR);
  raws(req, &r);
  check_err(&r, 403, "origin not allowed");
  CHECK_INT(geti(&n_start), before);
  resp_free(&r);
  snprintf(req, sizeof req, "POST /api/start HTTP/1.1\r\nHost: %s\r\nOrigin: http://%s\r\n"
           "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}", HOSTHDR, HOSTHDR);
  raws(req, &r);
  CHECK_INT(r.status, 200);
  CHECK_INT(geti(&n_start), before + 1);
  resp_free(&r);
}

static void test_ctype(void) {
  resp_t r;
  static const char *const OK[] = {
    "Content-Type: application/json\r\n", "Content-Type: application/json; charset=utf-8\r\n",
    "Content-Type: Application/JSON\r\n", "Content-Type: application/json;charset=UTF-8\r\n",
    "Content-Type:application/json \r\n", "Content-Type: application/json ; x=y\r\n",
  };
  static const char *const BAD[] = {
    "", "Content-Type: text/plain\r\n", "Content-Type: application/x-www-form-urlencoded\r\n",
    "Content-Type: multipart/form-data; boundary=x\r\n", "Content-Type: application/jsonp\r\n",
    "Content-Type: application/json, text/plain\r\n", "Content-Type: text/plain; application/json\r\n",
    "Content-Type: \r\n", "Content-Type: application/json\r\nContent-Type: application/json\r\n",
    "Content-Type: application/ json\r\n",
  };
  for (size_t i = 0; i < sizeof OK / sizeof OK[0]; i++) {
    int before = geti(&n_restart);
    rq("POST", "/api/restart", OK[i], "{}", &r);
    CHECK_INT(r.status, 200);
    CHECK_STR(r.body, "{\"ok\":true}");
    CHECK_INT(geti(&n_restart), before + 1);
    resp_free(&r);
  }
  for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
    int before = geti(&n_restart), sets = geti(&n_set);
    rq("POST", "/api/restart", BAD[i], "{}", &r);
    check_err(&r, 415, "use Content-Type: application/json");
    CHECK_INT(geti(&n_restart), before);
    resp_free(&r);
    rq("POST", "/api/config", BAD[i], "{\"channels\":8}", &r);
    check_err(&r, 415, "use Content-Type: application/json");
    CHECK_INT(geti(&n_set), sets);
    resp_free(&r);
  }
  /* GETs need no Content-Type */
  rq("GET", "/api/config", NULL, NULL, &r);
  CHECK_INT(r.status, 200);
  resp_free(&r);
}

static void test_parsing(void) {
  resp_t r;
  char req[16384];
  raws("GET /api/status HTTP/1.1\nHost: localhost\n\n", &r);              /* bare LF line ends */
  CHECK_INT(r.status, 200);
  resp_free(&r);
  raws("\r\n\r\nGET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n", &r); /* empty lines before it */
  CHECK_INT(r.status, 200);
  resp_free(&r);
  static const char *const BAD[] = {
    "GARBAGE\r\n\r\n", "GET\r\n\r\n", "GET /\r\nHost: localhost\r\n\r\n", "GET / HTTP/1.1 x\r\nHost: localhost\r\n\r\n",
    "GET  / HTTP/1.1\r\nHost: localhost\r\n\r\n", "GET /a b HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET http://localhost/ HTTP/1.1\r\nHost: localhost\r\n\r\n", "OPTIONS * HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET / http/1.1\r\nHost: localhost\r\n\r\n", "GET / HTTP/1.10\r\nHost: localhost\r\n\r\n",
    "GET /#x HTTP/1.1\r\nHost: localhost\r\n\r\n", "G(T / HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET / HTTP/1.1\r\nHost : localhost\r\n\r\n", "GET / HTTP/1.1\r\nHost: localhost\r\n folded\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: localhost\r\nNoColon\r\n\r\n", "GET / HTTP/1.1\r\nHost: localhost\r\n: x\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: local\rhost\r\n\r\n", "GET / HTTP/1.1\r\nHost: localhost\r\nX: a\x01b\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: localhost\r\nX: \x7f\r\n\r\n", "GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: abc\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: -1\r\n\r\n", "GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1, 1\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length:\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
    "GET /\x80 HTTP/1.1\r\nHost: localhost\r\n\r\n", "GET / HTTP/1.1\r\nX\x01Y: 1\r\nHost: localhost\r\n\r\n",
    "VERYVERYLONGMETHOD / HTTP/1.1\r\nHost: localhost\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
    raws(BAD[i], &r);
    if (r.status != 400) fprintf(stderr, "  bad request #%zu -> %d\n", i, r.status);
    check_err(&r, 400, "bad request");
    resp_free(&r);
  }
  /* a NUL in the head */
  static const char NUL_REQ[] = "GET / HTTP/1.1\r\nHost: local\0host\r\n\r\n";
  raw(NUL_REQ, sizeof NUL_REQ - 1, &r);
  check_err(&r, 400, "bad request");
  resp_free(&r);
  raws("GET / HTTP/2.0\r\nHost: localhost\r\n\r\n", &r);
  check_err(&r, 505, "only HTTP/1.0 and HTTP/1.1 are supported");
  resp_free(&r);
  /* the same Content-Length twice is fine */
  raws("GET /api/status HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nab", &r);
  CHECK_INT(r.status, 200);
  resp_free(&r);
  /* a GET with a body: read and ignored */
  raws("GET /api/status HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello", &r);
  CHECK_INT(r.status, 200);
  resp_free(&r);

  /* head limit: exactly 8192 bytes is fine, one more is not */
  for (int extra = 0; extra <= 1; extra++) {
    const char *pre = "GET /api/status HTTP/1.1\r\nHost: localhost\r\nX-Pad: ";
    size_t pl = strlen(pre), total = 8192 + (size_t)extra, pad = total - pl - 4;
    memcpy(req, pre, pl);
    memset(req + pl, 'a', pad);
    memcpy(req + pl + pad, "\r\n\r\n", 4);
    raw(req, total, &r);
    if (extra) check_err(&r, 413, "the request head is too big (at most 8192 bytes)");
    else CHECK_INT(r.status, 200);
    resp_free(&r);
  }
  /* a head that never ends */
  memset(req, 'a', 12000);
  memcpy(req, "GET / HTTP/1.1\r\nHost: localhost\r\nX: ", 36);
  raw(req, 12000, &r);
  check_err(&r, 413, "the request head is too big (at most 8192 bytes)");
  resp_free(&r);

  /* body limits */
  {
    char body[4200];
    memset(body, ' ', sizeof body);
    memcpy(body, "{\"channels\":9}", 14);
    body[4096] = 0;
    rq("POST", "/api/config", JSON_HDR, body, &r);                     /* exactly 4096 */
    CHECK_INT(r.status, 200);
    CHECK_INT(last().channels, 9);
    resp_free(&r);
    body[4096] = ' ';
    body[4097] = 0;
    int sets = geti(&n_set);
    rq("POST", "/api/config", JSON_HDR, body, &r);                     /* 4097 */
    check_err(&r, 413, "the body is too big (at most 4096 bytes)");
    CHECK_INT(geti(&n_set), sets);
    resp_free(&r);
  }
  rq("POST", "/api/config", JSON_HDR "Content-Length: 99999999999999999999999\r\n", NULL, &r);
  check_err(&r, 413, "the body is too big (at most 4096 bytes)");
  resp_free(&r);
  rq("GET", "/api/status", "Content-Length: 5000\r\n", NULL, &r);
  check_err(&r, 413, "the body is too big (at most 4096 bytes)");
  resp_free(&r);
  /* chunked and other transfer codings are refused */
  rq("POST", "/api/config", JSON_HDR "Transfer-Encoding: chunked\r\n", NULL, &r);
  check_err(&r, 411, "send the body with a Content-Length (chunked bodies are not supported)");
  resp_free(&r);
  rq("POST", "/api/start", JSON_HDR "Transfer-Encoding: chunked\r\nContent-Length: 2\r\n", NULL, &r);
  check_err(&r, 411, "send the body with a Content-Length (chunked bodies are not supported)");
  resp_free(&r);
  /* POST /api/config needs a Content-Length; start/stop/restart don't need a body */
  rq("POST", "/api/config", JSON_HDR, NULL, &r);
  check_err(&r, 411, "send the body with a Content-Length");
  resp_free(&r);
  int before = geti(&n_stop);
  rq("POST", "/api/stop", JSON_HDR, NULL, &r);
  CHECK_INT(r.status, 200);
  CHECK_INT(geti(&n_stop), before + 1);
  resp_free(&r);

  /* pipelining: one request per connection, the rest is ignored */
  snprintf(req, sizeof req, "GET /api/log?since=999999 HTTP/1.1\r\nHost: localhost\r\n\r\n"
           "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n");
  raws(req, &r);
  CHECK_INT(r.status, 200);
  CHECK(strstr(r.body, "\"lines\":[]") != NULL);
  CHECK(strstr(r.body, "HTTP/1.1") == NULL);
  CHECK_INT(r.clen, (long)r.body_len);
  resp_free(&r);
}

static void test_status(void) {
  resp_t r;
  char v[128];
  reset_snapshot();
  rq("GET", "/api/status", NULL, NULL, &r);
  CHECK_INT(r.status, 200);
  CHECK_STR(hdr(&r, "Content-Type", v, sizeof v), "application/json; charset=utf-8");
  CHECK_STR(hdr(&r, "Access-Control-Allow-Origin", v, sizeof v), "");
  check_common(&r);
  CHECK(json_valid(r.body, r.body_len));
  char want[2048];
  snprintf(want, sizeof want,
           "{\"version\":\"%s\",\"instance\":\"%s\",\"seq\":7,\"uptime_s\":12.5,\"running\":true,\"listening\":true,\"state\":\"streaming\","
           "\"status\":\"streaming\",\"detail\":\"192.168.1.50\",\"module\":{\"ip\":\"192.168.1.50\",\"kind\":\"ELRS\","
           "\"name\":\"Unified TX\"},\"rate_hz\":249.8,\"jitter_ms\":0.42,\"frames_last_s\":250,\"nch\":16,"
           "\"raw\":[7,2007,4007,6007,8007,10007,12007,14007,16007,18007,20007,22007,24007,26007,28007,30007],"
           "\"request\":{\"interval_us\":4000,\"rate_hz\":250,\"channels\":16},"
           "\"config\":{\"module_ip\":\"\",\"port\":11000,\"http_port\":80,\"interval_us\":4000,\"channels\":16},"
           "\"counters\":{\"frames\":3120,\"rx\":3125,\"beacons\":2,\"malformed\":1,\"dropped_filter\":3,\"dropped_other\":4,"
           "\"posts\":5,\"posts_ok\":4,\"posts_failed\":1,\"restarts\":6,\"gaps\":2,\"gap_max_ms\":313},"
           "\"joystick\":{\"enabled\":true,\"ok\":true,\"name\":\"ELRS Crossfire WiFi Joystick\",\"device\":\"/dev/input/event7\","
           "\"buttons\":true,\"error\":\"\"},\"log_seq\":%d}", APP_VERSION, INST, nlog);
  CHECK_STR(r.body, want);
  resp_free(&r);

  /* not bound, fewer channels, odd numbers, the other kinds and states */
  pthread_mutex_lock(&smu);
  S.bound = false;
  S.nch = 5;
  S.rate_hz = -0.04;
  S.jitter_ms = INFINITY;
  S.state = WIFI_ST_SEARCHING;
  S.cfg.interval_us = 6667;
  S.uptime_us = 0;
  S.gap_max_us = 0;
  pthread_mutex_unlock(&smu);
  rq("GET", "/api/status", NULL, NULL, &r);
  CHECK(json_valid(r.body, r.body_len));
  CHECK(strstr(r.body, "\"module\":null,") != NULL);
  CHECK(strstr(r.body, "\"state\":\"searching\"") != NULL);
  CHECK(strstr(r.body, "\"raw\":[7,2007,4007,6007,8007,0,0,0,0,0,0,0,0,0,0,0]") != NULL);
  CHECK(strstr(r.body, "\"nch\":5,") != NULL);
  CHECK(strstr(r.body, "\"rate_hz\":0.0,") != NULL);              /* never "-0.0" */
  CHECK(strstr(r.body, "\"jitter_ms\":0.00,") != NULL);           /* inf is not JSON */
  CHECK(strstr(r.body, "\"request\":{\"interval_us\":6667,\"rate_hz\":150,\"channels\":16}") != NULL);
  CHECK(strstr(r.body, "\"uptime_s\":0.0,") != NULL);
  CHECK(strstr(r.body, "\"gap_max_ms\":0}") != NULL);
  resp_free(&r);
  static const struct { wifi_state_t st; wifi_kind_t k; const char *sn, *kn; } K[] = {
    { WIFI_ST_STOPPED, WIFI_KIND_XF, "stopped", "Crossfire" }, { WIFI_ST_ERROR, WIFI_KIND_UNKNOWN, "error", "WiFi" },
  };
  for (int i = 0; i < 2; i++) {
    char a[64], b[64];
    pthread_mutex_lock(&smu);
    S.bound = true;
    S.state = K[i].st;
    S.kind = K[i].k;
    S.nch = 200;                                 /* never more than 16 numbers */
    pthread_mutex_unlock(&smu);
    rq("GET", "/api/status", NULL, NULL, &r);
    CHECK(json_valid(r.body, r.body_len));
    snprintf(a, sizeof a, "\"state\":\"%s\"", K[i].sn);
    snprintf(b, sizeof b, "\"kind\":\"%s\"", K[i].kn);
    CHECK(strstr(r.body, a) != NULL);
    CHECK(strstr(r.body, b) != NULL);
    CHECK(strstr(r.body, "\"nch\":16,") != NULL);
    resp_free(&r);
  }

  /* hostile strings: every one must come out as valid JSON that says the same thing */
  pthread_mutex_lock(&smu);
  snprintf(S.module_name, sizeof S.module_name, "a\"b\\c\x01</script>&\xff");
  snprintf(S.status, sizeof S.status, "line1\nline2\r\t\b\f");
  snprintf(S.detail, sizeof S.detail, "\x7f\xe2\x80\xa8\xe2\x80\xa9 \xc3\xa9 \xf0\x9f\x98\x80 \xc0\xaf \xed\xa0\x80 \xe2\x82");
  memset(S.joy_name, 'N', sizeof S.joy_name);    /* no NUL at all: never read past the array */
  snprintf(S.joy_error, sizeof S.joy_error, "/dev/uinput: permission denied\nrun: sudo modprobe uinput");
  memset(S.source, '9', sizeof S.source);
  pthread_mutex_unlock(&smu);
  rq("GET", "/api/status", NULL, NULL, &r);
  CHECK_INT(r.status, 200);
  CHECK(json_valid(r.body, r.body_len));
  CHECK(strstr(r.body, "\"name\":\"a\\\"b\\\\c\\u0001\\u003c/script\\u003e\\u0026\\ufffd\"") != NULL);
  CHECK(strstr(r.body, "\"status\":\"line1\\nline2\\r\\t\\u0008\\u000c\"") != NULL);
  CHECK(strstr(r.body, "\"detail\":\"\\u007f\\u2028\\u2029 \xc3\xa9 \xf0\x9f\x98\x80 \\ufffd\\ufffd \\ufffd\\ufffd\\ufffd \\ufffd\\ufffd\"") != NULL);
  {
    char want[128];
    snprintf(want, sizeof want, "\"name\":\"%.*s\"", (int)sizeof S.joy_name, "NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN");
    CHECK(strstr(r.body, want) != NULL);
    snprintf(want, sizeof want, "\"ip\":\"%.*s\",", (int)sizeof S.source, "99999999999999999999999999999999");
    CHECK(strstr(r.body, want) != NULL);
  }
  CHECK(strstr(r.body, "\"error\":\"/dev/uinput: permission denied\\nrun: sudo modprobe uinput\"") != NULL);
  CHECK(strstr(r.body, "<") == NULL);
  resp_free(&r);
  reset_snapshot();
}

static void test_config_get(void) {
  resp_t r;
  reset_snapshot();
  pthread_mutex_lock(&smu);
  snprintf(S.cfg.module_ip, sizeof S.cfg.module_ip, "10.0.0.7");
  S.cfg.interval_us = 2500;
  S.cfg.channels = 8;
  pthread_mutex_unlock(&smu);
  rq("GET", "/api/config", NULL, NULL, &r);
  CHECK_INT(r.status, 200);
  CHECK(json_valid(r.body, r.body_len));
  CHECK_STR(r.body,
            "{\"config\":{\"module_ip\":\"10.0.0.7\",\"port\":11000,\"http_port\":80,\"interval_us\":2500,\"channels\":8},"
            "\"defaults\":{\"module_ip\":\"\",\"port\":11000,\"http_port\":80,\"interval_us\":4000,\"channels\":16},"
            "\"supported\":{\"interval_us\":{\"min\":1000,\"max\":100000,\"presets\":["
            "{\"interval_us\":1000,\"rate_hz\":1000},{\"interval_us\":2000,\"rate_hz\":500},"
            "{\"interval_us\":3000,\"rate_hz\":333},{\"interval_us\":4000,\"rate_hz\":250},"
            "{\"interval_us\":5000,\"rate_hz\":200},{\"interval_us\":6667,\"rate_hz\":150},"
            "{\"interval_us\":10000,\"rate_hz\":100},{\"interval_us\":20000,\"rate_hz\":50}]},"
            "\"channels\":{\"min\":4,\"max\":16},\"port\":{\"min\":1,\"max\":65535},\"http_port\":{\"min\":1,\"max\":65535}},"
            "\"crossfire\":{\"channels\":16,\"rate_hz\":90,\"note\":\"Crossfire ignores the rate and channel count\"}}");
  resp_free(&r);
  reset_snapshot();
}

/* POST /api/config: status, the error text ("" = ok) and whether eng_set_config was called */
static void cfg_case(const char *body, int status, const char *error, int line) {
  resp_t r;
  int sets = geti(&n_set);
  post_json("/api/config", body, &r);
  bool bad = r.status != status;
  if (status == 200) {
    bad |= !starts(r.body, "{\"ok\":true,\"config\":{");
  } else {
    char want[1024];
    snprintf(want, sizeof want, "{\"ok\":false,\"error\":\"%s\"}", error);
    bad |= strcmp(r.body, want) != 0;
    bad |= geti(&n_set) != sets;                       /* nothing changes on any error */
  }
  bad |= !json_valid(r.body, r.body_len);
  t_count++;
  if (bad) {
    t_fail++;
    fprintf(stderr, "%s:%d: POST /api/config %s -> %d %s (expected %d %s)\n", __FILE__, line, body, r.status, r.body,
            status, error);
  }
  resp_free(&r);
}
#define CFG(body, status, error) cfg_case(body, status, error, __LINE__)

static void test_config_post(void) {
  resp_t r;
  reset_snapshot();
  /* a partial update keeps the rest */
  int sets = geti(&n_set);
  post_json("/api/config", "{\"channels\":8}", &r);
  CHECK_INT(r.status, 200);
  CHECK_STR(r.body, "{\"ok\":true,\"config\":{\"module_ip\":\"\",\"port\":11000,\"http_port\":80,\"interval_us\":4000,"
            "\"channels\":8},\"warning\":\"\"}");
  CHECK_INT(geti(&n_set), sets + 1);
  CHECK_INT(last().channels, 8);
  CHECK_INT(last().interval_us, 4000);
  CHECK_INT(last().port, 11000);
  CHECK_INT(getfields(), ENG_F_CHANNELS);       /* only what the body named: a CLI change meanwhile stays */
  resp_free(&r);
  /* the answer is the config in effect (as the engine has it), not the body's text */
  post_json("/api/config", "{\"module_ip\":\"\\r\\n127.0.0.2\\n\"}", &r);
  CHECK_INT(r.status, 200);
  CHECK(strstr(r.body, "\"module_ip\":\"127.0.0.2\",") != NULL);
  CHECK_STR(last().module_ip, "127.0.0.2");
  CHECK_INT(getfields(), ENG_F_MODULE_IP);
  resp_free(&r);
  post_json("/api/config", "{\"module_ip\":\" \\n \"}", &r);
  CHECK_INT(r.status, 200);
  CHECK(strstr(r.body, "\"module_ip\":\"\",") != NULL);
  resp_free(&r);
  post_json("/api/config", "{\"interval_us\":2000,\"module_ip\":\" \\t10.0.0.5 \",\"port\":11001,\"http_port\":8080}", &r);
  CHECK_INT(r.status, 200);
  CHECK_INT(getfields(), ENG_F_INTERVAL | ENG_F_MODULE_IP | ENG_F_PORT | ENG_F_HTTP_PORT);
  CHECK_STR(r.body, "{\"ok\":true,\"config\":{\"module_ip\":\"10.0.0.5\",\"port\":11001,\"http_port\":8080,\"interval_us\":2000,"
            "\"channels\":8},\"warning\":\"\"}");
  CHECK_STR(last().module_ip, "10.0.0.5");
  CHECK_INT(last().channels, 8);                /* from the previous POST, through the snapshot */
  resp_free(&r);
  post_json("/api/config", "{\"module_ip\":\"\"}", &r);    /* back to discovery */
  CHECK_INT(r.status, 200);
  CHECK_STR(last().module_ip, "");
  resp_free(&r);
  post_json("/api/config", "{\"module_ip\":\"   \"}", &r);
  CHECK_INT(r.status, 200);
  CHECK_STR(last().module_ip, "");
  resp_free(&r);
  /* nothing named: nothing set, the current config back */
  sets = geti(&n_set);
  post_json("/api/config", " { } ", &r);
  CHECK_INT(r.status, 200);
  CHECK_INT(geti(&n_set), sets);
  CHECK(strstr(r.body, "\"channels\":8") != NULL);
  resp_free(&r);
  /* the save failed: still ok, with the warning */
  set_warn("cannot save the settings: read-only file system \"/x\"");
  post_json("/api/config", "{\"channels\":10}", &r);
  CHECK_INT(r.status, 200);
  CHECK(strstr(r.body, "\"warning\":\"cannot save the settings: read-only file system \\\"/x\\\"\"}") != NULL);
  CHECK_INT(last().channels, 10);
  resp_free(&r);
  set_warn("");

  /* escapes in keys and values */
  CFG("{\"chan\\u006eels\":12}", 200, "");
  CHECK_INT(last().channels, 12);
  CFG("{\"module_ip\":\"10.0.0.\\u0035\"}", 200, "");
  CHECK_STR(last().module_ip, "10.0.0.5");
  CFG(" \n\t{ \"channels\" : 16 , \"interval_us\"\r:\n3000 }\r\n ", 200, "");
  CHECK_INT(last().channels, 16);
  CHECK_INT(last().interval_us, 3000);
  CFG("{\"interval_us\":100000}", 200, "");
  CFG("{\"interval_us\":1000}", 200, "");
  CFG("{\"channels\":4}", 200, "");
  CFG("{\"port\":65535,\"http_port\":1}", 200, "");
  CFG("{\"port\":11000,\"http_port\":80,\"interval_us\":4000,\"channels\":16,\"module_ip\":\"\"}", 200, "");

  /* values the engine refuses */
  CFG("{\"channels\":3}", 400, "channels: 3 is not supported - use 4 to 16");
  CFG("{\"channels\":17}", 400, "channels: 17 is not supported - use 4 to 16");
  CFG("{\"channels\":-0}", 400, "channels: 0 is not supported - use 4 to 16");
  CFG("{\"interval_us\":999}", 400, "interval_us: 999 is not supported - use 1000 to 100000");
  CFG("{\"interval_us\":100001}", 400, "interval_us: 100001 is not supported - use 1000 to 100000");
  CFG("{\"port\":0}", 400, "port: 0 is not supported - use 1 to 65535");
  CFG("{\"http_port\":0}", 400, "http_port: 0 is not supported - use 1 to 65535");
  CFG("{\"module_ip\":\"10.0.0\"}", 400, "module_ip: \\\"10.0.0\\\" is not an IPv4 address - use a.b.c.d, or leave it empty "
      "to find the module by its broadcast");
  CFG("{\"module_ip\":\"<b>\\n\"}", 400, "module_ip: \\\"\\u003cb\\u003e\\n\\\" is not an IPv4 address - use a.b.c.d, or leave "
      "it empty to find the module by its broadcast");
  CFG("{\"channels\":8,\"port\":0}", 400, "port: 0 is not supported - use 1 to 65535");     /* all or nothing */
  /* values the config cannot hold */
  CFG("{\"port\":70000}", 400, "port: 70000 is not supported - use 1 to 65535");
  CFG("{\"port\":-1}", 400, "port: -1 is not supported - use 1 to 65535");
  CFG("{\"http_port\":65536}", 400, "http_port: 65536 is not supported - use 1 to 65535");
  CFG("{\"port\":99999999999999999999999}", 400, "port: 99999999999999999999999 is not supported - use 1 to 65535");
  CFG("{\"channels\":-99999999999999999999999}", 400, "channels: -99999999999999999999999 is not supported - use 4 to 16");
  CFG("{\"interval_us\":5000000000}", 400, "interval_us: 5000000000 is not supported - use 1000 to 100000");
  CFG("{\"interval_us\":1234567890123456789012345678901234567890123456789012345678901234567890}", 400,
      "interval_us: 12345678901234567890123456789012345678901234... is not supported - use 1000 to 100000");
  {
    char body[200], want[300];
    snprintf(body, sizeof body, "{\"module_ip\":\"%070d\"}", 1);
    snprintf(want, sizeof want, "module_ip: \\\"%044d...\\\" is not an IPv4 address - use a.b.c.d, or leave it empty "
             "to find the module by its broadcast", 0);
    CFG(body, 400, want);
  }
  CFG("{\"module_ip\":\"1.2.3.4\\u0000\"}", 400, "module_ip: \\\"1.2.3.4?\\\" is not an IPv4 address - use a.b.c.d, or "
      "leave it empty to find the module by its broadcast");
  /* wrong types */
  CFG("{\"channels\":\"16\"}", 400, "channels must be a number");
  CFG("{\"channels\":null}", 400, "channels must be a number");
  CFG("{\"channels\":true}", 400, "channels must be a number");
  CFG("{\"channels\":[16]}", 400, "channels must be a number");
  CFG("{\"channels\":{\"a\":1}}", 400, "channels must be a number");
  CFG("{\"channels\":tru}", 400, "channels must be a number");
  CFG("{\"module_ip\":-1}", 400, "module_ip must be a string");
  CFG("{\"module_ip\":{}}", 400, "module_ip must be a string");
  CFG("{\"port\":16.0}", 400, "port must be a whole number");
  CFG("{\"interval_us\":4e3}", 400, "interval_us must be a whole number");
  CFG("{\"interval_us\":4000E-0}", 400, "interval_us must be a whole number");
  CFG("{\"http_port\":-0.5}", 400, "http_port must be a whole number");
  CFG("{\"module_ip\":5}", 400, "module_ip must be a string");
  CFG("{\"module_ip\":null}", 400, "module_ip must be a string");
  CFG("{\"module_ip\":[\"1.2.3.4\"]}", 400, "module_ip must be a string");
  /* keys */
  CFG("{\"foo\":1}", 400, "unknown key \\\"foo\\\"");
  CFG("{\"\":1}", 400, "unknown key \\\"\\\"");
  CFG("{\"Channels\":8}", 400, "unknown key \\\"Channels\\\"");
  CFG("{\"channels\":8,\"foo\":{\"x\":[1,2]}}", 400, "unknown key \\\"foo\\\"");
  CFG("{\"<script>\\\"\\u0000\":1}", 400, "unknown key \\\"\\u003cscript\\u003e\\\"?\\\"");
  CFG("{\"channels\":8,\"channels\":9}", 400, "channels is given twice");
  CFG("{\"module_ip\":\"\",\"module_ip\":\"\"}", 400, "module_ip is given twice");
  {
    char body[4000];
    memset(body, 'k', sizeof body);
    memcpy(body, "{\"", 2);
    memcpy(body + 3000, "\":1}", 5);
    char want[200];
    snprintf(want, sizeof want, "unknown key \\\"%.44s...\\\"", body + 2);
    CFG(body, 400, want);
  }
  /* not an object */
  CFG("", 400, "the body must be a JSON object");
  CFG("   ", 400, "the body must be a JSON object");
  CFG("[1]", 400, "the body must be a JSON object");
  CFG("16", 400, "the body must be a JSON object");
  CFG("\"channels\"", 400, "the body must be a JSON object");
  CFG("null", 400, "the body must be a JSON object");
  CFG("\xef\xbb\xbf{\"channels\":8}", 400, "the body must be a JSON object");     /* a BOM is not JSON */
  /* not JSON */
  static const char *const NOT_JSON[] = {
    "{", "{\"channels\":8", "{\"channels\":8,", "{\"channels\":8,}", "{\"channels\" 8}", "{\"channels\":8}}",
    "{\"channels\":8} x", "{\"channels\":8}{}", "{'channels':8}", "{channels:8}", "{\"channels\":016}",
    "{\"channels\":8 9}", "{\"channels\":-}", "{\"channels\":1.}", "{\"channels\":1e}", "{\"channels\":.5}",
    "{\"module_ip\":\"abc}", "{\"module_ip\":\"a\x01\"}", "{\"module_ip\":\"\\x41\"}", "{\"module_ip\":\"\\u12\"}",
    "{\"module_ip\":\"\\ud800\"}", "{\"module_ip\":\"\\udc00\"}", "{\"module_ip\":\"\\ud800\\u0041\"}",
    "{\"module_ip\":\"\xff\"}", "{\"module_ip\":\"\xc0\xaf\"}", "{\"module_ip\":\"\xed\xa0\x80\"}",
    "{\"chan\xe2\x82\":8}", "{\"channels\":8,,\"port\":1}", "{,}", "{\"channels\"::8}", "{\"channels\":8;}",
    "{\"channels\":+5}", "{\"channels\":.5}", "{\"module_ip\":'a'}", "{\"module_ip\":}", "{\"channels\":}",
    "{\"module_ip\":\"\\", "{\"channels\":0x10}", "{\"channels\":1_0}",
  };
  for (size_t i = 0; i < sizeof NOT_JSON / sizeof NOT_JSON[0]; i++) cfg_case(NOT_JSON[i], 400, "the body is not valid JSON", __LINE__);
  /* a valid surrogate pair and UTF-8 are fine as JSON (the engine then says it is no IPv4 address) */
  CFG("{\"module_ip\":\"\\ud83d\\ude00\xc3\xa9\"}", 400, "module_ip: \\\"\xf0\x9f\x98\x80\xc3\xa9\\\" is not an IPv4 address - "
      "use a.b.c.d, or leave it empty to find the module by its broadcast");
  /* a NUL byte in the body is not JSON */
  {
    static const char B[] = "{\"channels\":8}";
    char req[512];
    int n = snprintf(req, sizeof req, "POST /api/config HTTP/1.1\r\nHost: %s\r\n" JSON_HDR "Content-Length: 15\r\n\r\n", HOSTHDR);
    memcpy(req + n, B, sizeof B);                  /* includes the NUL */
    sets = geti(&n_set);
    raw(req, (size_t)n + 15, &r);
    check_err(&r, 400, "the body is not valid JSON");
    CHECK_INT(geti(&n_set), sets);
    resp_free(&r);
  }
  reset_snapshot();
}

static void test_actions(void) {
  resp_t r;
  int a = geti(&n_start), b = geti(&n_stop), c = geti(&n_restart);
  post_json("/api/start", "{}", &r);
  CHECK_INT(r.status, 200);
  CHECK_STR(r.body, "{\"ok\":true}");
  resp_free(&r);
  post_json("/api/stop", "{}", &r);
  CHECK_INT(r.status, 200);
  CHECK_STR(r.body, "{\"ok\":true}");
  resp_free(&r);
  post_json("/api/restart", "not json at all", &r);         /* the body is ignored */
  CHECK_INT(r.status, 200);
  CHECK_STR(r.body, "{\"ok\":true}");
  check_common(&r);
  resp_free(&r);
  CHECK_INT(geti(&n_start), a + 1);
  CHECK_INT(geti(&n_stop), b + 1);
  CHECK_INT(geti(&n_restart), c + 1);
}

static void test_log(void) {
  resp_t r;
  char want[1024];
  /* 450 lines (main): the newest 200 */
  CHECK_INT(nlog, 450);
  rq("GET", "/api/log", NULL, NULL, &r);
  CHECK_INT(r.status, 200);
  CHECK(json_valid(r.body, r.body_len));
  CHECK(starts(r.body, "{\"seq\":450,\"lines\":[{\"seq\":251,\"time_ms\":1729590000250,\"text\":\"boot line 251\"},"));
  CHECK(strstr(r.body, "{\"seq\":450,\"time_ms\":1729590000449,\"text\":\"line 450 \\u003c") != NULL);
  int count = 0;
  for (const char *p = r.body; (p = strstr(p, "\"time_ms\"")); p++) count++;
  CHECK_INT(count, 200);
  CHECK(strstr(r.body, "<") == NULL);
  resp_free(&r);
  rq("GET", "/api/log?since=0", NULL, NULL, &r);
  CHECK(starts(r.body, "{\"seq\":450,\"lines\":[{\"seq\":251,"));
  resp_free(&r);
  rq("GET", "/api/log?since=440", NULL, NULL, &r);
  CHECK(starts(r.body, "{\"seq\":450,\"lines\":[{\"seq\":441,"));
  resp_free(&r);
  rq("GET", "/api/log?x=1&since=449&y", NULL, NULL, &r);
  CHECK(starts(r.body, "{\"seq\":450,\"lines\":[{\"seq\":450,"));
  resp_free(&r);
  /* nothing new: lines [], seq = since */
  rq("GET", "/api/log?since=450", NULL, NULL, &r);
  snprintf(want, sizeof want, "{\"seq\":450,\"lines\":[],\"instance\":\"%s\"}", INST);
  CHECK_STR(r.body, want);
  resp_free(&r);
  /* since beyond the newest (the program restarted): seq goes back, so the page starts over */
  rq("GET", "/api/log?since=4294967295", NULL, NULL, &r);
  CHECK_STR(r.body, want);
  resp_free(&r);
  /* hostile lines */
  log_raw("plain line");
  log_raw("quote \" backslash \\ tab \t <b>bold</b> & \xff \x01 \xe2\x80\xa8");
  rq("GET", "/api/log?since=450", NULL, NULL, &r);
  CHECK_INT(r.status, 200);
  CHECK(json_valid(r.body, r.body_len));
  snprintf(want, sizeof want, "{\"seq\":452,\"lines\":[{\"seq\":451,\"time_ms\":1729590000450,\"text\":\"plain line\"},"
           "{\"seq\":452,\"time_ms\":1729590000451,\"text\":\"quote \\\" backslash \\\\ tab \\t \\u003cb\\u003ebold\\u003c/b\\u003e "
           "\\u0026 \\ufffd \\u0001 \\u2028\"}],\"instance\":\"%s\"}", INST);
  CHECK_STR(r.body, want);
  resp_free(&r);
  static const char *const BAD[] = {
    "/api/log?since=", "/api/log?since=abc", "/api/log?since=-1", "/api/log?since=4294967296", "/api/log?since=1.5",
    "/api/log?since=+1", "/api/log?since=99999999999", "/api/log?since=1&since=x", "/api/log?since=1%30",
  };
  for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
    rq("GET", BAD[i], NULL, NULL, &r);
    check_err(&r, 400, "since must be a number");
    resp_free(&r);
  }
}

/* ---------------------------------------------------------------- slow, partial and many clients */
static void test_slow_writer(void) {
  /* the request one byte at a time: still answered */
  char req[256];
  int n = snprintf(req, sizeof req, "GET /api/status HTTP/1.1\r\nHost: %s\r\n\r\n", HOSTHDR);
  int fd = connect_to(PORT, 0);
  CHECK(fd >= 0);
  for (int i = 0; i < n; i++) {
    CHECK(send_all(fd, req + i, 1));
    struct timespec ts = { 0, 3000000 };
    nanosleep(&ts, NULL);
  }
  size_t len;
  resp_t r;
  {
    char *rb_ = read_all(fd, &len, 3000, NULL);
    resp_parse(&r, rb_, len, false);
  }
  close(fd);
  CHECK_INT(r.status, 200);
  CHECK(json_valid(r.body, r.body_len));
  resp_free(&r);
  /* a POST whose body comes in pieces, the last one after the head */
  fd = connect_to(PORT, 0);
  n = snprintf(req, sizeof req, "POST /api/config HTTP/1.1\r\nHost: %s\r\n" JSON_HDR "Content-Length: 15\r\n\r\n{\"chan", HOSTHDR);
  CHECK(send_all(fd, req, (size_t)n));
  struct timespec ts = { 0, 50000000 };
  nanosleep(&ts, NULL);
  CHECK(send_all(fd, "nels\":", 6));
  nanosleep(&ts, NULL);
  CHECK(send_all(fd, "11}", 3));
  {
    char *rb_ = read_all(fd, &len, 3000, NULL);
    resp_parse(&r, rb_, len, false);
  }
  close(fd);
  CHECK_INT(r.status, 200);
  CHECK_INT(last().channels, 11);
  resp_free(&r);
  /* the client half-closes after its request: still answered */
  fd = connect_to(PORT, 0);
  n = snprintf(req, sizeof req, "GET /api/config HTTP/1.1\r\nHost: %s\r\n\r\n", HOSTHDR);
  CHECK(send_all(fd, req, (size_t)n));
  shutdown(fd, SHUT_WR);
  {
    char *rb_ = read_all(fd, &len, 3000, NULL);
    resp_parse(&r, rb_, len, false);
  }
  close(fd);
  CHECK_INT(r.status, 200);
  resp_free(&r);
  /* a client that leaves halfway: nothing breaks */
  for (int i = 0; i < 20; i++) {
    fd = connect_to(PORT, 0);
    send_all(fd, "GET /api/status HTTP/1.1\r\nHo", 28);
    close(fd);
    fd = connect_to(PORT, 0);
    n = snprintf(req, sizeof req, "POST /api/config HTTP/1.1\r\nHost: %s\r\n" JSON_HDR "Content-Length: 99\r\n\r\n{", HOSTHDR);
    send_all(fd, req, (size_t)n);
    struct linger lg = { 1, 0 };                  /* and one that resets */
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(fd);
  }
  resp_t r2;
  rq("GET", "/api/status", NULL, NULL, &r2);
  CHECK_INT(r2.status, 200);
  resp_free(&r2);
}

/* a reader that is slow (small window, reads in bites) gets everything; meanwhile others are served */
static void test_slow_reader(void) {
  char req[256];
  int n = snprintf(req, sizeof req, "GET /api/log HTTP/1.1\r\nHost: %s\r\n\r\n", HOSTHDR);
  int fd = connect_to(PORT, 2048);
  CHECK(fd >= 0);
  CHECK(send_all(fd, req, (size_t)n));
  struct timespec ts = { 0, 300000000 };
  nanosleep(&ts, NULL);                           /* not reading: the server must not wait for us */
  int64_t t0 = mono_ms();
  resp_t r;
  rq("GET", "/api/status", NULL, NULL, &r);
  CHECK_INT(r.status, 200);
  CHECK(mono_ms() - t0 < 1000);
  resp_free(&r);
  size_t cap = 1 << 20, len = 0;
  char *buf = malloc(cap);
  for (;;) {
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, 3000) <= 0) break;
    ssize_t k = recv(fd, buf + len, len + 1000 < cap ? 1000 : cap - len - 1, 0);
    if (k <= 0) break;
    len += (size_t)k;
    struct timespec t1 = { 0, 200000 };
    nanosleep(&t1, NULL);
  }
  buf[len] = 0;
  close(fd);
  resp_parse(&r, buf, len, false);
  CHECK_INT(r.status, 200);
  CHECK(r.body_len > 100000);
  CHECK_INT(r.clen, (long)r.body_len);
  CHECK(json_valid(r.body, r.body_len));
  resp_free(&r);
}

/* concurrent clients: every request answered */
static int conc_bad;
static void *conc_worker(void *arg) {
  for (int i = 0; i < 25; i++) {
    resp_t r;
    if (i % 3 == 2) post_json("/api/restart", "{}", &r);
    else rq("GET", i % 3 ? "/api/log?since=440" : "/api/status", NULL, NULL, &r);
    if (r.status != 200 || !json_valid(r.body, r.body_len)) __atomic_add_fetch(&conc_bad, 1, __ATOMIC_RELAXED);
    resp_free(&r);
  }
  return arg;
}

static void test_concurrent(void) {
  pthread_t th[48];
  int64_t t0 = mono_ms();
  for (int i = 0; i < 48; i++) pthread_create(&th[i], NULL, conc_worker, NULL);
  for (int i = 0; i < 48; i++) pthread_join(th[i], NULL);
  CHECK_INT(conc_bad, 0);
  printf("  1200 requests from 48 threads in %lld ms\n", (long long)(mono_ms() - t0));
}

/* idle and stuck clients are dropped after 5 s (runs on its own thread while the rest goes on) */
typedef struct {
  int64_t idle_ms, partial_ms, body_ms;          /* when each was closed / answered */
  int partial_status, body_status, idle_bytes;
  int64_t stuck_ms;                              /* the stuck reader: when its data ended */
  size_t stuck_len;
  long stuck_clen;
} timing_t;
static timing_t TM;

static void *timing_worker(void *arg) {
  char req[256];
  int64_t t0 = mono_ms();
  int idle = connect_to(PORT, 0);
  int partial = connect_to(PORT, 0);
  int body = connect_to(PORT, 0);
  int stuck = connect_to(PORT, 2048);
  send_all(partial, "GET /api/status HTTP/1.1\r\nHost: loc", 35);
  int n = snprintf(req, sizeof req, "POST /api/config HTTP/1.1\r\nHost: %s\r\n" JSON_HDR "Content-Length: 10\r\n\r\n{\"c", HOSTHDR);
  send_all(body, req, (size_t)n);
  n = snprintf(req, sizeof req, "GET /api/log HTTP/1.1\r\nHost: %s\r\n\r\n", HOSTHDR);
  send_all(stuck, req, (size_t)n);
  struct pollfd p[3] = { { idle, POLLIN, 0 }, { partial, POLLIN, 0 }, { body, POLLIN, 0 } };
  int64_t *when[3] = { &TM.idle_ms, &TM.partial_ms, &TM.body_ms };
  int left = 3;
  while (left && mono_ms() - t0 < 9000) {
    if (poll(p, 3, 200) <= 0) continue;
    for (int i = 0; i < 3; i++) {
      if (p[i].fd < 0 || !p[i].revents) continue;
      *when[i] = mono_ms() - t0;
      size_t len;
      char *b = read_all(p[i].fd, &len, 2000, NULL);
      if (i == 0) { TM.idle_bytes = (int)len; free(b); }
      else {
        resp_t r;
        resp_parse(&r, b, len, false);
        if (i == 1) TM.partial_status = r.status;
        else TM.body_status = r.status;
        resp_free(&r);
      }
      close(p[i].fd);
      p[i].fd = -1;
      left--;
    }
  }
  /* the stuck reader: wait well past the write timeout, then read what is left */
  while (mono_ms() - t0 < 7000) {
    struct timespec ts = { 0, 100000000 };
    nanosleep(&ts, NULL);
  }
  size_t len;
  bool to;
  char *b = read_all(stuck, &len, 1500, &to);
  TM.stuck_len = len;
  TM.stuck_ms = to ? -1 : mono_ms() - t0;
  resp_t r;
  resp_parse(&r, b, len, false);
  TM.stuck_clen = r.clen;
  resp_free(&r);
  close(stuck);
  return arg;
}

static void check_timing(void) {
  printf("  idle closed at %lld ms (%d bytes), partial head answered %d at %lld ms, partial body %d at %lld ms\n",
         (long long)TM.idle_ms, TM.idle_bytes, TM.partial_status, (long long)TM.partial_ms, TM.body_status,
         (long long)TM.body_ms);
  printf("  stuck reader: got %zu bytes of a %ld byte body, %s\n", TM.stuck_len, TM.stuck_clen,
         TM.stuck_ms < 0 ? "connection still open (BAD)" : "connection closed");
  CHECK(TM.idle_ms >= 4800 && TM.idle_ms <= 6500);
  CHECK_INT(TM.idle_bytes, 0);                  /* nothing was asked: closed without an answer */
  CHECK(TM.partial_ms >= 4800 && TM.partial_ms <= 6500);
  CHECK_INT(TM.partial_status, 408);
  CHECK(TM.body_ms >= 4800 && TM.body_ms <= 6500);
  CHECK_INT(TM.body_status, 408);
  CHECK(TM.stuck_ms >= 0);                       /* the server let go of it */
  CHECK(TM.stuck_clen > 200000 && TM.stuck_len < (size_t)TM.stuck_clen);   /* before the end */
}

/* 32 connections at most; the 33rd waits in the backlog until one ends */
static void test_slots(void) {
  int fds[32];
  for (int i = 0; i < 32; i++) { fds[i] = connect_to(PORT, 0); CHECK(fds[i] >= 0); }
  struct timespec ts = { 0, 100000000 };
  nanosleep(&ts, NULL);
  char req[256];
  int n = snprintf(req, sizeof req, "GET /api/status HTTP/1.1\r\nHost: %s\r\n\r\n", HOSTHDR);
  int x = connect_to(PORT, 0);                   /* the kernel completes the handshake: it is in the backlog */
  CHECK(x >= 0);
  CHECK(send_all(x, req, (size_t)n));
  struct pollfd p = { x, POLLIN, 0 };
  CHECK_INT(poll(&p, 1, 400), 0);                /* not served while every slot is taken */
  close(fds[0]);                                 /* one ends: the waiting one gets its slot */
  int64_t t0 = mono_ms();
  size_t len;
  resp_t r;
  {
    char *rb_ = read_all(x, &len, 3000, NULL);
    resp_parse(&r, rb_, len, false);
  }
  close(x);
  CHECK_INT(r.status, 200);
  CHECK(mono_ms() - t0 < 1000);
  resp_free(&r);
  for (int i = 1; i < 32; i++) close(fds[i]);
  nanosleep(&ts, NULL);
  rq("GET", "/api/status", NULL, NULL, &r);
  CHECK_INT(r.status, 200);
  resp_free(&r);
}

static void test_start_stop(void) {
  char err[160], url[64];
  /* the port in use */
  web_t *w2 = web_start(&ENG, "127.0.0.1", PORT, err, sizeof err);
  CHECK(w2 == NULL);
  char want[160];
  snprintf(want, sizeof want, "cannot listen on 127.0.0.1:%u: address already in use", (unsigned)PORT);
  CHECK_STR(err, want);
  w2 = web_start(&ENG, "localhost", 0, err, sizeof err);
  CHECK(w2 == NULL);
  CHECK_STR(err, "cannot listen on localhost:0: not an IPv4 address");
  w2 = web_start(&ENG, "999.1.1.1", 0, err, sizeof err);
  CHECK(w2 == NULL);
  w2 = web_start(&ENG, NULL, 0, err, sizeof err);
  CHECK(w2 == NULL);
  w2 = web_start(&ENG, "10.255.255.254", 0, err, sizeof err);   /* not an address of this machine */
  CHECK(w2 == NULL);
  CHECK(strstr(err, "cannot listen on 10.255.255.254:0: ") == err);
  printf("  %s\n", err);
  web_stop(NULL);
  web_url(NULL, url, sizeof url);
  CHECK_STR(url, "");

  /* every network: the URL names an address of this machine, and it serves on loopback too */
  w2 = web_start(&ENG, "0.0.0.0", 0, err, sizeof err);
  CHECK(w2 != NULL);
  if (w2) {
    char list[256], ip[16] = "127.0.0.1";
    web_url(w2, url, sizeof url);
    if (wifi_net_local_ipv4(list, sizeof list) > 0) sscanf(list, "%15[0-9.]", ip);
    unsigned port2 = 0;
    char got[16] = "";
    CHECK_INT(sscanf(url, "http://%15[0-9.]:%u/", got, &port2), 2);
    CHECK_STR(got, ip);
    CHECK(port2 > 0 && port2 != PORT);
    printf("  0.0.0.0 -> %s\n", url);
    uint16_t keep = PORT;
    PORT = (uint16_t)port2;
    resp_t r;
    raws("GET /api/status HTTP/1.1\r\nHost: 192.168.77.1:1\r\n\r\n", &r);
    CHECK_INT(r.status, 200);
    resp_free(&r);
    /* stop with connections open: returns at once, the clients see the end */
    int idle = connect_to(PORT, 0), part = connect_to(PORT, 0);
    send_all(part, "GET / HTTP/1.1\r\n", 16);
    struct timespec ts = { 0, 50000000 };
    nanosleep(&ts, NULL);
    int64_t t0 = mono_ms();
    web_stop(w2);
    CHECK(mono_ms() - t0 < 500);
    size_t len;
    bool to;
    free(read_all(idle, &len, 1000, &to));
    CHECK(!to);
    free(read_all(part, &len, 1000, &to));
    CHECK(!to);
    close(idle);
    close(part);
    CHECK(connect_to(PORT, 0) < 0);               /* nobody listens any more */
    PORT = keep;
  }
  /* start, stop, start again on the same port at once (SO_REUSEADDR, TIME_WAIT) */
  w2 = web_start(&ENG, "127.0.0.1", 0, err, sizeof err);
  CHECK(w2 != NULL);
  if (w2) {
    unsigned p2 = 0;
    web_url(w2, url, sizeof url);
    sscanf(url, "http://127.0.0.1:%u/", &p2);
    uint16_t keep = PORT;
    PORT = (uint16_t)p2;
    resp_t r;
    rq("GET", "/api/status", NULL, NULL, &r);
    CHECK_INT(r.status, 200);
    resp_free(&r);
    web_stop(w2);
    w2 = web_start(&ENG, "127.0.0.1", (uint16_t)p2, err, sizeof err);
    CHECK(w2 != NULL);
    if (!w2) printf("  restart: %s\n", err);
    web_stop(w2);
    PORT = keep;
  }
}

int main(void) {
  char err[160], url[64];
  signal(SIGPIPE, SIG_DFL);                      /* the server must not need SIGPIPE ignored */
  reset_snapshot();
  web_t *w = web_start(&ENG, "127.0.0.1", 0, err, sizeof err);
  CHECK(w != NULL);
  if (!w) { fprintf(stderr, "web_start: %s\n", err); T_DONE(); }
  web_url(w, url, sizeof url);
  unsigned p = 0;
  CHECK_INT(sscanf(url, "http://127.0.0.1:%u/", &p), 1);
  PORT = (uint16_t)p;
  snprintf(HOSTHDR, sizeof HOSTHDR, "127.0.0.1:%u", p);
  printf("  serving on %s\n", url);
  {
    /* this run's instance: the same in every status and log answer */
    resp_t r;
    rq("GET", "/api/status", NULL, NULL, &r);
    const char *q = strstr(r.body, "\"instance\":\"");
    CHECK(q != NULL);
    if (q) snprintf(INST, sizeof INST, "%.*s", (int)strcspn(q + 12, "\""), q + 12);
    CHECK(strlen(INST) >= 3);
    resp_free(&r);
  }
  char small[8] = "x";
  web_url(w, small, sizeof small);               /* cut, NUL-terminated */
  CHECK_INT(strlen(small), 7);
  web_url(w, small, 0);
  CHECK_STR(small, "http://");

  /* 450 log lines, most of them long and all '<' (6 bytes each in JSON): GET /api/log is ~360 KB */
  char lts[301];
  memset(lts, '<', 300);
  lts[300] = 0;
  for (int i = 0; i < 300; i++) eng_log(&ENG, "boot line %d", i + 1);
  while (nlog < 450) eng_log(&ENG, "line %d %s", nlog + 1, lts);
  reset_snapshot();

  pthread_t tt;
  pthread_create(&tt, NULL, timing_worker, NULL);
  test_page();
  test_methods();
  test_host();
  test_origin();
  test_ctype();
  test_parsing();
  test_status();
  test_config_get();
  test_config_post();
  test_actions();
  test_log();
  test_slow_writer();
  test_slow_reader();
  test_concurrent();
  pthread_join(tt, NULL);
  check_timing();
  test_slots();
  test_start_stop();
  CHECK_INT(acao_seen, 0);                       /* no Access-Control-* header, ever */
  web_stop(w);
  T_DONE();
}

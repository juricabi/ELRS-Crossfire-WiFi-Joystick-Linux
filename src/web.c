/* The web page and its JSON API (docs/WEB_API.md, web.h).
 *
 * One thread, non-blocking sockets and poll(): the listening socket, a wake pipe (web_stop) and at
 * most WEB_MAX_CONN connections. Each connection carries exactly one request (Connection: close):
 * read the head (<= WEB_HEAD_MAX) and the body (<= WEB_BODY_MAX, Content-Length only) into the
 * connection's own buffer, answer, then a lingering close (shutdown, drain until the client closes)
 * so unread request bytes never turn the answer into a reset. A request must arrive within
 * WEB_IDLE_MS of the accept, and a response that makes no progress for WEB_IDLE_MS is dropped, so a
 * slow or stuck client holds a slot for a few seconds at most. When every slot is taken, new
 * connections wait in the kernel's backlog instead of being refused.
 *
 * Every request is checked before it is routed (the table in WEB_API.md): the Host header must name
 * this machine by a loopback name or an IPv4 literal (DNS rebinding: a web site that points its own
 * name at 127.0.0.1 is still sent with its name in Host), an Origin header must name the same host,
 * and a POST must say Content-Type: application/json, which a cross-site page can only send after a
 * CORS preflight that is never answered (no Access-Control-* headers are ever sent).
 *
 * The engine is read through eng_snapshot / eng_log_read and changed through eng_update_config /
 * eng_start / eng_stop / eng_restart_stream only: all of them return at once (eng_update_config may
 * write the settings file through its save hook, on this thread). Numbers are formatted without
 * printf's %f, so a locale set by the program can never put a decimal comma into the JSON. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE               /* accept4, pipe2 */
#endif
#include "web.h"
#include "wifi_net.h"
#include "wifi_proto.h"
#include "webpage.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef APP_VERSION
#define APP_VERSION "0.0.0-dev"
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define WEB_MAX_CONN   32
#define WEB_HEAD_MAX   8192        /* request line + headers + the blank line */
#define WEB_BODY_MAX   4096
#define WEB_IDLE_MS    5000
#define WEB_LINGER_MS  1000        /* after the answer: wait this long for the client to close */
#define WEB_LOG_MAX    200         /* lines per GET /api/log */
#define WEB_BACKLOG    64
#define WEB_SNDBUF     65536       /* per connection (the kernel doubles it): bounds what a stuck client holds */

#define JSON_CT "application/json; charset=utf-8"
#define TEXT_CT "text/plain; charset=utf-8"
#define HTML_CT "text/html; charset=utf-8"
/* the page is self-contained: inline script and style, data: icon, fetches to this server only */
#define PAGE_HEADERS \
  "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; " \
  "img-src data:; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n" \
  "X-Frame-Options: DENY\r\n"

/* ================================================================ string builder (JSON bodies) */
typedef struct { char *p; size_t len, cap; bool oom; } sb_t;

static bool sb_room(sb_t *b, size_t n) {
  if (b->oom) return false;
  if (b->len + n + 1 <= b->cap) return true;
  size_t nc = b->cap ? b->cap : 1024;
  while (nc < b->len + n + 1) nc *= 2;
  char *np = realloc(b->p, nc);
  if (!np) { b->oom = true; return false; }
  b->p = np;
  b->cap = nc;
  return true;
}

static void sb_add(sb_t *b, const char *s, size_t n) {
  if (!sb_room(b, n)) return;
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = 0;
}

static void sb_puts(sb_t *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_printf(sb_t *b, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;
static void sb_printf(sb_t *b, const char *fmt, ...) {
  char tmp[128];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof tmp) { sb_add(b, tmp, (size_t)n); return; }
  if (!sb_room(b, (size_t)n)) return;
  va_start(ap, fmt);
  vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
}

/* a fixed-point number ("249.8"): integer math, so no locale can make it "249,8" */
static void sb_fixed(sb_t *b, double v, int decimals) {
  if (!isfinite(v)) v = 0;
  if (v > 1e12) v = 1e12;
  if (v < -1e12) v = -1e12;
  long long scale = 1;
  for (int i = 0; i < decimals; i++) scale *= 10;
  long long q = llround(v * (double)scale);
  const char *sign = q < 0 ? "-" : "";
  if (q < 0) q = -q;
  if (decimals) sb_printf(b, "%s%lld.%0*lld", sign, q / scale, decimals, q % scale);
  else sb_printf(b, "%s%lld", sign, q);
}

/* length of the valid UTF-8 sequence at p (RFC 3629: no overlongs, surrogates or > U+10FFFF), 0 if invalid */
static int utf8_seq(const unsigned char *p, size_t n, unsigned *cp) {
  unsigned c = p[0];
  if (c < 0x80) { *cp = c; return 1; }
  if (c >= 0xC2 && c <= 0xDF) {
    if (n < 2 || (p[1] & 0xC0) != 0x80) return 0;
    *cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
    return 2;
  }
  if (c >= 0xE0 && c <= 0xEF) {
    unsigned lo = c == 0xE0 ? 0xA0 : 0x80, hi = c == 0xED ? 0x9F : 0xBF;
    if (n < 3 || p[1] < lo || p[1] > hi || (p[2] & 0xC0) != 0x80) return 0;
    *cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    unsigned lo = c == 0xF0 ? 0x90 : 0x80, hi = c == 0xF4 ? 0x8F : 0xBF;
    if (n < 4 || p[1] < lo || p[1] > hi || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
    *cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    return 4;
  }
  return 0;
}

/* A JSON string from n bytes of anything (names from beacons, the joystick name, log lines cut in the
 * middle of a character): control characters, '<', '>', '&' (so JSON can never be read as HTML) and
 * U+2028/2029 become \u escapes, invalid UTF-8 becomes U+FFFD. */
static void sb_json_strn(sb_t *b, const char *s, size_t n) {
  const unsigned char *p = (const unsigned char *)s, *end = p + n;
  sb_add(b, "\"", 1);
  while (p < end) {
    const unsigned char *q = p;
    while (q < end && *q >= 0x20 && *q < 0x7f && *q != '"' && *q != '\\' && *q != '<' && *q != '>' && *q != '&') q++;
    if (q > p) { sb_add(b, (const char *)p, (size_t)(q - p)); p = q; continue; }
    unsigned c = *p, cp;
    switch (c) {
      case '"': sb_add(b, "\\\"", 2); p++; continue;
      case '\\': sb_add(b, "\\\\", 2); p++; continue;
      case '\n': sb_add(b, "\\n", 2); p++; continue;
      case '\r': sb_add(b, "\\r", 2); p++; continue;
      case '\t': sb_add(b, "\\t", 2); p++; continue;
      default: break;
    }
    if (c < 0x80) { sb_printf(b, "\\u%04x", c); p++; continue; }
    int k = utf8_seq(p, (size_t)(end - p), &cp);
    if (!k) { sb_add(b, "\\ufffd", 6); p++; continue; }
    if (cp == 0x2028 || cp == 0x2029) sb_printf(b, "\\u%04x", cp);
    else sb_add(b, (const char *)p, (size_t)k);
    p += k;
  }
  sb_add(b, "\"", 1);
}

/* a NUL-terminated string in a fixed array of cap bytes (never read past it) */
static void sb_json_arr(sb_t *b, const char *s, size_t cap) { sb_json_strn(b, s, strnlen(s, cap)); }
static void sb_json_str(sb_t *b, const char *s) { sb_json_strn(b, s, strlen(s)); }
#define SB_JSON_FIELD(b, arr) sb_json_arr((b), (arr), sizeof(arr))

/* ================================================================ server state */
enum { C_FREE, C_READ, C_WRITE, C_LINGER };

typedef struct { const char *p; size_t n; } span_t;

typedef struct {
  char method[16];
  span_t path, query;
  int minor;                   /* HTTP/1.<minor> */
  int nhost, norigin, nctype;  /* how many of each header */
  span_t host, origin, ctype;
  bool has_len;
  size_t clen;                 /* SIZE_MAX: too big to say */
  bool te;                     /* any Transfer-Encoding */
} req_t;

typedef struct {
  int fd;
  int st;
  int64_t deadline_us;
  bool head_done;              /* the head is parsed, a body of rq.clen bytes is awaited */
  bool is_head;                /* HEAD: the answer has no body */
  int route;
  size_t skip;                 /* CR/LF before the request line (allowed, RFC 9112 2.2) */
  size_t scanned;              /* where the search for the end of the head resumes */
  size_t head_len;             /* bytes of the head incl. the blank line (and skip) */
  size_t inlen;
  req_t rq;
  /* the answer: hdr, then body (static, or heap when owned) */
  char hdr[768];
  size_t hdr_len;
  const char *body;
  size_t body_len;
  char *heap;
  size_t sent;
  char in[WEB_HEAD_MAX + WEB_BODY_MAX];
} conn_t;

struct web {
  engine_t *e;
  pthread_t th;
  int lfd;
  int wake[2];
  int quit;                    /* set by web_stop (__atomic) */
  uint32_t ip;                 /* bound address (network order) */
  uint16_t port;               /* bound port (the real one when 0 was asked for) */
  char instance[40];           /* this run of the program (status and log): a page sees a restart by it */
  int nconn;
  int64_t accept_pause_until;
  eng_logline_t logbuf[WEB_LOG_MAX];
  conn_t conn[WEB_MAX_CONN];
};

static int64_t now_us(void) { return wifi_net_now_us(); }

/* ================================================================ request parsing */
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static bool ieq_n(const char *a, const char *b, size_t n) {   /* ASCII only: locale-proof */
  for (size_t i = 0; i < n; i++) if (lc((unsigned char)a[i]) != lc((unsigned char)b[i])) return false;
  return true;
}

static bool span_ieq(span_t s, const char *lit) {
  size_t n = strlen(lit);
  return s.n == n && ieq_n(s.p, lit, n);
}

static bool span_eq(span_t s, const char *lit) {
  size_t n = strlen(lit);
  return s.n == n && !memcmp(s.p, lit, n);
}

static bool is_digit(int c) { return c >= '0' && c <= '9'; }

static bool tchar(int ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || is_digit(ch) || (ch && strchr("!#$%&'*+-.^_`|~", ch));
}

/* Parse the head h[0..n) (it ends with the blank line): 0, or the HTTP status of the error. */
static int parse_head(const char *h, size_t n, req_t *rq) {
  memset(rq, 0, sizeof *rq);
  rq->path.p = rq->query.p = rq->host.p = rq->origin.p = rq->ctype.p = "";
  if (memchr(h, 0, n)) return 400;
  const char *end = h + n, *line = h;
  bool first = true;
  while (line < end) {
    const char *nl = memchr(line, '\n', (size_t)(end - line));
    if (!nl) return 400;
    const char *le = nl;
    if (le > line && le[-1] == '\r') le--;
    if (memchr(line, '\r', (size_t)(le - line))) return 400;    /* a bare CR */
    size_t ll = (size_t)(le - line);
    if (first) {
      first = false;
      const char *sp1 = memchr(line, ' ', ll);
      if (!sp1) return 400;
      const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(le - sp1 - 1));
      if (!sp2 || memchr(sp2 + 1, ' ', (size_t)(le - sp2 - 1))) return 400;
      size_t ml = (size_t)(sp1 - line);
      if (!ml || ml >= sizeof rq->method) return 400;
      for (size_t i = 0; i < ml; i++) if (!tchar((unsigned char)line[i])) return 400;
      memcpy(rq->method, line, ml);
      rq->method[ml] = 0;
      const char *v = sp2 + 1;
      size_t vl = (size_t)(le - v);
      if (vl == 8 && !memcmp(v, "HTTP/1.1", 8)) rq->minor = 1;
      else if (vl == 8 && !memcmp(v, "HTTP/1.0", 8)) rq->minor = 0;
      else if (vl == 8 && !memcmp(v, "HTTP/", 5) && is_digit(v[5]) && v[6] == '.' && is_digit(v[7])) return 505;
      else return 400;
      const char *t = sp1 + 1;
      size_t tl = (size_t)(sp2 - t);
      if (!tl || t[0] != '/') return 400;                       /* origin-form only */
      for (size_t i = 0; i < tl; i++) {
        unsigned char ch = (unsigned char)t[i];
        if (ch <= 0x20 || ch >= 0x7f || ch == '#') return 400;
      }
      const char *q = memchr(t, '?', tl);
      rq->path = (span_t){ t, q ? (size_t)(q - t) : tl };
      if (q) rq->query = (span_t){ q + 1, (size_t)(t + tl - q - 1) };
    } else {
      if (!ll) break;                                           /* the blank line */
      if (line[0] == ' ' || line[0] == '\t') return 400;        /* obsolete line folding */
      const char *colon = memchr(line, ':', ll);
      if (!colon || colon == line) return 400;
      for (const char *p = line; p < colon; p++) if (!tchar((unsigned char)*p)) return 400;
      span_t name = { line, (size_t)(colon - line) };
      const char *v = colon + 1, *ve = le;
      while (v < ve && (*v == ' ' || *v == '\t')) v++;
      while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
      for (const char *p = v; p < ve; p++) {
        unsigned char ch = (unsigned char)*p;
        if ((ch < 0x20 && ch != '\t') || ch == 0x7f) return 400;
      }
      span_t val = { v, (size_t)(ve - v) };
      if (span_ieq(name, "host")) {
        rq->nhost++;
        rq->host = val;
      } else if (span_ieq(name, "origin")) {
        rq->norigin++;
        rq->origin = val;
      } else if (span_ieq(name, "content-type")) {
        rq->nctype++;
        rq->ctype = val;
      } else if (span_ieq(name, "transfer-encoding")) {
        rq->te = true;
      } else if (span_ieq(name, "content-length")) {
        if (!val.n) return 400;
        size_t x = 0;
        for (size_t i = 0; i < val.n; i++) {
          if (!is_digit(val.p[i])) return 400;
          if (x <= 100000000) x = x * 10 + (size_t)(val.p[i] - '0');
          else x = SIZE_MAX;                                    /* far beyond any limit here */
        }
        if (rq->has_len && x != rq->clen) return 400;           /* two different lengths: smuggling */
        rq->has_len = true;
        rq->clen = x;
      }
    }
    line = nl + 1;
  }
  return 0;
}

/* end of the head (just after the blank line) in p[from..n), 0 when not there yet */
static size_t head_end(const char *p, size_t n, size_t from) {
  for (size_t i = from; i + 1 < n; i++) {
    if (p[i] != '\n') continue;
    if (p[i + 1] == '\n') return i + 2;
    if (p[i + 1] == '\r' && i + 2 < n && p[i + 2] == '\n') return i + 3;
  }
  return 0;
}

/* ================================================================ Host / Origin / Content-Type */
static bool port_ok(const char *p, size_t n) {
  if (!n || n > 5) return false;
  long v = 0;
  for (size_t i = 0; i < n; i++) {
    if (!is_digit(p[i])) return false;
    v = v * 10 + (p[i] - '0');
  }
  return v <= 65535;
}

/* dotted decimal, as browsers write it: four 0..255 parts without leading zeros */
static bool ipv4_ok(const char *s, size_t n) {
  int parts = 0;
  size_t i = 0;
  while (parts < 4) {
    size_t d = 0;
    int v = 0;
    while (i < n && is_digit(s[i]) && d < 4) { v = v * 10 + (s[i] - '0'); i++; d++; }
    if (!d || d > 3 || v > 255 || (d > 1 && s[i - d] == '0')) return false;
    parts++;
    if (parts == 4) break;
    if (i >= n || s[i] != '.') return false;
    i++;
  }
  return i == n;
}

/* localhost, 127.0.0.1, [::1] or an IPv4 literal, each with an optional :port */
static bool host_ok(span_t h) {
  if (!h.n) return false;
  if (h.p[0] == '[') {
    if (h.n < 5 || memcmp(h.p, "[::1]", 5)) return false;
    return h.n == 5 || (h.p[5] == ':' && port_ok(h.p + 6, h.n - 6));
  }
  const char *colon = memchr(h.p, ':', h.n);
  size_t hl = colon ? (size_t)(colon - h.p) : h.n;
  if (colon && !port_ok(colon + 1, h.n - hl - 1)) return false;
  if (hl == 9 && ieq_n(h.p, "localhost", 9)) return true;
  return ipv4_ok(h.p, hl);
}

/* host[:port] without a default ":80" (only when that is its one port: never "a:8080:80" -> "a:8080") */
static span_t hostport_norm(span_t s) {
  if (s.n <= 3 || memcmp(s.p + s.n - 3, ":80", 3)) return s;
  const char *rb = s.n > 4 ? memchr(s.p, ']', s.n - 3) : NULL;
  const char *from = rb ? rb + 1 : s.p, *to = s.p + s.n - 3;
  if (!memchr(from, ':', (size_t)(to - from))) s.n -= 3;
  return s;
}

/* "http://<the Host header>" (default port and case aside); "null" counts as no Origin */
static bool origin_ok(span_t origin, span_t host) {
  if (span_eq(origin, "null")) return true;
  if (origin.n < 7 || !ieq_n(origin.p, "http://", 7)) return false;
  span_t a = hostport_norm((span_t){ origin.p + 7, origin.n - 7 }), b = hostport_norm(host);
  return a.n && a.n == b.n && ieq_n(a.p, b.p, a.n);
}

/* application/json, parameters (; charset=utf-8) allowed */
static bool ctype_json(span_t ct) {
  size_t n = 0;
  while (n < ct.n && ct.p[n] != ';') n++;
  while (n && (ct.p[n - 1] == ' ' || ct.p[n - 1] == '\t')) n--;
  return n == 16 && ieq_n(ct.p, "application/json", 16);
}

/* ================================================================ connections */
/* abort: a reset instead of a FIN, so the kernel drops what it still holds for a client that stopped
 * reading (a plain close would keep trying to deliver it for minutes) */
static void conn_close(web_t *w, conn_t *c, bool abort) {
  if (c->fd >= 0 && abort) {
    struct linger lg = { 1, 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
  }
  if (c->fd >= 0) close(c->fd);
  free(c->heap);
  c->heap = NULL;
  c->fd = -1;
  c->st = C_FREE;
  w->nconn--;
}

static const char *reason(int code) {
  switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 505: return "HTTP Version Not Supported";
    default: return "Internal Server Error";
  }
}

static void conn_write(web_t *w, conn_t *c);

/* Queue the answer and start sending it. heap (may be NULL) is owned from here on; body may point into it. */
static void respond(web_t *w, conn_t *c, int code, const char *ctype, const char *extra,
                    const char *body, size_t blen, char *heap) {
  int n = snprintf(c->hdr, sizeof c->hdr,
                   "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                   "Connection: close\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n%s\r\n",
                   code, reason(code), ctype, blen, extra ? extra : "");
  if (n < 0 || (size_t)n >= sizeof c->hdr) n = 0;              /* cannot happen: the extras are fixed */
  free(c->heap);
  c->heap = heap;
  c->hdr_len = (size_t)n;
  c->body = c->is_head ? NULL : body;
  c->body_len = c->is_head ? 0 : blen;
  c->sent = 0;
  c->st = C_WRITE;
  c->deadline_us = now_us() + (int64_t)WEB_IDLE_MS * 1000;
  conn_write(w, c);
}

static void respond_sb(web_t *w, conn_t *c, int code, const char *extra, sb_t *b) {
  if (b->oom) {
    static const char OOM[] = "{\"ok\":false,\"error\":\"out of memory\"}";
    free(b->p);
    respond(w, c, 500, JSON_CT, NULL, OOM, sizeof OOM - 1, NULL);
    return;
  }
  respond(w, c, code, JSON_CT, extra, b->p ? b->p : "", b->len, b->p);
}

static void respond_err(web_t *w, conn_t *c, int code, const char *msg, const char *extra) {
  sb_t b = { 0 };
  sb_puts(&b, "{\"ok\":false,\"error\":");
  sb_json_str(&b, msg);
  sb_puts(&b, "}");
  respond_sb(w, c, code, extra, &b);
}

static void conn_write(web_t *w, conn_t *c) {
  size_t total = c->hdr_len + c->body_len;
  while (c->sent < total) {
    struct iovec iov[2];
    int k = 0;
    if (c->sent < c->hdr_len) {
      iov[k].iov_base = c->hdr + c->sent;
      iov[k++].iov_len = c->hdr_len - c->sent;
      if (c->body_len) { iov[k].iov_base = (void *)c->body; iov[k++].iov_len = c->body_len; }
    } else {
      iov[k].iov_base = (void *)(c->body + (c->sent - c->hdr_len));
      iov[k++].iov_len = total - c->sent;
    }
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = iov;
    m.msg_iovlen = (size_t)k;
    ssize_t r = sendmsg(c->fd, &m, MSG_NOSIGNAL);
    if (r > 0) {
      c->sent += (size_t)r;
      c->deadline_us = now_us() + (int64_t)WEB_IDLE_MS * 1000;
      continue;
    }
    if (r < 0 && errno == EINTR) continue;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;   /* POLLOUT */
    conn_close(w, c, false);                                           /* the client is gone */
    return;
  }
  /* all sent: half-close and drain until the client closes (an unread request byte would make the
   * kernel answer our close with a reset, which can destroy the answer before the client reads it) */
  free(c->heap);
  c->heap = NULL;
  c->body = NULL;
  shutdown(c->fd, SHUT_WR);
  c->st = C_LINGER;
  c->deadline_us = now_us() + (int64_t)WEB_LINGER_MS * 1000;
}

/* ================================================================ JSON in (POST /api/config) */
typedef struct { const char *p, *end; } jp_t;

static void jp_ws(jp_t *j) {
  while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool jp_hex4(jp_t *j, unsigned *out) {
  if (j->end - j->p < 4) return false;
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    int h = hexval((unsigned char)j->p[i]);
    if (h < 0) return false;
    v = v * 16 + (unsigned)h;
  }
  j->p += 4;
  *out = v;
  return true;
}

static size_t utf8_put(char *o, unsigned cp) {
  if (cp < 0x80) { o[0] = (char)cp; return 1; }
  if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* A JSON string at j->p (its opening quote), decoded (escapes, surrogate pairs) into out; the decoded
 * text is never longer than its JSON, so cap >= the input length is enough. Invalid UTF-8, raw
 * control characters, bad escapes and lone surrogates are errors. */
static bool jp_str(jp_t *j, char *out, size_t cap, size_t *len) {
  if (j->p >= j->end || *j->p != '"') return false;
  j->p++;
  size_t o = 0;
  while (j->p < j->end) {
    unsigned char ch = (unsigned char)*j->p;
    if (o + 4 > cap) return false;
    if (ch == '"') { j->p++; *len = o; return true; }
    if (ch < 0x20) return false;
    if (ch == '\\') {
      if (j->end - j->p < 2) return false;
      char e = j->p[1];
      j->p += 2;
      unsigned cp;
      switch (e) {
        case '"': case '\\': case '/': out[o++] = e; continue;
        case 'b': out[o++] = '\b'; continue;
        case 'f': out[o++] = '\f'; continue;
        case 'n': out[o++] = '\n'; continue;
        case 'r': out[o++] = '\r'; continue;
        case 't': out[o++] = '\t'; continue;
        case 'u':
          if (!jp_hex4(j, &cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            unsigned lo;
            if (j->end - j->p < 2 || j->p[0] != '\\' || j->p[1] != 'u') return false;
            j->p += 2;
            if (!jp_hex4(j, &lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
          }
          o += utf8_put(out + o, cp);
          continue;
        default:
          return false;
      }
    }
    if (ch < 0x80) { out[o++] = (char)ch; j->p++; continue; }
    unsigned cp;
    int k = utf8_seq((const unsigned char *)j->p, (size_t)(j->end - j->p), &cp);
    if (!k) return false;
    memcpy(out + o, j->p, (size_t)k);
    o += (size_t)k;
    j->p += k;
  }
  return false;                                                  /* unterminated */
}

/* A JSON number at j->p: 1 an integer (*v; *big when it has more than 15 digits), 2 one with a
 * fraction or an exponent, 0 not a JSON number. lit = its text. */
static int jp_num(jp_t *j, long long *v, bool *big, span_t *lit) {
  const char *p = j->p, *s = p, *e = j->end;
  bool neg = false;
  if (p < e && *p == '-') { neg = true; p++; }
  if (p >= e || !is_digit(*p)) return 0;
  long long x = 0;
  int digits = 0;
  if (*p == '0') {
    p++;
    if (p < e && is_digit(*p)) return 0;                        /* leading zeros are not JSON */
  } else {
    while (p < e && is_digit(*p)) {
      if (++digits <= 15) x = x * 10 + (*p - '0');
      p++;
    }
  }
  bool frac = false;
  if (p < e && *p == '.') {
    p++;
    if (p >= e || !is_digit(*p)) return 0;
    while (p < e && is_digit(*p)) p++;
    frac = true;
  }
  if (p < e && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < e && (*p == '+' || *p == '-')) p++;
    if (p >= e || !is_digit(*p)) return 0;
    while (p < e && is_digit(*p)) p++;
    frac = true;
  }
  *lit = (span_t){ s, (size_t)(p - s) };
  *big = digits > 15;
  *v = neg ? -x : x;
  j->p = p;
  return frac ? 2 : 1;
}

enum { F_IP, F_PORT, F_HTTP, F_INTERVAL, F_CHANNELS, F_N };
static const char *const FIELD[F_N] = { "module_ip", "port", "http_port", "interval_us", "channels" };

/* text for an error message: cut, and NULs (from \u0000) shown as '?' */
static void msg_text(char *out, size_t cap, const char *s, size_t n) {
  size_t max = cap > 4 ? cap - 4 : 0, o = 0;
  bool cut = n > max;
  if (cut) n = max;
  for (size_t i = 0; i < n; i++) out[o++] = s[i] ? s[i] : '?';
  if (cut) { memcpy(out + o, "...", 3); o += 3; }
  out[o] = 0;
}

/* Parse the flat object of POST /api/config onto cfg. 0, or -1 with err (nothing applied).
 * *given: a bit per field (1 << F_*) that the body named. */
static int parse_config(const char *body, size_t n, eng_config_t *cfg, unsigned *given, char *err, size_t cap) {
  static const char BAD[] = "the body is not valid JSON";
  char key[WEB_BODY_MAX + 4], sval[WEB_BODY_MAX + 4], t[48];
  jp_t j = { body, body + n };
  *given = 0;
  jp_ws(&j);
  if (j.p >= j.end || *j.p != '{') { snprintf(err, cap, "the body must be a JSON object"); return -1; }
  j.p++;
  jp_ws(&j);
  if (j.p < j.end && *j.p == '}') {
    j.p++;
  } else {
    for (;;) {
      size_t kl;
      jp_ws(&j);
      if (!jp_str(&j, key, sizeof key, &kl)) { snprintf(err, cap, "%s", BAD); return -1; }
      jp_ws(&j);
      if (j.p >= j.end || *j.p != ':') { snprintf(err, cap, "%s", BAD); return -1; }
      j.p++;
      jp_ws(&j);
      int f = -1;
      for (int i = 0; i < F_N; i++)
        if (strlen(FIELD[i]) == kl && !memcmp(FIELD[i], key, kl)) f = i;
      if (f < 0) {
        msg_text(t, sizeof t, key, kl);
        snprintf(err, cap, "unknown key \"%s\"", t);
        return -1;
      }
      if (*given & (1u << f)) { snprintf(err, cap, "%s is given twice", FIELD[f]); return -1; }
      *given |= 1u << f;
      if (j.p >= j.end) { snprintf(err, cap, "%s", BAD); return -1; }
      /* a JSON value of the wrong type is named as such; anything else is a syntax error */
      bool other_value = strchr("\"-0123456789tfn[{", *j.p) != NULL && *j.p;
      if (f == F_IP) {
        size_t sl;
        if (*j.p != '"') {
          snprintf(err, cap, "%s", other_value ? "module_ip must be a string" : BAD);
          return -1;
        }
        if (!jp_str(&j, sval, sizeof sval, &sl)) { snprintf(err, cap, "%s", BAD); return -1; }
        size_t a = 0;
        while (a < sl && (sval[a] == ' ' || sval[a] == '\t')) a++;
        while (sl > a && (sval[sl - 1] == ' ' || sval[sl - 1] == '\t')) sl--;
        if (sl - a >= sizeof cfg->module_ip || memchr(sval + a, 0, sl - a)) {
          msg_text(t, sizeof t, sval + a, sl - a);
          snprintf(err, cap, "module_ip: \"%s\" is not an IPv4 address - use a.b.c.d, or leave it empty to find "
                   "the module by its broadcast", t);
          return -1;
        }
        memcpy(cfg->module_ip, sval + a, sl - a);
        cfg->module_ip[sl - a] = 0;
      } else {
        long long v;
        bool big;
        span_t lit;
        int k = (*j.p == '-' || is_digit(*j.p)) ? jp_num(&j, &v, &big, &lit) : other_value ? -1 : 0;
        if (k < 0) { snprintf(err, cap, "%s must be a number", FIELD[f]); return -1; }
        if (k == 0) { snprintf(err, cap, "%s", BAD); return -1; }
        if (k == 2) { snprintf(err, cap, "%s must be a whole number", FIELD[f]); return -1; }
        long long lo = 1, hi = 65535;
        if (f == F_INTERVAL) { lo = WIFI_INTERVAL_MIN; hi = WIFI_INTERVAL_MAX; }
        if (f == F_CHANNELS) { lo = WIFI_MIN_CH; hi = WIFI_MAX_CH; }
        /* only what the config cannot hold is refused here; eng_update_config checks the rest */
        bool fits = f == F_PORT || f == F_HTTP ? v >= 0 && v <= 65535 : v >= -1000000000LL && v <= 1000000000LL;
        if (big || !fits) {
          msg_text(t, sizeof t, lit.p, lit.n);
          snprintf(err, cap, "%s: %s is not supported - use %lld to %lld", FIELD[f], t, lo, hi);
          return -1;
        }
        if (f == F_PORT) cfg->port = (uint16_t)v;
        else if (f == F_HTTP) cfg->http_port = (uint16_t)v;
        else if (f == F_INTERVAL) cfg->interval_us = (int)v;
        else cfg->channels = (int)v;
      }
      jp_ws(&j);
      if (j.p < j.end && *j.p == ',') { j.p++; continue; }
      if (j.p < j.end && *j.p == '}') { j.p++; break; }
      snprintf(err, cap, "%s", BAD);
      return -1;
    }
  }
  jp_ws(&j);
  if (j.p != j.end) { snprintf(err, cap, "%s", BAD); return -1; }
  return 0;
}

/* ================================================================ the API */
static void json_config(sb_t *b, const eng_config_t *c) {
  sb_puts(b, "{\"module_ip\":");
  SB_JSON_FIELD(b, c->module_ip);
  sb_printf(b, ",\"port\":%u,\"http_port\":%u,\"interval_us\":%d,\"channels\":%d}",
            (unsigned)c->port, (unsigned)c->http_port, c->interval_us, c->channels);
}

static void api_status(web_t *w, sb_t *b) {
  eng_snapshot_t s;
  eng_snapshot(w->e, &s);
  sb_puts(b, "{\"version\":");
  sb_json_str(b, APP_VERSION);
  sb_puts(b, ",\"instance\":");
  sb_json_str(b, w->instance);
  sb_printf(b, ",\"seq\":%u,\"uptime_s\":", (unsigned)s.seq);
  sb_fixed(b, (double)s.uptime_us / 1e6, 1);
  sb_printf(b, ",\"running\":%s,\"listening\":%s,\"state\":", s.running ? "true" : "false", s.listening ? "true" : "false");
  sb_json_str(b, wifi_state_name(s.state));
  sb_puts(b, ",\"status\":");
  SB_JSON_FIELD(b, s.status);
  sb_puts(b, ",\"detail\":");
  SB_JSON_FIELD(b, s.detail);
  sb_puts(b, ",\"module\":");
  if (s.bound) {
    sb_puts(b, "{\"ip\":");
    SB_JSON_FIELD(b, s.source);
    sb_puts(b, ",\"kind\":");
    sb_json_str(b, wifi_kind_name(s.kind));
    sb_puts(b, ",\"name\":");
    SB_JSON_FIELD(b, s.module_name);
    sb_puts(b, "}");
  } else {
    sb_puts(b, "null");
  }
  sb_puts(b, ",\"rate_hz\":");
  sb_fixed(b, s.rate_hz, 1);
  sb_puts(b, ",\"jitter_ms\":");
  sb_fixed(b, s.jitter_ms, 2);
  int nch = s.nch > WIFI_MAX_CH ? WIFI_MAX_CH : s.nch;
  sb_printf(b, ",\"frames_last_s\":%d,\"nch\":%d,\"raw\":[", s.frames_last_s, nch);
  for (int i = 0; i < WIFI_MAX_CH; i++) sb_printf(b, "%s%u", i ? "," : "", i < nch ? (unsigned)s.raw[i] : 0u);
  sb_printf(b, "],\"request\":{\"interval_us\":%d,\"rate_hz\":%d,\"channels\":%d},\"config\":",
            s.cfg.interval_us, wifi_interval_hz(s.cfg.interval_us), s.cfg.channels);
  json_config(b, &s.cfg);
  sb_printf(b, ",\"counters\":{\"frames\":%u,\"rx\":%u,\"beacons\":%u,\"malformed\":%u,\"dropped_filter\":%u,"
            "\"dropped_other\":%u,\"posts\":%u,\"posts_ok\":%u,\"posts_failed\":%u,\"restarts\":%u,\"gaps\":%u,"
            "\"gap_max_ms\":",
            (unsigned)s.frames, (unsigned)s.rx, (unsigned)s.beacons, (unsigned)s.malformed, (unsigned)s.dropped_filter,
            (unsigned)s.dropped_other, (unsigned)s.posts, (unsigned)s.posts_ok, (unsigned)s.posts_failed,
            (unsigned)s.restarts, (unsigned)s.gaps);
  sb_fixed(b, (double)s.gap_max_us / 1000.0, 0);
  sb_printf(b, "},\"joystick\":{\"enabled\":%s,\"ok\":%s,\"name\":", s.joy_enabled ? "true" : "false", s.joy_ok ? "true" : "false");
  SB_JSON_FIELD(b, s.joy_name);
  sb_puts(b, ",\"device\":");
  SB_JSON_FIELD(b, s.joy_device);
  sb_printf(b, ",\"buttons\":%s,\"error\":", s.joy_buttons ? "true" : "false");
  SB_JSON_FIELD(b, s.joy_error);
  sb_printf(b, "},\"log_seq\":%u}", (unsigned)s.log_seq);
}

static void api_config_get(web_t *w, sb_t *b) {
  eng_snapshot_t s;
  eng_config_t d;
  static const int presets[WIFI_N_INTERVAL_PRESETS] = WIFI_INTERVAL_PRESETS;
  eng_snapshot(w->e, &s);
  eng_config_default(&d);
  sb_puts(b, "{\"config\":");
  json_config(b, &s.cfg);
  sb_puts(b, ",\"defaults\":");
  json_config(b, &d);
  sb_printf(b, ",\"supported\":{\"interval_us\":{\"min\":%d,\"max\":%d,\"presets\":[", WIFI_INTERVAL_MIN, WIFI_INTERVAL_MAX);
  for (int i = 0; i < WIFI_N_INTERVAL_PRESETS; i++)
    sb_printf(b, "%s{\"interval_us\":%d,\"rate_hz\":%d}", i ? "," : "", presets[i], wifi_interval_hz(presets[i]));
  sb_printf(b, "]},\"channels\":{\"min\":%d,\"max\":%d},\"port\":{\"min\":1,\"max\":65535},"
            "\"http_port\":{\"min\":1,\"max\":65535}},\"crossfire\":{\"channels\":%d,\"rate_hz\":90,"
            "\"note\":\"Crossfire ignores the rate and channel count\"}}", WIFI_MIN_CH, WIFI_MAX_CH, WIFI_XF_CHANNELS);
}

/* 0 = answered ok in b, else the error status with b holding the error JSON */
static int api_config_post(web_t *w, const char *body, size_t n, sb_t *b) {
  eng_snapshot_t s;
  char err[320];
  unsigned given;
  eng_snapshot(w->e, &s);
  eng_config_t cfg = s.cfg, now = s.cfg;
  err[0] = 0;
  if (parse_config(body, n, &cfg, &given, err, sizeof err) != 0) goto fail;
  if (given) {
    /* only the fields the body names (merged by the engine, so a CLI change meanwhile stays) */
    static const unsigned BIT[F_N] = { ENG_F_MODULE_IP, ENG_F_PORT, ENG_F_HTTP_PORT, ENG_F_INTERVAL, ENG_F_CHANNELS };
    unsigned fields = 0;
    for (int i = 0; i < F_N; i++) if (given & (1u << i)) fields |= BIT[i];
    err[0] = 0;
    if (eng_update_config(w->e, &cfg, fields, &now, err, sizeof err) != 0) {
      if (!err[0]) snprintf(err, sizeof err, "the settings were not accepted");
      goto fail;
    }
  }
  /* the config now asked for, as the engine has it (module_ip in its plain form), not the body's text */
  sb_puts(b, "{\"ok\":true,\"config\":");
  json_config(b, &now);
  sb_puts(b, ",\"warning\":");
  sb_json_arr(b, err, sizeof err);
  sb_puts(b, "}");
  return 0;
fail:
  sb_puts(b, "{\"ok\":false,\"error\":");
  sb_json_arr(b, err, sizeof err);
  sb_puts(b, "}");
  return 400;
}

/* since=N from the query (other parameters ignored): 0 ok, -1 not a number */
static int query_since(span_t q, uint32_t *since) {
  *since = 0;
  const char *p = q.p, *e = q.p + q.n;
  while (p < e) {
    const char *amp = memchr(p, '&', (size_t)(e - p));
    const char *pe = amp ? amp : e;
    if (pe - p >= 6 && !memcmp(p, "since=", 6)) {
      const char *v = p + 6;
      if (v == pe || pe - v > 10) return -1;
      unsigned long long x = 0;
      for (const char *d = v; d < pe; d++) {
        if (!is_digit(*d)) return -1;
        x = x * 10 + (unsigned)(*d - '0');
      }
      if (x > UINT32_MAX) return -1;
      *since = (uint32_t)x;
    }
    p = pe + (amp ? 1 : 0);
  }
  return 0;
}

static void api_log(web_t *w, uint32_t since, sb_t *b) {
  eng_snapshot_t s;
  eng_snapshot(w->e, &s);
  uint32_t from = since;
  if (s.log_seq > since && s.log_seq - since > WEB_LOG_MAX) from = s.log_seq - WEB_LOG_MAX;   /* the newest 200 */
  int n = eng_log_read(w->e, from, w->logbuf, WEB_LOG_MAX);
  if (n < 0) n = 0;
  if (n > WEB_LOG_MAX) n = WEB_LOG_MAX;
  /* no new lines: the engine's newest seq, which is below since when the program was restarted */
  uint32_t seq = n ? w->logbuf[n - 1].seq : s.log_seq;
  sb_printf(b, "{\"seq\":%u,\"lines\":[", (unsigned)seq);
  for (int i = 0; i < n; i++) {
    sb_printf(b, "%s{\"seq\":%u,\"time_ms\":%lld,\"text\":", i ? "," : "", (unsigned)w->logbuf[i].seq,
              (long long)w->logbuf[i].wall_ms);
    SB_JSON_FIELD(b, w->logbuf[i].text);
    sb_puts(b, "}");
  }
  sb_puts(b, "],\"instance\":");
  sb_json_str(b, w->instance);
  sb_puts(b, "}");
}

/* ================================================================ routing */
enum { M_GET = 1, M_HEAD = 2, M_POST = 4 };
enum { R_PAGE, R_STATUS, R_CONFIG, R_START, R_STOP, R_RESTART, R_LOG };
static const struct { const char *path; int id; unsigned methods; const char *allow; } ROUTES[] = {
  { "/", R_PAGE, M_GET | M_HEAD, "Allow: GET, HEAD\r\n" },
  { "/index.html", R_PAGE, M_GET | M_HEAD, "Allow: GET, HEAD\r\n" },
  { "/api/status", R_STATUS, M_GET | M_HEAD, "Allow: GET, HEAD\r\n" },
  { "/api/config", R_CONFIG, M_GET | M_HEAD | M_POST, "Allow: GET, HEAD, POST\r\n" },
  { "/api/start", R_START, M_POST, "Allow: POST\r\n" },
  { "/api/stop", R_STOP, M_POST, "Allow: POST\r\n" },
  { "/api/restart", R_RESTART, M_POST, "Allow: POST\r\n" },
  { "/api/log", R_LOG, M_GET | M_HEAD, "Allow: GET, HEAD\r\n" },
};
#define NROUTES (sizeof ROUTES / sizeof ROUTES[0])

static unsigned method_bit(const char *m) {
  if (!strcmp(m, "GET")) return M_GET;
  if (!strcmp(m, "HEAD")) return M_HEAD;
  if (!strcmp(m, "POST")) return M_POST;
  return 0;
}

static void dispatch(web_t *w, conn_t *c) {
  const char *body = c->in + c->head_len;
  size_t blen = c->rq.has_len ? c->rq.clen : 0;
  sb_t b = { 0 };
  int code = 200;
  switch (c->route) {
    case R_PAGE:
      respond(w, c, 200, HTML_CT, PAGE_HEADERS, (const char *)WEBPAGE, WEBPAGE_LEN, NULL);
      return;
    case R_STATUS:
      api_status(w, &b);
      break;
    case R_CONFIG:
      if (!strcmp(c->rq.method, "POST")) code = api_config_post(w, body, blen, &b);
      else api_config_get(w, &b);
      if (!code) code = 200;
      break;
    case R_START:
      eng_start(w->e);
      sb_puts(&b, "{\"ok\":true}");
      break;
    case R_STOP:
      eng_stop(w->e);
      sb_puts(&b, "{\"ok\":true}");
      break;
    case R_RESTART:
      eng_restart_stream(w->e);
      sb_puts(&b, "{\"ok\":true}");
      break;
    case R_LOG: {
      uint32_t since;
      if (query_since(c->rq.query, &since) != 0) {
        respond_err(w, c, 400, "since must be a number", NULL);
        return;
      }
      api_log(w, since, &b);
      break;
    }
    default:
      respond_err(w, c, 500, "no route", NULL);
      return;
  }
  respond_sb(w, c, code, NULL, &b);
}

/* The page asked for by a host name (http://raspberrypi.local:11000/ with --web 0.0.0.0): a person in a
 * browser, who gets told what to open instead. The name in Host is not repeated (it is the visitor's
 * text); the address is this server's own. */
static void refused_page(web_t *w, conn_t *c) {
  char url[96], here[48];
  web_url(w, url, sizeof url);
  snprintf(here, sizeof here, "http://127.0.0.1:%u/", (unsigned)w->port);
  sb_t b = { 0 };
  sb_puts(&b, "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><meta name=\"viewport\" "
              "content=\"width=device-width, initial-scale=1\"><title>Open this page by its IP address - "
              "elrs-wifi-joystick</title><body style=\"font: 16px/1.5 system-ui, sans-serif; margin: 2em; max-width: 40em\">"
              "<h1 style=\"font-size: 1.3em\">Open this page by its IP address</h1>"
              "<p>elrs-wifi-joystick answers only to an IP address or localhost, not to a host name: that keeps "
              "other web sites from reaching it through a name of theirs (DNS rebinding).</p><p>Open <a href=\"");
  sb_puts(&b, url);
  sb_puts(&b, "\">");
  sb_puts(&b, url);
  sb_puts(&b, "</a>");
  if (strcmp(url, here)) {
    sb_puts(&b, " (or ");
    sb_puts(&b, here);
    sb_puts(&b, " on that machine)");
  }
  sb_puts(&b, ". The program also prints the address when it starts (\"Web page: ...\").</p></body></html>\n");
  if (b.oom) {
    free(b.p);
    respond_err(w, c, 403, "host not allowed", NULL);
    return;
  }
  respond(w, c, 403, HTML_CT, "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'\r\n"
          "X-Frame-Options: DENY\r\n", b.p, b.len, b.p);
}

/* The head is complete: check it (the table in WEB_API.md) and answer, or wait for the body. */
static void on_head(web_t *w, conn_t *c) {
  req_t *rq = &c->rq;
  int err = parse_head(c->in + c->skip, c->head_len - c->skip, rq);
  if (err) {
    respond_err(w, c, err, err == 505 ? "only HTTP/1.0 and HTTP/1.1 are supported" : "bad request", NULL);
    return;
  }
  c->is_head = !strcmp(rq->method, "HEAD");
  if (rq->te) {
    respond_err(w, c, 411, "send the body with a Content-Length (chunked bodies are not supported)", NULL);
    return;
  }
  if (rq->has_len && rq->clen > WEB_BODY_MAX) {
    respond_err(w, c, 413, "the body is too big (at most 4096 bytes)", NULL);
    return;
  }
  if (rq->nhost > 1 || (rq->minor == 1 && !rq->nhost)) {
    respond_err(w, c, 400, rq->nhost ? "more than one Host header" : "the Host header is missing", NULL);
    return;
  }
  if (!host_ok(rq->host)) {
    unsigned m = method_bit(rq->method);
    if ((m == M_GET || m == M_HEAD) && (span_eq(rq->path, "/") || span_eq(rq->path, "/index.html"))) refused_page(w, c);
    else respond_err(w, c, 403, "host not allowed", NULL);
    return;
  }
  if (rq->norigin > 1 || (rq->norigin && !origin_ok(rq->origin, rq->host))) {
    respond_err(w, c, 403, "origin not allowed", NULL);
    return;
  }
  int r = -1;
  for (size_t i = 0; i < NROUTES; i++)
    if (span_eq(rq->path, ROUTES[i].path)) r = (int)i;
  if (r < 0) {
    if (rq->path.n >= 5 && !memcmp(rq->path.p, "/api/", 5)) {
      respond_err(w, c, 404, "not found", NULL);
    } else {
      static const char NF[] = "not found\n";
      respond(w, c, 404, TEXT_CT, NULL, NF, sizeof NF - 1, NULL);
    }
    return;
  }
  unsigned m = method_bit(rq->method);
  if (!(ROUTES[r].methods & m)) {
    respond_err(w, c, 405, "method not allowed", ROUTES[r].allow);
    return;
  }
  if (m == M_POST) {
    if (rq->nctype != 1 || !ctype_json(rq->ctype)) {
      respond_err(w, c, 415, "use Content-Type: application/json", NULL);
      return;
    }
    if (ROUTES[r].id == R_CONFIG && !rq->has_len) {
      respond_err(w, c, 411, "send the body with a Content-Length", NULL);
      return;
    }
  }
  c->route = ROUTES[r].id;
  c->head_done = true;
}

/* Read what there is and move the request on. */
static void conn_read(web_t *w, conn_t *c) {
  bool eof = false;
  for (;;) {
    size_t room = sizeof c->in - c->inlen;
    if (!room) break;
    ssize_t r = recv(c->fd, c->in + c->inlen, room, 0);
    if (r > 0) { c->inlen += (size_t)r; continue; }
    if (r == 0) { eof = true; break; }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    conn_close(w, c, false);                                     /* reset by the client */
    return;
  }
  if (!c->head_done) {
    while (c->skip < c->inlen && c->skip < WEB_HEAD_MAX && (c->in[c->skip] == '\r' || c->in[c->skip] == '\n')) c->skip++;
    size_t lim = c->inlen < WEB_HEAD_MAX ? c->inlen : WEB_HEAD_MAX;
    size_t from = c->scanned > c->skip + 2 ? c->scanned - 2 : c->skip;
    size_t he = lim > c->skip ? head_end(c->in, lim, from) : 0;
    if (!he) {
      c->scanned = lim;
      if (c->inlen >= WEB_HEAD_MAX) { respond_err(w, c, 413, "the request head is too big (at most 8192 bytes)", NULL); return; }
      if (eof) conn_close(w, c, false);
      return;
    }
    c->head_len = he;
    on_head(w, c);
    if (c->st != C_READ) return;                                 /* answered (an error) */
  }
  size_t need = c->head_len + (c->rq.has_len ? c->rq.clen : 0);
  if (c->inlen >= need) { dispatch(w, c); return; }
  if (eof) conn_close(w, c, false);                              /* the body never came */
}

static void conn_drain(web_t *w, conn_t *c) {
  char tmp[2048];
  for (;;) {
    ssize_t r = recv(c->fd, tmp, sizeof tmp, 0);
    if (r > 0) continue;
    if (r < 0 && errno == EINTR) continue;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    conn_close(w, c, false);                                     /* closed by the client (or reset) */
    return;
  }
}

static void accept_all(web_t *w) {
  while (w->nconn < WEB_MAX_CONN) {
    int fd = accept4(w->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)                 /* out of descriptors or memory: pause */
        w->accept_pause_until = now_us() + 200000;
      return;
    }
    conn_t *c = NULL;
    for (int i = 0; i < WEB_MAX_CONN && !c; i++)
      if (w->conn[i].st == C_FREE) c = &w->conn[i];
    if (!c) { close(fd); return; }                                /* cannot happen: nconn counts them */
    int sb = WEB_SNDBUF;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
    c->fd = fd;
    c->st = C_READ;
    c->deadline_us = now_us() + (int64_t)WEB_IDLE_MS * 1000;
    c->head_done = c->is_head = false;
    c->route = -1;
    c->skip = c->scanned = c->head_len = c->inlen = 0;
    c->hdr_len = c->body_len = c->sent = 0;
    c->body = NULL;
    c->heap = NULL;
    w->nconn++;
  }
}

static void sweep(web_t *w) {
  int64_t t = now_us();
  for (int i = 0; i < WEB_MAX_CONN; i++) {
    conn_t *c = &w->conn[i];
    if (c->st == C_FREE || t < c->deadline_us) continue;
    if (c->st == C_READ && c->inlen > c->skip) respond_err(w, c, 408, "the request took too long", NULL);
    else conn_close(w, c, c->st == C_WRITE);   /* nothing asked, done lingering, or a stuck reader */
  }
}

static void *web_thread(void *arg) {
  web_t *w = arg;
  struct pollfd pf[2 + WEB_MAX_CONN];
  int map[2 + WEB_MAX_CONN];
  while (!__atomic_load_n(&w->quit, __ATOMIC_ACQUIRE)) {
    int n = 0, li = -1;
    int64_t t = now_us(), next = t + 1000000;
    pf[n].fd = w->wake[0];
    pf[n].events = POLLIN;
    map[n++] = -1;
    if (w->nconn < WEB_MAX_CONN) {
      if (t >= w->accept_pause_until) {
        li = n;
        pf[n].fd = w->lfd;
        pf[n].events = POLLIN;
        map[n++] = -1;
      } else if (w->accept_pause_until < next) {
        next = w->accept_pause_until;
      }
    }
    for (int i = 0; i < WEB_MAX_CONN; i++) {
      conn_t *c = &w->conn[i];
      if (c->st == C_FREE) continue;
      pf[n].fd = c->fd;
      pf[n].events = c->st == C_WRITE ? POLLOUT : POLLIN;
      map[n++] = i;
      if (c->deadline_us < next) next = c->deadline_us;
    }
    int64_t wait = (next - t + 999) / 1000;
    int r = poll(pf, (nfds_t)n, wait < 0 ? 0 : (int)wait);
    if (r < 0) {
      if (errno != EINTR) usleep(10000);           /* cannot happen with valid descriptors: don't spin */
      continue;
    }
    if (pf[0].revents) {
      char tmp[16];
      while (read(w->wake[0], tmp, sizeof tmp) > 0) {}
    }
    if (__atomic_load_n(&w->quit, __ATOMIC_ACQUIRE)) break;
    for (int k = 0; k < n; k++) {
      if (map[k] < 0 || !pf[k].revents) continue;
      conn_t *c = &w->conn[map[k]];
      if (c->st == C_WRITE) conn_write(w, c);
      else if (c->st == C_READ) conn_read(w, c);
      else if (c->st == C_LINGER) conn_drain(w, c);
    }
    if (li >= 0 && (pf[li].revents & POLLIN)) accept_all(w);
    sweep(w);
  }
  for (int i = 0; i < WEB_MAX_CONN; i++)
    if (w->conn[i].st != C_FREE) conn_close(w, &w->conn[i], false);
  return NULL;
}

/* ================================================================ web.h */
web_t *web_start(engine_t *e, const char *addr, uint16_t port, char *err, size_t cap) {
  uint32_t ip;
  char et[64];
  if (cap) err[0] = 0;
  if (!addr || wifi_parse_ip(addr, &ip) != 1) {
    snprintf(err, cap, "cannot listen on %s:%u: not an IPv4 address", addr ? addr : "", (unsigned)port);
    return NULL;
  }
  web_t *w = calloc(1, sizeof *w);
  if (!w) { snprintf(err, cap, "cannot start the web page: out of memory"); return NULL; }
  w->e = e;
  w->ip = ip;
  w->wake[0] = w->wake[1] = -1;
  for (int i = 0; i < WEB_MAX_CONN; i++) { w->conn[i].fd = -1; w->conn[i].st = C_FREE; }
  w->lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (w->lfd < 0) goto sock_fail;
  int one = 1;
  /* a restart right after a stop must not fail on connections still in TIME_WAIT; on Linux this
   * does not let a second program listen on the same address and port */
  setsockopt(w->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = ip;
  if (bind(w->lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(w->lfd, WEB_BACKLOG) != 0) goto sock_fail;
  socklen_t al = sizeof a;
  w->port = getsockname(w->lfd, (struct sockaddr *)&a, &al) == 0 ? ntohs(a.sin_port) : port;
  {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(w->instance, sizeof w->instance, "%lx-%llx", (unsigned long)getpid(),
             (unsigned long long)ts.tv_sec * 1000000ULL + (unsigned long long)ts.tv_nsec / 1000);
  }
  if (pipe2(w->wake, O_NONBLOCK | O_CLOEXEC) != 0) goto sock_fail;
  /* signals go to the other threads (the program's handlers or sigwait) */
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  int pe = pthread_create(&w->th, NULL, web_thread, w);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  if (pe != 0) {
    snprintf(err, cap, "cannot start the web page: %s", strerror(pe));
    goto fail;
  }
  return w;
sock_fail: {
    int e2 = errno;
    wifi_net_err_text(e2, et, sizeof et);
    if (!strncmp(et, "error ", 6)) snprintf(et, sizeof et, "%s", strerror(e2));
    snprintf(err, cap, "cannot listen on %s:%u: %s", addr, (unsigned)port, et);
  }
fail:
  if (w->lfd >= 0) close(w->lfd);
  if (w->wake[0] >= 0) close(w->wake[0]);
  if (w->wake[1] >= 0) close(w->wake[1]);
  free(w);
  return NULL;
}

void web_url(const web_t *w, char *out, size_t cap) {
  if (!cap) return;
  out[0] = 0;
  if (!w) return;
  char ip[16];
  if (w->ip == htonl(INADDR_ANY)) {
    /* "192.168.1.20 (wlan0), ...": the first address, else this machine */
    char list[256];
    size_t k = 0;
    snprintf(ip, sizeof ip, "127.0.0.1");
    if (wifi_net_local_ipv4(list, sizeof list) > 0) {
      while (k < sizeof ip - 1 && list[k] && list[k] != ' ' && list[k] != ',') k++;
      if (k >= 7) { memcpy(ip, list, k); ip[k] = 0; }
    }
  } else {
    wifi_ip_str(w->ip, ip);
  }
  snprintf(out, cap, "http://%s:%u/", ip, (unsigned)w->port);
}

void web_stop(web_t *w) {
  if (!w) return;
  __atomic_store_n(&w->quit, 1, __ATOMIC_RELEASE);
  char one = 1;
  while (write(w->wake[1], &one, 1) < 0 && errno == EINTR) {}
  pthread_join(w->th, NULL);
  close(w->lfd);
  close(w->wake[0]);
  close(w->wake[1]);
  free(w);
}

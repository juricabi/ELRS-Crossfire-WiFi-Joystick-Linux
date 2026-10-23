/* Command line and settings file: see config.h. */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define CFG_MAX_FILE 65536

static const int PRESETS[WIFI_N_INTERVAL_PRESETS] = WIFI_INTERVAL_PRESETS;

void cfg_defaults(cfg_settings_t *s) {
  memset(s, 0, sizeof *s);
  eng_config_default(&s->eng);
  s->web = true;
  snprintf(s->web_addr, sizeof s->web_addr, "127.0.0.1");
  s->web_port = 11000;
  s->joystick = true;
  s->buttons = true;
}

/* ---------------------------------------------------------------- values */
static bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* trimmed copy */
static void trim_copy(const char *s, char *out, size_t cap) {
  if (!cap) return;
  if (!s) s = "";
  while (is_blank(*s)) s++;
  size_t n = strlen(s);
  while (n && is_blank(s[n - 1])) n--;
  if (n >= cap) n = cap - 1;
  memcpy(out, s, n);
  out[n] = 0;
}

/* user text inside an error message: one clean line, not too long */
static void safe_text(const char *s, char *out, size_t cap) {
  size_t k = 0;
  for (; s && *s && k + 4 < cap && k < 40; s++) out[k++] = (*s >= 0x20 && *s < 0x7f) ? *s : '?';
  if (s && *s && k + 4 < cap) { out[k++] = '.'; out[k++] = '.'; out[k++] = '.'; }
  out[k] = 0;
}

int cfg_parse_int(const char *s, long min, long max, long *out) {
  char t[32];
  if (!s || strlen(s) >= sizeof t) return -1;
  trim_copy(s, t, sizeof t);
  const char *p = t;
  bool neg = false;
  if (*p == '+' || *p == '-') neg = *p++ == '-';
  if (!*p) return -1;
  /* 64 bits whatever long is (32 on i686/armv7/armv6): 18 digits can't overflow it, and a 10-digit
   * number never wraps into range */
  long long v = 0;
  int nd = 0;
  for (; *p; p++) {
    if (*p < '0' || *p > '9') return -1;
    if (++nd > 18) return -2;              /* far out of any range (and no overflow) */
    v = v * 10 + (*p - '0');
  }
  if (neg) v = -v;
  if (v < min || v > max) return -2;
  if (out) *out = (long)v;
  return 0;
}

static int num_err(int r, const char *s, const char *use, char *err, size_t cap) {
  char t[48];
  safe_text(s, t, sizeof t);
  char tt[48];
  trim_copy(t, tt, sizeof tt);
  if (r == -1) snprintf(err, cap, "\"%s\" is not a number - use %s", tt, use);
  else snprintf(err, cap, "%s is not supported - use %s", tt, use);
  return -1;
}

int cfg_value_module_ip(const char *s, char out[64], char *err, size_t cap) {
  uint32_t ip = 0;
  int r = wifi_parse_ip(s, &ip);
  if (r == 0) { out[0] = 0; return 0; }
  if (r < 0 || ip == 0) {
    char t[48];
    safe_text(s, t, sizeof t);
    snprintf(err, cap, "\"%s\" is not an IPv4 address - use a.b.c.d, or leave it empty to find the module by its "
             "broadcast", t);
    return -1;
  }
  char ips[16];
  wifi_ip_str(ip, ips);
  snprintf(out, 64, "%s", ips);
  return 0;
}

int cfg_rate_to_interval(int hz) {
  if (hz < 10 || hz > 1000) return 0;
  for (int i = 0; i < WIFI_N_INTERVAL_PRESETS; i++)
    if (wifi_interval_hz(PRESETS[i]) == hz) return PRESETS[i];
  return (1000000 + hz / 2) / hz;
}

#define RATE_USE "1000, 500, 333, 250, 200, 150, 100 or 50 (any 10 to 1000 works)"

int cfg_value_rate(const char *s, int *interval_us, char *err, size_t cap) {
  long v;
  int r = cfg_parse_int(s, 10, 1000, &v);
  if (r) return num_err(r, s, RATE_USE, err, cap);
  *interval_us = cfg_rate_to_interval((int)v);
  return 0;
}

int cfg_value_interval(const char *s, int *interval_us, char *err, size_t cap) {
  long v;
  int r = cfg_parse_int(s, WIFI_INTERVAL_MIN, WIFI_INTERVAL_MAX, &v);
  if (r) return num_err(r, s, "1000 to 100000", err, cap);
  *interval_us = (int)v;
  return 0;
}

int cfg_value_channels(const char *s, int *channels, char *err, size_t cap) {
  long v;
  int r = cfg_parse_int(s, WIFI_MIN_CH, WIFI_MAX_CH, &v);
  if (r) return num_err(r, s, "4 to 16", err, cap);
  *channels = (int)v;
  return 0;
}

int cfg_value_port(const char *s, uint16_t *port, char *err, size_t cap) {
  long v;
  int r = cfg_parse_int(s, 1, 65535, &v);
  if (r) return num_err(r, s, "1 to 65535", err, cap);
  *port = (uint16_t)v;
  return 0;
}

int cfg_value_web(const char *s, bool *on, char addr[16], uint16_t *port, char *err, size_t cap) {
  char t[96];
  trim_copy(s, t, sizeof t);
  if (!strcasecmp(t, "off")) { *on = false; return 0; }
  char a[16] = "127.0.0.1";
  const char *ps = t;
  char *colon = strrchr(t, ':');
  bool ok = true;
  if (colon) {
    *colon = 0;
    ps = colon + 1;
    uint32_t ip = 0;
    if (!strcasecmp(t, "localhost")) snprintf(a, sizeof a, "127.0.0.1");
    else if (wifi_parse_ip(t, &ip) == 1) wifi_ip_str(ip, a);
    else ok = false;
  }
  long v = 0;
  if (!ok || cfg_parse_int(ps, 1, 65535, &v) != 0) {
    char q[48];
    safe_text(s, q, sizeof q);
    snprintf(err, cap, "\"%s\" is not a web address - use [ADDR:]PORT (127.0.0.1:11000 = this machine only, "
             "0.0.0.0:11000 = reachable from the network) or off", q);
    return -1;
  }
  *on = true;
  snprintf(addr, 16, "%s", a);
  *port = (uint16_t)v;
  return 0;
}

int cfg_value_bool(const char *s, bool *v, char *err, size_t cap) {
  char t[16];
  trim_copy(s, t, sizeof t);
  static const char *const yes[] = { "on", "yes", "true", "1" }, *const no[] = { "off", "no", "false", "0" };
  for (int i = 0; i < 4; i++) {
    if (!strcasecmp(t, yes[i])) { *v = true; return 0; }
    if (!strcasecmp(t, no[i])) { *v = false; return 0; }
  }
  char q[48];
  safe_text(s, q, sizeof q);
  snprintf(err, cap, "\"%s\" is not supported - use on or off", q);
  return -1;
}

int cfg_value_name(const char *s, char out[64], char *err, size_t cap) {
  char t[256];
  trim_copy(s, t, sizeof t);
  size_t n = strlen(t);
  bool ok = n >= 1 && n <= 63 && strlen(s ? s : "") < sizeof t - 1;
  for (size_t i = 0; ok && i < n; i++) ok = (unsigned char)t[i] >= 0x20 && t[i] != 0x7f;
  if (!ok) {
    snprintf(err, cap, "the name must be 1 to 63 bytes long (a letter outside ASCII takes 2 to 4), without control "
             "characters");
    return -1;
  }
  memcpy(out, t, n + 1);
  return 0;
}

/* ---------------------------------------------------------------- command line */
enum { O_MODULE, O_RATE, O_INTERVAL, O_CHANNELS, O_PORT, O_HTTP_PORT, O_WEB, O_NO_WEB, O_NO_JOYSTICK,
       O_NO_BUTTONS, O_NAME, O_CONFIG, O_NO_CONFIG, O_DAEMON, O_LIST_RATES, O_VERSION, O_HELP };

typedef struct { const char *name; char shortopt; bool arg; int id; } opt_t;
static const opt_t OPTS[] = {
  { "module", 'm', true, O_MODULE }, { "tx", 0, true, O_MODULE }, { "rate", 'r', true, O_RATE },
  { "interval", 'i', true, O_INTERVAL }, { "channels", 'c', true, O_CHANNELS }, { "port", 'p', true, O_PORT },
  { "http-port", 0, true, O_HTTP_PORT }, { "web", 'w', true, O_WEB }, { "no-web", 0, false, O_NO_WEB },
  { "no-joystick", 0, false, O_NO_JOYSTICK }, { "no-buttons", 0, false, O_NO_BUTTONS }, { "name", 0, true, O_NAME },
  { "config", 0, true, O_CONFIG }, { "no-config", 0, false, O_NO_CONFIG }, { "daemon", 0, false, O_DAEMON },
  { "list-rates", 0, false, O_LIST_RATES }, { "version", 'V', false, O_VERSION }, { "help", 'h', false, O_HELP },
};
#define NOPTS ((int)(sizeof OPTS / sizeof OPTS[0]))

static int arg_err(char *err, size_t cap, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 3, 4)))
#endif
  ;
static int arg_err(char *err, size_t cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, cap, fmt, ap);
  va_end(ap);
  return -1;
}

/* one option with its value (NULL for flags); 1 = stop parsing (an action), 0 ok, -1 error */
static int apply_opt(cfg_args_t *a, const opt_t *o, const char *v, char *err, size_t cap) {
  char e[256];
  int r = 0;
  cfg_settings_t *s = &a->set;
  switch (o->id) {
    case O_MODULE: r = cfg_value_module_ip(v, s->eng.module_ip, e, sizeof e); a->mask |= CFG_K_MODULE_IP; break;
    case O_RATE: r = cfg_value_rate(v, &s->eng.interval_us, e, sizeof e); a->mask |= CFG_K_INTERVAL; break;
    case O_INTERVAL: r = cfg_value_interval(v, &s->eng.interval_us, e, sizeof e); a->mask |= CFG_K_INTERVAL; break;
    case O_CHANNELS: r = cfg_value_channels(v, &s->eng.channels, e, sizeof e); a->mask |= CFG_K_CHANNELS; break;
    case O_PORT: r = cfg_value_port(v, &s->eng.port, e, sizeof e); a->mask |= CFG_K_PORT; break;
    case O_HTTP_PORT: r = cfg_value_port(v, &s->eng.http_port, e, sizeof e); a->mask |= CFG_K_HTTP_PORT; break;
    case O_WEB: r = cfg_value_web(v, &s->web, s->web_addr, &s->web_port, e, sizeof e); a->mask |= CFG_K_WEB; break;
    case O_NO_WEB: s->web = false; a->mask |= CFG_K_WEB; break;
    case O_NO_JOYSTICK: s->joystick = false; a->mask |= CFG_K_JOYSTICK; break;
    case O_NO_BUTTONS: s->buttons = false; a->mask |= CFG_K_BUTTONS; break;
    case O_NAME: r = cfg_value_name(v, s->name, e, sizeof e); a->mask |= CFG_K_NAME; break;
    case O_CONFIG:
      if (!*v || strlen(v) >= sizeof a->config_path) return arg_err(err, cap, "--config: give a file name (up to %d characters)",
                                                                    (int)sizeof a->config_path - 1);
      snprintf(a->config_path, sizeof a->config_path, "%s", v);
      break;
    case O_NO_CONFIG: a->no_config = true; break;
    case O_DAEMON: a->daemon = true; break;
    case O_LIST_RATES: a->action = CFG_LIST_RATES; return 1;
    case O_VERSION: a->action = CFG_VERSION; return 1;
    case O_HELP: a->action = CFG_HELP; return 1;
  }
  if (r) return arg_err(err, cap, "--%s: %s", o->name, e);
  return 0;
}

int cfg_parse_args(int argc, char **argv, cfg_args_t *a, char *err, size_t cap) {
  memset(a, 0, sizeof *a);
  cfg_defaults(&a->set);
  a->action = CFG_RUN;
  if (cap) err[0] = 0;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    char q[48];
    if (!strcmp(arg, "--")) {
      if (i + 1 < argc) {
        safe_text(argv[i + 1], q, sizeof q);
        return arg_err(err, cap, "unexpected argument \"%s\" - see --help", q);
      }
      break;
    }
    if (arg[0] == '-' && arg[1] == '-') {
      const char *name = arg + 2, *eq = strchr(name, '=');
      size_t nlen = eq ? (size_t)(eq - name) : strlen(name);
      const opt_t *o = NULL;
      for (int k = 0; k < NOPTS && !o; k++)
        if (strlen(OPTS[k].name) == nlen && !strncmp(OPTS[k].name, name, nlen)) o = &OPTS[k];
      if (!o) {
        safe_text(arg, q, sizeof q);
        return arg_err(err, cap, "unknown option %s - see --help", q);
      }
      const char *v = NULL;
      if (o->arg) {
        if (eq) v = eq + 1;
        else if (i + 1 < argc) v = argv[++i];
        else return arg_err(err, cap, "--%s needs a value - see --help", o->name);
      } else if (eq) {
        return arg_err(err, cap, "--%s takes no value", o->name);
      }
      int r = apply_opt(a, o, v, err, cap);
      if (r) return r < 0 ? -1 : 0;
      continue;
    }
    if (arg[0] == '-' && arg[1]) {
      /* short options: "-r 500", "-r500", flags may be combined ("-hV") */
      for (const char *c = arg + 1; *c; c++) {
        const opt_t *o = NULL;
        for (int k = 0; k < NOPTS && !o; k++)
          if (OPTS[k].shortopt && OPTS[k].shortopt == *c) o = &OPTS[k];
        if (!o) {
          char ch[8];
          safe_text((char[]){ *c, 0 }, ch, sizeof ch);
          return arg_err(err, cap, "unknown option -%s - see --help", ch);
        }
        const char *v = NULL;
        if (o->arg) {
          if (c[1]) v = c + 1;
          else if (i + 1 < argc) v = argv[++i];
          else return arg_err(err, cap, "--%s needs a value - see --help", o->name);
        }
        int r = apply_opt(a, o, v, err, cap);
        if (r) return r < 0 ? -1 : 0;
        if (o->arg) break;
      }
      continue;
    }
    safe_text(arg, q, sizeof q);
    return arg_err(err, cap, "unexpected argument \"%s\" - see --help", q);
  }
  return 0;
}

void cfg_apply_args(cfg_settings_t *s, const cfg_args_t *a) {
  const cfg_settings_t *c = &a->set;
  if (a->mask & CFG_K_MODULE_IP) snprintf(s->eng.module_ip, sizeof s->eng.module_ip, "%s", c->eng.module_ip);
  if (a->mask & CFG_K_PORT) s->eng.port = c->eng.port;
  if (a->mask & CFG_K_HTTP_PORT) s->eng.http_port = c->eng.http_port;
  if (a->mask & CFG_K_INTERVAL) s->eng.interval_us = c->eng.interval_us;
  if (a->mask & CFG_K_CHANNELS) s->eng.channels = c->eng.channels;
  if (a->mask & CFG_K_WEB) {
    s->web = c->web;
    if (c->web) {
      snprintf(s->web_addr, sizeof s->web_addr, "%s", c->web_addr);
      s->web_port = c->web_port;
    }
  }
  if (a->mask & CFG_K_JOYSTICK) s->joystick = c->joystick;
  if (a->mask & CFG_K_BUTTONS) s->buttons = c->buttons;
  if (a->mask & CFG_K_NAME) snprintf(s->name, sizeof s->name, "%s", c->name);
}

/* ---------------------------------------------------------------- settings file */
typedef struct { const char *key; unsigned bit; } cfg_key_t;
static const cfg_key_t KEYS[] = {
  { "module_ip", CFG_K_MODULE_IP }, { "port", CFG_K_PORT }, { "http_port", CFG_K_HTTP_PORT },
  { "interval_us", CFG_K_INTERVAL }, { "channels", CFG_K_CHANNELS }, { "web", CFG_K_WEB },
  { "joystick", CFG_K_JOYSTICK }, { "buttons", CFG_K_BUTTONS }, { "name", CFG_K_NAME },
};
#define NKEYS ((int)(sizeof KEYS / sizeof KEYS[0]))

int cfg_lock_path(const char *xdg, const char *run_user, unsigned uid, char *out, size_t cap) {
  int n;
  struct stat st;
  if (xdg && xdg[0] == '/') n = snprintf(out, cap, "%s/" CFG_APP ".lock", xdg);
  else if (run_user && run_user[0] == '/' && stat(run_user, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == uid)
    n = snprintf(out, cap, "%s/" CFG_APP ".lock", run_user);
  else n = snprintf(out, cap, "/tmp/" CFG_APP "-%u.lock", uid);
  if (n < 0 || (size_t)n >= cap) { if (cap) out[0] = 0; return -1; }
  return 0;
}

int cfg_default_path(char *out, size_t cap) {
  const char *x = getenv("XDG_CONFIG_HOME");
  int n;
  if (x && x[0] == '/') {
    n = snprintf(out, cap, "%s/" CFG_APP "/config", x);
  } else {
    const char *h = getenv("HOME");
    if (!h || h[0] != '/') {
      struct passwd *pw = getpwuid(getuid());
      h = pw && pw->pw_dir && pw->pw_dir[0] == '/' ? pw->pw_dir : NULL;
    }
    if (!h) { if (cap) out[0] = 0; return -1; }
    n = snprintf(out, cap, "%s/.config/" CFG_APP "/config", h);
  }
  if (n < 0 || (size_t)n >= cap) { if (cap) out[0] = 0; return -1; }
  return 0;
}

/* One line: 0 blank/comment, 1 key = value (key index in *ki, or -1 unknown; value trimmed in val),
 * -1 not a key = value line. */
static int split_line(const char *line, size_t len, int *ki, char *key, size_t kcap, char *val, size_t vcap) {
  const char *s = line, *e = line + len;
  while (s < e && is_blank(*s)) s++;
  while (e > s && is_blank(e[-1])) e--;
  if (s == e || *s == '#') return 0;
  const char *eq = memchr(s, '=', (size_t)(e - s));
  if (!eq) return -1;
  const char *ke = eq;
  while (ke > s && is_blank(ke[-1])) ke--;
  size_t kl = (size_t)(ke - s);
  if (kl == 0) return -1;
  if (kl >= kcap) kl = kcap - 1;
  memcpy(key, s, kl);
  key[kl] = 0;
  const char *v = eq + 1;
  while (v < e && is_blank(*v)) v++;
  size_t vl = (size_t)(e - v);
  /* "a value in quotes" (a name with spaces at its ends) */
  if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') { v++; vl -= 2; }
  if (vl >= vcap) vl = vcap - 1;
  memcpy(val, v, vl);
  val[vl] = 0;
  *ki = -1;
  for (int k = 0; k < NKEYS; k++) if (!strcmp(key, KEYS[k].key)) *ki = k;
  return 1;
}

/* apply one key to s; 0 ok, -1 with err */
static int apply_key(int ki, const char *val, cfg_settings_t *s, char *err, size_t cap) {
  switch (KEYS[ki].bit) {
    case CFG_K_MODULE_IP: return cfg_value_module_ip(val, s->eng.module_ip, err, cap);
    case CFG_K_PORT: return cfg_value_port(val, &s->eng.port, err, cap);
    case CFG_K_HTTP_PORT: return cfg_value_port(val, &s->eng.http_port, err, cap);
    case CFG_K_INTERVAL: return cfg_value_interval(val, &s->eng.interval_us, err, cap);
    case CFG_K_CHANNELS: return cfg_value_channels(val, &s->eng.channels, err, cap);
    case CFG_K_WEB: return cfg_value_web(val, &s->web, s->web_addr, &s->web_port, err, cap);
    case CFG_K_JOYSTICK: return cfg_value_bool(val, &s->joystick, err, cap);
    case CFG_K_BUTTONS: return cfg_value_bool(val, &s->buttons, err, cap);
    case CFG_K_NAME: return cfg_value_name(val, s->name, err, cap);
  }
  return -1;
}

static void warnf(cfg_warn_fn warn, void *ud, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 3, 4)))
#endif
  ;
static void warnf(cfg_warn_fn warn, void *ud, const char *fmt, ...) {
  if (!warn) return;
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  warn(ud, line);
}

static const char *skip_bom(const char *t) {
  return t && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF ? t + 3 : t;
}

unsigned cfg_parse_text(const char *text, cfg_settings_t *s, const char *fname, cfg_warn_fn warn, void *ud) {
  unsigned got = 0;
  int lineno = 0;
  if (!fname) fname = "settings";
  for (const char *t = skip_bom(text); t && *t;) {
    const char *nl = strchr(t, '\n');
    size_t len = nl ? (size_t)(nl - t) : strlen(t);
    lineno++;
    char key[64], val[512], err[256];
    int ki = -1;
    int r = split_line(t, len, &ki, key, sizeof key, val, sizeof val);
    t = nl ? nl + 1 : t + len;
    if (r == 0) continue;
    if (r < 0) { warnf(warn, ud, "%s line %d: not a \"key = value\" line - ignored", fname, lineno); continue; }
    char ks[48];
    safe_text(key, ks, sizeof ks);
    if (ki < 0) { warnf(warn, ud, "%s line %d: unknown key \"%s\" - ignored", fname, lineno, ks); continue; }
    cfg_settings_t tmp = *s;
    if (apply_key(ki, val, &tmp, err, sizeof err)) {
      warnf(warn, ud, "%s line %d: %s: %s - ignored", fname, lineno, ks, err);
      continue;
    }
    *s = tmp;
    got |= KEYS[ki].bit;
  }
  return got;
}

/* the whole file, NUL-terminated, into buf (cap bytes); 1 read, 0 none, -1 error (errno set) */
static int read_file(const char *path, char *buf, size_t cap) {
  buf[0] = 0;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return errno == ENOENT ? 0 : -1;
  size_t got = 0;
  for (;;) {
    if (got >= cap - 1) { close(fd); errno = EFBIG; return -1; }
    ssize_t k = read(fd, buf + got, cap - 1 - got);
    if (k < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      close(fd);
      errno = e;
      return -1;
    }
    if (k == 0) break;
    got += (size_t)k;
  }
  close(fd);
  buf[got] = 0;
  if (memchr(buf, 0, got)) { errno = EINVAL; return -1; }   /* not a text file */
  return 1;
}

int cfg_load(const char *path, cfg_settings_t *s, cfg_warn_fn warn, void *ud) {
  static char buf[CFG_MAX_FILE];
  int r = read_file(path, buf, sizeof buf);
  if (r < 0) {
    warnf(warn, ud, "cannot read the settings file %s: %s - using the defaults", path,
          errno == EFBIG ? "it is too big" : errno == EINVAL ? "it is not a text file" : strerror(errno));
    return -1;
  }
  if (r == 0) return 0;
  cfg_parse_text(buf, s, path, warn, ud);
  return 1;
}

typedef struct { char *p; size_t cap, n; bool over; } sbuf_t;
static void sb_add(sbuf_t *b, const char *s, size_t n) {
  if (b->over || b->n + n + 1 > b->cap) { b->over = true; return; }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}
static void sb_str(sbuf_t *b, const char *s) { sb_add(b, s, strlen(s)); }

static void live_line(sbuf_t *b, unsigned bit, const eng_config_t *c) {
  char line[128];
  switch (bit) {
    case CFG_K_MODULE_IP: snprintf(line, sizeof line, c->module_ip[0] ? "module_ip = %s\n" : "module_ip =\n", c->module_ip); break;
    case CFG_K_PORT: snprintf(line, sizeof line, "port = %u\n", c->port); break;
    case CFG_K_HTTP_PORT: snprintf(line, sizeof line, "http_port = %u\n", c->http_port); break;
    case CFG_K_INTERVAL: snprintf(line, sizeof line, "interval_us = %d\n", c->interval_us); break;
    case CFG_K_CHANNELS: snprintf(line, sizeof line, "channels = %d\n", c->channels); break;
    default: return;
  }
  sb_str(b, line);
}

static const char HEADER[] =
  "# " CFG_APP " settings: \"key = value\" lines; a line that starts with # is a comment.\n"
  "# The program rewrites module_ip, port, http_port, interval_us and channels when you change them\n"
  "# (keys, commands, web page) and keeps every other line. More keys:\n"
  "#   web = 127.0.0.1:11000    [ADDR:]PORT of the web page, or off (0.0.0.0:11000 = reachable from the network)\n"
  "#   joystick = on            off = no virtual joystick (monitor only)\n"
  "#   buttons = on             off = CH9-CH16 are not joystick buttons\n"
  "#   name = " ENG_DEVICE_NAME "\n";

int cfg_render(const char *old, const eng_config_t *c, unsigned keys, char *out, size_t cap) {
  sbuf_t b = { out, cap, 0, cap == 0 };
  if (cap) out[0] = 0;
  unsigned written = 0;
  old = skip_bom(old);
  if (!old || !*old) sb_str(&b, HEADER);
  for (const char *t = old; t && *t;) {
    const char *nl = strchr(t, '\n');
    size_t len = nl ? (size_t)(nl - t) : strlen(t);
    const char *line = t;
    t = nl ? nl + 1 : t + len;
    char key[64], val[512], err[256];
    int ki = -1;
    int r = split_line(line, len, &ki, key, sizeof key, val, sizeof val);
    if (r < 0 || (r == 1 && ki < 0)) continue;           /* dropped: not a setting */
    if (r == 1) {
      unsigned bit = KEYS[ki].bit;
      if (bit & keys) {
        if (!(written & bit)) live_line(&b, bit, c);
        written |= bit;
        continue;
      }
      cfg_settings_t tmp;
      cfg_defaults(&tmp);
      if (apply_key(ki, val, &tmp, err, sizeof err)) continue;   /* a bad value: dropped */
    }
    sb_add(&b, line, len);
    sb_str(&b, "\n");
  }
  for (int k = 0; k < NKEYS; k++)
    if ((KEYS[k].bit & keys & CFG_K_LIVE) && !(written & KEYS[k].bit)) live_line(&b, KEYS[k].bit, c);
  return b.over ? -1 : (int)b.n;
}

unsigned cfg_live_diff(const eng_config_t *a, const eng_config_t *b) {
  unsigned m = 0;
  if (strcmp(a->module_ip, b->module_ip)) m |= CFG_K_MODULE_IP;
  if (a->port != b->port) m |= CFG_K_PORT;
  if (a->http_port != b->http_port) m |= CFG_K_HTTP_PORT;
  if (a->interval_us != b->interval_us) m |= CFG_K_INTERVAL;
  if (a->channels != b->channels) m |= CFG_K_CHANNELS;
  return m;
}

/* mkdir -p of the directory part of path */
static int make_dirs(const char *path) {
  char d[PATH_MAX];
  snprintf(d, sizeof d, "%s", path);
  char *slash = strrchr(d, '/');
  if (!slash || slash == d) return 0;
  *slash = 0;
  for (char *p = d + 1;; p++) {
    if (*p == '/' || *p == 0) {
      char c = *p;
      *p = 0;
      if (mkdir(d, 0700) != 0 && errno != EEXIST) return -1;
      *p = c;
      if (!c) break;
    }
  }
  struct stat st;
  if (stat(d, &st) != 0) return -1;
  if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
  return 0;
}

static int write_all(int fd, const char *s, size_t n) {
  while (n) {
    ssize_t k = write(fd, s, n);
    if (k < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    s += k;
    n -= (size_t)k;
  }
  return 0;
}

/* Where a symlinked settings file points (dotfiles kept elsewhere), also when that file does not exist yet
 * (realpath fails then): the link's target, relative to the link's directory, followed up to 8 links. */
static void link_target(const char *in, char *out, size_t cap) {
  snprintf(out, cap, "%s", in);
  struct stat st;
  for (int depth = 0; depth < 8 && lstat(out, &st) == 0 && S_ISLNK(st.st_mode); depth++) {
    char real[PATH_MAX];
    if (realpath(out, real)) { snprintf(out, cap, "%s", real); return; }
    char t[PATH_MAX];
    ssize_t k = readlink(out, t, sizeof t - 1);
    if (k <= 0) return;
    t[k] = 0;
    char next[PATH_MAX];
    const char *slash = strrchr(out, '/');
    int m = t[0] == '/' || !slash ? snprintf(next, sizeof next, "%s", t)
                                  : snprintf(next, sizeof next, "%.*s%s", (int)(slash - out + 1), out, t);
    if (m < 0 || (size_t)m >= sizeof next || (size_t)m >= cap) return;     /* too long: the link itself */
    memcpy(out, next, (size_t)m + 1);
  }
}

int cfg_save(const char *path_in, const eng_config_t *c, unsigned keys, char *err, size_t cap) {
  if (cap) err[0] = 0;
  keys &= CFG_K_LIVE;
  if (!keys) return 0;
  char path[PATH_MAX];
  link_target(path_in, path, sizeof path);
  struct stat st;
  char *old = malloc(CFG_MAX_FILE), *out = malloc(CFG_MAX_FILE + 1024);
  int e = 0;
  const char *why = NULL;
  char tmp[PATH_MAX + 48];
  tmp[0] = 0;
  int fd = -1;
  bool keep_mode = false;
  mode_t mode = 0;
  if (!old || !out) { e = ENOMEM; goto fail; }
  if (make_dirs(path) != 0) { e = errno; goto fail; }
  int r = read_file(path, old, CFG_MAX_FILE);
  if (r < 0) {
    if (errno == EFBIG || errno == EINVAL) { why = errno == EFBIG ? "the file is too big" : "it is not a text file"; goto fail; }
    e = errno;
    goto fail;
  }
  if (r > 0 && stat(path, &st) == 0) { keep_mode = true; mode = st.st_mode & 07777; }
  int n = cfg_render(r > 0 ? old : NULL, c, keys, out, CFG_MAX_FILE + 1024);
  if (n < 0) { why = "the file is too big"; goto fail; }
  /* temp file next to it (same file system), then fsync + rename: never half a file. An existing file keeps
   * its mode; a new one gets 0666 minus the umask, like any file the user makes (O_EXCL: never an old file) */
  {
    const char *slash = strrchr(path, '/');
    size_t dl = slash ? (size_t)(slash - path) + 1 : 0;
    for (int k = 0; k < 100 && fd < 0; k++) {
      snprintf(tmp, sizeof tmp, "%.*s.%s.%ld.%d.tmp", (int)dl, path, slash ? slash + 1 : path, (long)getpid(), k);
      fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, keep_mode ? 0600 : 0666);
      if (fd < 0 && errno != EEXIST) break;
    }
  }
  if (fd < 0) { e = errno; tmp[0] = 0; goto fail; }
  if (write_all(fd, out, (size_t)n) != 0 || (keep_mode && fchmod(fd, mode) != 0) || fsync(fd) != 0) { e = errno; goto fail; }
  if (close(fd) != 0) { fd = -1; e = errno; goto fail; }
  fd = -1;
  if (rename(tmp, path) != 0) { e = errno; goto fail; }
  tmp[0] = 0;
  {
    /* the rename itself survives a power cut only once the directory is on disk */
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *(slash == dir ? slash + 1 : slash) = 0;
    else snprintf(dir, sizeof dir, ".");
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
      fsync(dfd);
      close(dfd);
    }
  }
  free(old);
  free(out);
  return 0;
fail:
  if (fd >= 0) close(fd);
  if (tmp[0]) unlink(tmp);
  free(old);
  free(out);
  snprintf(err, cap, "cannot save the settings to %s: %s", path, why ? why : strerror(e));
  return -1;
}

/* ---------------------------------------------------------------- text */
void cfg_print_help(FILE *f) {
  fputs("Usage: " CFG_APP " [options]\n"
        "Turns the WiFi joystick stream of an ExpressLRS or TBS Crossfire TX module into a Linux joystick.\n"
        "\n"
        "  -m, --module IP       the module's IP (default: find it by its broadcast); also --tx\n"
        "  -r, --rate HZ         ask the module for HZ updates per second: 1000 500 333 250 200 150 100 50\n"
        "                        (default 250); any 10..1000 is accepted\n"
        "  -i, --interval US     the same as an interval in microseconds: 1000..100000 (default 4000)\n"
        "  -c, --channels N      channels to ask for: 4..16 (default 16)\n"
        "  -p, --port N          UDP port to listen on (default 11000)\n"
        "      --http-port N     the module's web server port (default 80)\n"
        "  -w, --web [ADDR:]PORT web page address (default 127.0.0.1:11000); 0.0.0.0:11000 = reachable from\n"
        "                        the network\n"
        "      --no-web          no web page\n"
        "      --no-joystick     monitor only: no virtual joystick\n"
        "      --no-buttons      CH9-CH16 are not buttons\n"
        "      --name NAME       the joystick's name (default \"" ENG_DEVICE_NAME "\")\n"
        "      --config FILE     settings file (default $XDG_CONFIG_HOME/" CFG_APP "/config)\n"
        "      --no-config       don't read or write a settings file\n"
        "      --daemon          no keys and no status line: log lines only, with timestamps (systemd)\n"
        "      --list-rates      print the supported rates and channel counts, then exit\n"
        "  -V, --version         print the version, then exit\n"
        "  -h, --help            this help, then exit\n"
        "\n"
        "Axes: CH1 X, CH2 Y, CH3 RX, CH4 RY, CH5 RZ, CH6 Z, CH7 throttle, CH8 rudder (0..32767).\n"
        "Buttons: CH9-CH16 = joystick buttons 1-8 (pressed above 75 %).\n"
        "Keys while running: s start/stop, t module IP, r rate, c channels, p UDP port, a restart the stream,\n"
        "w web address, h help, q quit. With stdin not a terminal, the same as lines: start, stop, t <ip>,\n"
        "rate <hz>, interval <us>, channels <n>, port <n>, restart, status, quit.\n"
        "Stop and quit (also Ctrl+C, SIGTERM) release the module: it stops streaming to this machine and\n"
        "broadcasts again, so another app can connect to it (a Crossfire module broadcasts anyway).\n"
        "Exit codes: 0 ok, 1 error, 2 bad options, 3 already running.\n"
        CFG_LINKS "\n", f);
}

void cfg_print_version(FILE *f, const char *version) {
  fprintf(f, CFG_APP " %s\n" CFG_LINKS "\n", version);
}

void cfg_print_rates(FILE *f) {
  fprintf(f, "Rates (--rate HZ, or --interval US):\n");
  for (int i = 0; i < WIFI_N_INTERVAL_PRESETS; i++)
    fprintf(f, "  %4d Hz  %6d us%s\n", wifi_interval_hz(PRESETS[i]), PRESETS[i],
            PRESETS[i] == WIFI_INTERVAL_DEFAULT ? "  (default)" : "");
  fprintf(f, "  any rate from 10 to 1000 Hz, or interval from %d to %d us\n", WIFI_INTERVAL_MIN, WIFI_INTERVAL_MAX);
  fprintf(f, "Channels (--channels N): %d to %d (default %d)\n", WIFI_MIN_CH, WIFI_MAX_CH, WIFI_CHANNELS_DEFAULT);
  fprintf(f, "ELRS (3.4.0 and later) streams what is asked for. Crossfire ignores both: it always sends %d channels\n"
             "at about 90 Hz.\n", WIFI_XF_CHANNELS);
}

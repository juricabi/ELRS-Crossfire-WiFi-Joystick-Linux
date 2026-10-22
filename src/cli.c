/* The terminal UI: see cli.h. */
#include "cli.h"
#include "config.h"
#include "joymap.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DRAW_MS 100                   /* the interactive status line, ~10 Hz */
#define STATUS_LINES_MS 2000          /* redirected: a status line this often while streaming */
#define STATUS_DAEMON_MS 60000
#define STOP_WAIT_MS 1500             /* at quit: time for the engine to say "Stopped" */

/* ================================================================ signals */
static int sig_pipe[2] = { -1, -1 };
static volatile sig_atomic_t g_quit;      /* how many quit signals came */
static volatile sig_atomic_t g_raw;       /* the terminal is in raw mode: handlers put it back */
static volatile sig_atomic_t g_redraw;
static struct termios g_orig, g_rawt;     /* written before g_raw is set */

static void wake_main(void) {
  int e = errno;
  char c = 1;
  if (sig_pipe[1] >= 0 && write(sig_pipe[1], &c, 1) < 0) { /* full: awake anyway */ }
  errno = e;
}

static void tty_restore_now(void) {
  if (g_raw) tcsetattr(STDIN_FILENO, TCSANOW, &g_orig);
}

static void on_quit(int sig) {
  /* the third one: whatever is stuck, the user wants out (the terminal first) */
  if (++g_quit >= 3) {
    tty_restore_now();
    _exit(128 + sig);
  }
  wake_main();
}

static void on_fatal(int sig) {
  tty_restore_now();
  signal(sig, SIG_DFL);
  raise(sig);
}

static bool foreground(void) { return tcgetpgrp(STDIN_FILENO) == getpgrp(); }

/* Ctrl+Z: a normal terminal while stopped, raw mode again after fg */
static void on_tstp(int sig) {
  (void)sig;
  int e = errno;
  if (g_raw) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig);
    if (write(STDOUT_FILENO, "\r\n", 2) < 0) {}
  }
  kill(getpid(), SIGSTOP);
  if (g_raw && foreground()) tcsetattr(STDIN_FILENO, TCSANOW, &g_rawt);
  g_redraw = 1;
  wake_main();
  errno = e;
}

static void on_cont(int sig) {
  (void)sig;
  int e = errno;
  if (g_raw && foreground()) tcsetattr(STDIN_FILENO, TCSANOW, &g_rawt);
  g_redraw = 1;
  wake_main();
  errno = e;
}

static void handle(int sig, void (*fn)(int)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = fn;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(sig, &sa, NULL);
}

int cli_signals_init(void) {
  if (sig_pipe[0] < 0 && pipe2(sig_pipe, O_NONBLOCK | O_CLOEXEC) != 0) return -1;
  signal(SIGPIPE, SIG_IGN);
  handle(SIGINT, on_quit);
  handle(SIGTERM, on_quit);
  handle(SIGHUP, on_quit);
  return 0;
}

bool cli_quit_requested(void) { return g_quit > 0; }

/* ================================================================ pure parts */
static const struct { const char *name; cli_cmd_t cmd; } CMDS[] = {
  { "start", CLI_START }, { "stop", CLI_STOP }, { "s", CLI_TOGGLE }, { "t", CLI_TARGET }, { "module", CLI_TARGET },
  { "rate", CLI_RATE }, { "r", CLI_RATE }, { "interval", CLI_INTERVAL }, { "i", CLI_INTERVAL },
  { "channels", CLI_CHANNELS }, { "c", CLI_CHANNELS }, { "port", CLI_PORT }, { "p", CLI_PORT },
  { "http-port", CLI_HTTP_PORT }, { "restart", CLI_RESTART }, { "a", CLI_RESTART }, { "status", CLI_STATUS },
  { "web", CLI_WEB }, { "w", CLI_WEB }, { "help", CLI_HELP }, { "h", CLI_HELP }, { "?", CLI_HELP },
  { "quit", CLI_QUIT }, { "q", CLI_QUIT }, { "exit", CLI_QUIT },
};

static bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

cli_cmd_t cli_parse_line(const char *line, char *arg, size_t cap) {
  if (cap) arg[0] = 0;
  if (!line) return CLI_NONE;
  if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line += 3;
  while (blank(*line)) line++;
  if (!*line) return CLI_NONE;
  const char *w = line;
  while (*line && !blank(*line)) line++;
  size_t wl = (size_t)(line - w);
  while (blank(*line)) line++;
  size_t al = strlen(line);
  while (al && blank(line[al - 1])) al--;
  if (cap) {
    if (al >= cap) al = cap - 1;
    memcpy(arg, line, al);
    arg[al] = 0;
  }
  for (size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++)
    if (strlen(CMDS[i].name) == wl && !strncasecmp(CMDS[i].name, w, wl)) return CMDS[i].cmd;
  return CLI_UNKNOWN;
}

/* bounded appends */
typedef struct { char *p; size_t cap, n; int w, maxw; } lb_t;
static void lb_raw(lb_t *b, const char *s) {
  size_t k = strlen(s);
  if (b->n + k + 1 > b->cap) return;
  memcpy(b->p + b->n, s, k);
  b->n += k;
  b->p[b->n] = 0;
}
/* visible text, whole or not at all */
static bool lb_text(lb_t *b, const char *s) {
  int w = cli_width(s);
  if (b->w + w > b->maxw || b->n + strlen(s) + 1 > b->cap) return false;
  lb_raw(b, s);
  b->w += w;
  return true;
}
static void lb_fmt(lb_t *b, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;
static void lb_fmt(lb_t *b, const char *fmt, ...) {
  char t[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(t, sizeof t, fmt, ap);
  va_end(ap);
  lb_text(b, t);
}

int cli_width(const char *s) {
  int w = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p;) {
    if (*p == 0x1b) {                                   /* ESC [ ... final byte */
      p++;
      if (*p == '[') {
        p++;
        while (*p && !(*p >= 0x40 && *p <= 0x7e)) p++;
        if (*p) p++;
      }
      continue;
    }
    if ((*p & 0xC0) != 0x80) w++;                       /* count characters, not bytes */
    p++;
  }
  return w;
}

void cli_status_plain(const eng_snapshot_t *s, char *out, size_t cap) {
  lb_t b = { out, cap, 0, 0, 1 << 30 };
  if (cap) out[0] = 0;
  int hz = wifi_interval_hz(s->cfg.interval_us);
  if (s->bound) {
    lb_fmt(&b, "streaming from %s %s", wifi_kind_name(s->kind), s->source);
    if (s->module_name[0]) lb_fmt(&b, " (%s)", s->module_name);
    lb_fmt(&b, " | %.1f Hz, jitter %.2f ms | asked %d Hz x %d ch", s->rate_hz, s->jitter_ms, hz, s->cfg.channels);
    if (s->nch) {
      lb_text(&b, " | CH1-8 %");
      for (int i = 0; i < 8 && i < s->nch; i++) lb_fmt(&b, " %d", wifi_raw_percent(s->raw[i]));
      if (s->nch > 8 && s->joy_buttons) {
        lb_text(&b, " | buttons ");
        for (int i = 8; i < 16; i++) lb_text(&b, i < s->nch && s->raw[i] >= JOY_BTN_PRESS ? "X" : "-");
      } else if (s->nch > 8) {
        lb_text(&b, " | CH9-16 %");
        for (int i = 8; i < s->nch; i++) lb_fmt(&b, " %d", wifi_raw_percent(s->raw[i]));
      }
    }
  } else {
    lb_text(&b, s->status);
    lb_fmt(&b, " | asked %d Hz x %d ch | UDP %u", hz, s->cfg.channels, s->cfg.port);
  }
  if (s->joy_enabled && !s->joy_ok) lb_text(&b, " | no joystick (monitor only)");
}

void cli_status_tty(const eng_snapshot_t *s, int width, bool utf8, bool color, bool *btn, char *out, size_t cap) {
  if (width < 10) width = 10;
  lb_t b = { out, cap, 0, 0, width - 1 };
  if (cap) out[0] = 0;
  const char *col = s->state == WIFI_ST_STREAMING ? "\x1b[32m" : s->state == WIFI_ST_ERROR ? "\x1b[31m"
                    : s->running ? "\x1b[33m" : "\x1b[90m";
  const char *dot = utf8 ? (s->bound ? "\xe2\x97\x8f " : s->state == WIFI_ST_ERROR ? "\xe2\x9c\x96 " : "\xe2\x97\x8b ")
                         : (s->bound ? "* " : s->state == WIFI_ST_ERROR ? "x " : "o ");
  if (color) lb_raw(&b, col);
  /* the state always shows, cut if it must be */
  char st[96];
  snprintf(st, sizeof st, "%s%s", dot, s->status);
  if (!lb_text(&b, st)) {
    for (const char *p = st; *p && b.w < b.maxw;) {
      int len = 1;
      while (((unsigned char)p[len] & 0xC0) == 0x80) len++;
      char ch[8];
      memcpy(ch, p, (size_t)len);
      ch[len] = 0;
      lb_text(&b, ch);
      p += len;
    }
  }
  if (color) lb_raw(&b, "\x1b[0m");
  char t[128];
  if (s->bound) {
    snprintf(t, sizeof t, "  %s %s", wifi_kind_name(s->kind), s->source);
    lb_text(&b, t);
    /* the first second after a lock has no rate yet */
    if (s->rate_hz > 0) {
      snprintf(t, sizeof t, utf8 ? "  %.1f Hz \xc2\xb1%.2f ms" : "  %.1f Hz +-%.2f ms", s->rate_hz, s->jitter_ms);
      lb_text(&b, t);
    }
  }
  snprintf(t, sizeof t, utf8 ? "  ask %d Hz\xc3\x97%d" : "  ask %d Hz x%d", wifi_interval_hz(s->cfg.interval_us),
           s->cfg.channels);
  lb_text(&b, t);
  if (!s->running) lb_text(&b, "  (s = start)");
  if (s->bound && s->nch) {
    /* CH1-8: one bar each (8 levels), then the 8 button marks */
    static const char *const bars[8] = { "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
                                         "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88" };
    char bar[64] = "  ";
    for (int i = 0; i < 8; i++) {
      int v = i < s->nch ? s->raw[i] : 0;
      if (utf8) strcat(bar, bars[v * 8 / (WIFI_RAW_MAX + 1)]);
      else {
        char d[2] = { (char)('0' + v * 10 / (WIFI_RAW_MAX + 1)), 0 };
        strcat(bar, d);
      }
    }
    lb_text(&b, bar);
    if (s->joy_buttons && s->nch > 8) {
      char marks[64] = " ";
      for (int i = 0; i < 8; i++) {
        if (8 + i < s->nch) btn[i] = joymap_button_next(btn[i], s->raw[8 + i]);
        strcat(marks, btn[i] ? (utf8 ? "\xe2\x97\x8f" : "X") : (utf8 ? "\xc2\xb7" : "-"));
      }
      lb_text(&b, marks);
    }
  } else {
    for (int i = 0; i < 8; i++) btn[i] = false;
  }
  if (s->joy_enabled && !s->joy_ok) {
    if (color) lb_raw(&b, "\x1b[33m");
    lb_text(&b, "  monitor only");
    if (color) lb_raw(&b, "\x1b[0m");
  }
}

void cli_time(int64_t wall_ms, bool iso, char *out, size_t cap) {
  time_t t = (time_t)(wall_ms / 1000);
  struct tm tm;
  if (!localtime_r(&t, &tm)) { snprintf(out, cap, "?"); return; }
  if (!iso) {
    strftime(out, cap, "%H:%M:%S", &tm);
    return;
  }
  char a[32];
  strftime(a, sizeof a, "%Y-%m-%dT%H:%M:%S", &tm);
  long off = tm.tm_gmtoff / 60;
  char sign = off < 0 ? '-' : '+';
  if (off < 0) off = -off;
  snprintf(out, cap, "%s.%03d%c%02ld:%02ld", a, (int)(wall_ms % 1000), sign, off / 60, off % 60);
}

/* ================================================================ the running UI */
enum { M_TTY, M_LINES, M_DAEMON };

typedef struct {
  const cli_opts_t *o;
  engine_t *e;
  int mode;
  bool read_stdin;          /* commands (keys or lines) come from stdin */
  bool color, utf8, ansi;
  bool quit;
  uint32_t since;
  bool status_shown;
  bool btn[8];
  int prompt;               /* a key whose prompt is open (cooked mode), 0 = none */
  char line[1024];
  size_t llen;
  bool overflow, bom_checked;
  int64_t next_status;
} cli_t;

static int64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t wall_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int term_width(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 10) return ws.ws_col;
  return 80;
}

static void clear_status(cli_t *c) {
  if (!c->status_shown) return;
  if (c->ansi) fputs("\r\x1b[K", stdout);
  else {
    int w = term_width();
    fputc('\r', stdout);
    for (int i = 0; i < w - 1; i++) fputc(' ', stdout);
    fputc('\r', stdout);
  }
  c->status_shown = false;
}

static const char *line_color(const char *t) {
  if (!strncmp(t, "ERROR", 5)) return "\x1b[31m";
  if (!strncmp(t, "!!", 2)) return "\x1b[33m";
  if (strstr(t, "source locked to") || !strncmp(t, "Virtual joystick", 16)) return "\x1b[32m";
  return NULL;
}

/* one line with its time, above the status line */
static void out_line(cli_t *c, int64_t when, const char *text) {
  char ts[48];
  cli_time(when, c->mode == M_DAEMON, ts, sizeof ts);
  clear_status(c);
  const char *col = c->color ? line_color(text) : NULL;
  if (c->color) printf("\x1b[90m%s\x1b[0m  %s%s%s\n", ts, col ? col : "", text, col ? "\x1b[0m" : "");
  else printf(c->mode == M_DAEMON ? "%s %s\n" : "%s  %s\n", ts, text);
}

static void say(cli_t *c, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;
static void say(cli_t *c, const char *fmt, ...) {
  char t[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(t, sizeof t, fmt, ap);
  va_end(ap);
  out_line(c, wall_ms(), t);
}

static void print_logs(cli_t *c) {
  eng_logline_t l[64];
  int n;
  while ((n = eng_log_read(c->e, c->since, l, 64)) > 0) {
    if (l[0].seq > c->since + 1) say(c, "(%u older log lines were not shown)", l[0].seq - c->since - 1);
    for (int i = 0; i < n; i++) out_line(c, l[i].wall_ms, l[i].text);
    c->since = l[n - 1].seq;
  }
}

static void draw_status(cli_t *c) {
  eng_snapshot_t s;
  eng_snapshot(c->e, &s);
  char line[1024];
  cli_status_tty(&s, term_width(), c->utf8, c->color, c->btn, line, sizeof line);
  fputc('\r', stdout);
  fputs(line, stdout);
  if (c->ansi) fputs("\x1b[K", stdout);
  c->status_shown = true;
}

static void web_info(cli_t *c) {
  if (c->o->web) {
    char u[128];
    web_url(c->o->web, u, sizeof u);
    say(c, "Web page: %s", u);
  } else {
    say(c, "No web page: %s", c->o->web_note && *c->o->web_note ? c->o->web_note : "off");
  }
}

static void help(cli_t *c) {
  if (c->mode == M_TTY) {
    say(c, "Keys: s start/stop, t module IP, r rate, c channels, p UDP port, a restart the stream (send the request again),");
    say(c, "      w web page address, h help, q quit (also Ctrl+C)");
  } else {
    say(c, "Commands: start, stop, s (start/stop), t <ip> (t alone: find the module by its broadcast), rate <hz>,");
    say(c, "          interval <us>, channels <n>, port <n>, http-port <n>, restart, status, web, help, quit");
  }
}

static void run_cmd(cli_t *c, cli_cmd_t cmd, const char *arg) {
  eng_snapshot_t s;
  eng_snapshot(c->e, &s);
  eng_config_t n = s.cfg;
  char err[256] = "";
  int r = 0;
  const char *name = NULL;
  switch (cmd) {
    case CLI_NONE: return;
    case CLI_START: eng_start(c->e); return;
    case CLI_STOP: eng_stop(c->e); return;
    case CLI_TOGGLE:
      if (s.running) eng_stop(c->e);
      else eng_start(c->e);
      return;
    case CLI_TARGET: name = "module IP"; r = cfg_value_module_ip(arg, n.module_ip, err, sizeof err); break;
    case CLI_RATE: name = "rate"; r = cfg_value_rate(arg, &n.interval_us, err, sizeof err); break;
    case CLI_INTERVAL: name = "interval"; r = cfg_value_interval(arg, &n.interval_us, err, sizeof err); break;
    case CLI_CHANNELS: name = "channels"; r = cfg_value_channels(arg, &n.channels, err, sizeof err); break;
    case CLI_PORT: name = "port"; r = cfg_value_port(arg, &n.port, err, sizeof err); break;
    case CLI_HTTP_PORT: name = "http-port"; r = cfg_value_port(arg, &n.http_port, err, sizeof err); break;
    case CLI_RESTART: eng_restart_stream(c->e); return;
    case CLI_STATUS: {
      char line[512];
      cli_status_plain(&s, line, sizeof line);
      say(c, "%s", line);
      return;
    }
    case CLI_WEB: web_info(c); return;
    case CLI_HELP: help(c); return;
    case CLI_QUIT: c->quit = true; return;
    case CLI_UNKNOWN: return;
  }
  if (cmd != CLI_TARGET && !*arg) {
    say(c, "%s needs a value, e.g. %s", name, cmd == CLI_RATE ? "rate 250" : cmd == CLI_INTERVAL ? "interval 4000"
                                           : cmd == CLI_CHANNELS ? "channels 16" : cmd == CLI_PORT ? "port 11000" : "http-port 80");
    return;
  }
  if (r) { say(c, "%s: %s", name, err); return; }
  if (!cfg_live_diff(&n, &s.cfg)) {
    /* nothing would happen (and nothing would be logged): say so */
    if (cmd == CLI_TARGET && n.module_ip[0]) say(c, "The module IP is %s already (a = send the request again)", n.module_ip);
    else if (cmd == CLI_TARGET) say(c, "The module is found by its broadcast already");
    else if (cmd == CLI_RATE || cmd == CLI_INTERVAL)
      say(c, "Asking for %d Hz (%d us) already", wifi_interval_hz(n.interval_us), n.interval_us);
    else if (cmd == CLI_CHANNELS) say(c, "Asking for %d channels already", n.channels);
    else if (cmd == CLI_PORT) say(c, "Listening on UDP port %u already", n.port);
    else say(c, "The module's web server port is %u already", n.http_port);
    return;
  }
  /* a failed save is logged by the engine itself */
  if (eng_set_config(c->e, &n, err, sizeof err) < 0) say(c, "%s", err);
}

/* ---- line commands (stdin not a terminal) ---- */
static void do_line(cli_t *c, char *line) {
  char arg[512];
  cli_cmd_t cmd = cli_parse_line(line, arg, sizeof arg);
  if (cmd == CLI_UNKNOWN) {
    char safe[64];
    size_t k = 0;
    for (const char *p = line; *p && k < 48; p++)
      if (!blank(*p) || k) safe[k++] = ((unsigned char)*p >= 0x20 && *p != 0x7f) ? *p : '?';
    while (k && blank(safe[k - 1])) k--;
    safe[k] = 0;
    say(c, "Unknown command \"%s\" - try help", safe);
    return;
  }
  run_cmd(c, cmd, arg);
}

static void read_lines(cli_t *c) {
  char buf[4096];
  ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
  if (n < 0) {
    if (errno == EAGAIN || errno == EINTR) return;
    c->read_stdin = false;
    return;
  }
  if (n == 0) {
    /* EOF ends the commands, not the program (cmd < /dev/null keeps running) */
    if (c->llen && !c->overflow) { c->line[c->llen] = 0; do_line(c, c->line); }
    c->llen = 0;
    c->read_stdin = false;
    return;
  }
  size_t i = 0;
  if (!c->bom_checked && c->llen + (size_t)n >= 3) {
    c->bom_checked = true;
    /* a UTF-8 BOM before the first command (PowerShell, .NET writers) */
    if (c->llen == 0 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) i = 3;
  }
  for (; i < (size_t)n && !c->quit; i++) {
    char ch = buf[i];
    if (ch == '\n') {
      c->line[c->llen] = 0;
      if (c->overflow) say(c, "Command line too long - ignored");
      else do_line(c, c->line);
      c->llen = 0;
      c->overflow = false;
      continue;
    }
    if (c->llen + 1 >= sizeof c->line) { c->overflow = true; continue; }
    c->line[c->llen++] = ch;
  }
}

/* ---- keys (interactive) ---- */
static void tty_raw(bool on) {
  if (on) {
    if (tcsetattr(STDIN_FILENO, TCSANOW, &g_rawt) == 0) g_raw = 1;
  } else if (g_raw) {
    g_raw = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig);
  }
}

static void open_prompt(cli_t *c, int key) {
  eng_snapshot_t s;
  eng_snapshot(c->e, &s);
  print_logs(c);
  clear_status(c);
  tty_raw(false);
  c->prompt = key;
  const char *cy = c->color ? "\x1b[36m" : "", *nc = c->color ? "\x1b[0m" : "";
  switch (key) {
    case 't':
      printf("%sModule IP (now %s; empty = find it by its broadcast, Esc Enter = cancel): %s", cy,
             s.cfg.module_ip[0] ? s.cfg.module_ip : "found by its broadcast", nc);
      break;
    case 'r':
      printf("%sRate in Hz: 1000 500 333 250 200 150 100 50, or 10..1000 (now %d; Enter = keep): %s", cy,
             wifi_interval_hz(s.cfg.interval_us), nc);
      break;
    case 'c':
      printf("%sChannels 4-16 (now %d; Enter = keep): %s", cy, s.cfg.channels, nc);
      break;
    case 'p':
      printf("%sUDP port 1-65535 (now %u; Enter = keep): %s", cy, s.cfg.port, nc);
      break;
  }
  fflush(stdout);
}

static void prompt_answer(cli_t *c) {
  char buf[256];
  ssize_t n = read(STDIN_FILENO, buf, sizeof buf - 1);
  if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
  int key = c->prompt;
  c->prompt = 0;
  tty_raw(true);
  if (n <= 0) { fputc('\n', stdout); return; }           /* Ctrl+D: cancelled */
  buf[n] = 0;
  bool esc = strchr(buf, 0x1b) != NULL;
  char *nl = strchr(buf, '\n');
  if (nl) *nl = 0;
  char arg[256];
  size_t k = 0;
  const char *p = buf;
  while (blank(*p)) p++;
  for (; *p && k + 1 < sizeof arg; p++) arg[k++] = *p;
  while (k && blank(arg[k - 1])) k--;
  arg[k] = 0;
  if (esc || (!arg[0] && key != 't')) return;          /* cancelled / kept */
  cli_cmd_t cmd = key == 't' ? CLI_TARGET : key == 'r' ? CLI_RATE : key == 'c' ? CLI_CHANNELS : CLI_PORT;
  run_cmd(c, cmd, arg);
}

static void read_keys(cli_t *c) {
  char buf[64];
  ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
  if (n < 0) return;
  if (n == 0) { c->quit = true; return; }              /* the terminal went away */
  if (buf[0] == 0x1b) return;                          /* Esc, arrows, function keys */
  for (ssize_t i = 0; i < n && !c->quit; i++) {
    char k = buf[i];
    if (k >= 'A' && k <= 'Z') k = (char)(k - 'A' + 'a');
    switch (k) {
      case 's': run_cmd(c, CLI_TOGGLE, ""); break;
      case 't': case 'r': case 'c': case 'p': open_prompt(c, k); return;   /* the rest was typed ahead */
      case 'a': run_cmd(c, CLI_RESTART, ""); break;
      case 'w': run_cmd(c, CLI_WEB, ""); break;
      case 'h': case '?': run_cmd(c, CLI_HELP, ""); break;
      case 'q': c->quit = true; break;
      case '\n': case '\r': case ' ': break;
      default:
        if ((unsigned char)k >= 0x20 && k != 0x7f) say(c, "Unknown key '%c' - h for help", k);
        break;
    }
  }
}

/* ================================================================ run */
static bool env_utf8(void) {
  const char *v[] = { getenv("LC_ALL"), getenv("LC_CTYPE"), getenv("LANG") };
  for (int i = 0; i < 3; i++) {
    if (!v[i] || !*v[i]) continue;
    /* the first one set decides, as for setlocale */
    return strcasestr(v[i], "utf-8") || strcasestr(v[i], "utf8");
  }
  return false;
}

static void header(cli_t *c) {
  eng_snapshot_t s;
  eng_snapshot(c->e, &s);
  char web[160] = "no web page";
  if (c->o->web) {
    memcpy(web, "web page ", 9);
    web_url(c->o->web, web + 9, sizeof web - 9);
  }
  if (c->mode == M_TTY) {
    const char *b = c->color ? "\x1b[1;36m" : "", *d = c->color ? "\x1b[90m" : "", *n = c->color ? "\x1b[0m" : "";
    printf("%sELRS / Crossfire WiFi Joystick %s%s - UDP %u - %s\n", b, c->o->version, n, s.cfg.port, web);
    printf("%sKeys: [s] start/stop  [t] module IP  [r] rate  [c] channels  [p] UDP port  [a] restart stream  "
           "[w] web  [h] help  [q] quit%s\n", d, n);
  } else if (c->mode == M_LINES) {
    printf("elrs-wifi-joystick %s - UDP %u - %s\n", c->o->version, s.cfg.port, web);
    if (c->read_stdin) printf("Commands on stdin: start, stop, t <ip>, rate <hz>, channels <n>, port <n>, restart, status, quit\n");
  } else {
    char ts[48];
    cli_time(wall_ms(), true, ts, sizeof ts);
    printf("%s elrs-wifi-joystick %s - UDP %u - %s\n", ts, c->o->version, s.cfg.port, web);
  }
  fflush(stdout);
}

int cli_run(const cli_opts_t *o) {
  cli_t c;
  memset(&c, 0, sizeof c);
  c.o = o;
  c.e = o->e;
  bool in_tty = isatty(STDIN_FILENO), out_tty = isatty(STDOUT_FILENO);
  bool fg = in_tty && foreground();
  if (o->daemon) c.mode = M_DAEMON;
  else if (in_tty && out_tty && fg && tcgetattr(STDIN_FILENO, &g_orig) == 0) c.mode = M_TTY;
  else c.mode = M_LINES;
  /* a terminal on stdin read in the background would stop the program (SIGTTIN) */
  c.read_stdin = c.mode == M_TTY || (c.mode == M_LINES && (!in_tty || fg));
  const char *term = getenv("TERM"), *nocolor = getenv("NO_COLOR");
  c.ansi = c.mode == M_TTY && term && *term && strcmp(term, "dumb");
  c.color = c.ansi && !(nocolor && *nocolor);
  c.utf8 = env_utf8();
  if (!out_tty) setvbuf(stdout, NULL, _IOLBF, 0);
  if (c.mode == M_TTY) {
    g_rawt = g_orig;
    g_rawt.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    g_rawt.c_cc[VMIN] = 1;
    g_rawt.c_cc[VTIME] = 0;
    handle(SIGTSTP, on_tstp);
    handle(SIGCONT, on_cont);
    int fatal[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    for (int i = 0; i < 5; i++) handle(fatal[i], on_fatal);
    tty_raw(true);
  }
  header(&c);
  int64_t now = mono_ms(), next_draw = now;
  c.next_status = now + (c.mode == M_DAEMON ? STATUS_DAEMON_MS : STATUS_LINES_MS);
  while (!c.quit && !cli_quit_requested()) {
    struct pollfd pf[2] = { { sig_pipe[0], POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 } };
    int nfd = c.read_stdin ? 2 : 1;
    int timeout = c.mode == M_TTY && !c.prompt ? (int)(next_draw - mono_ms()) : c.mode == M_TTY ? DRAW_MS : 200;
    if (timeout < 0) timeout = 0;
    if (timeout > DRAW_MS) timeout = DRAW_MS;
    int r = poll(pf, (nfds_t)nfd, timeout);
    if (r > 0 && pf[0].revents) {
      char b[64];
      while (read(sig_pipe[0], b, sizeof b) > 0) {}
    }
    if (cli_quit_requested()) break;
    if (r > 0 && nfd == 2 && pf[1].revents) {
      if (pf[1].revents & POLLNVAL) c.read_stdin = false;
      else if (c.mode == M_TTY) {
        if (c.prompt) prompt_answer(&c);
        else read_keys(&c);
      } else {
        read_lines(&c);
      }
    }
    if (c.prompt) { fflush(stdout); continue; }        /* the prompt line stays clean */
    print_logs(&c);
    now = mono_ms();
    if (c.mode == M_TTY) {
      if (now >= next_draw || g_redraw) {
        g_redraw = 0;
        draw_status(&c);
        next_draw = now + DRAW_MS;
      }
    } else if (now >= c.next_status) {
      c.next_status = now + (c.mode == M_DAEMON ? STATUS_DAEMON_MS : STATUS_LINES_MS);
      eng_snapshot_t s;
      eng_snapshot(c.e, &s);
      if (s.bound) {
        char line[512];
        cli_status_plain(&s, line, sizeof line);
        say(&c, "%s", line);
      }
    }
    fflush(stdout);
  }
  /* quitting: stop (the joystick centers), show the last lines, give the terminal back */
  if (c.prompt) { fputc('\n', stdout); c.prompt = 0; }
  eng_snapshot_t s;
  eng_snapshot(c.e, &s);
  if (s.running) {
    eng_stop(c.e);
    int64_t end = mono_ms() + STOP_WAIT_MS;
    while (mono_ms() < end) {
      eng_snapshot(c.e, &s);
      if (!s.running) break;
      eng_wait(c.e, s.seq, s.log_seq, 50);
    }
  }
  print_logs(&c);
  clear_status(&c);
  fflush(stdout);
  if (c.mode == M_TTY) tty_raw(false);
  return 0;
}

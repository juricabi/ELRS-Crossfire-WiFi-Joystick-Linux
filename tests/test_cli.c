/* cli: the pure parts - command lines, the plain and the terminal status line, times, widths.
 * (The running UI is tested by tests/e2e.py: line commands, --daemon, keys on a pseudo terminal.) */
#include "tests/t.h"
#include "cli.h"

#include <stdlib.h>
#include <time.h>

static void parse(void) {
  char arg[64];
  CHECK_INT(cli_parse_line("start", arg, sizeof arg), CLI_START);
  CHECK_STR(arg, "");
  CHECK_INT(cli_parse_line("  STOP  ", arg, sizeof arg), CLI_STOP);
  CHECK_INT(cli_parse_line("s", arg, sizeof arg), CLI_TOGGLE);
  CHECK_INT(cli_parse_line("t 10.0.0.1", arg, sizeof arg), CLI_TARGET);
  CHECK_STR(arg, "10.0.0.1");
  CHECK_INT(cli_parse_line("t", arg, sizeof arg), CLI_TARGET);
  CHECK_STR(arg, "");
  CHECK_INT(cli_parse_line("module\t 10.0.0.2 \r", arg, sizeof arg), CLI_TARGET);
  CHECK_STR(arg, "10.0.0.2");
  CHECK_INT(cli_parse_line("rate 500", arg, sizeof arg), CLI_RATE);
  CHECK_STR(arg, "500");
  CHECK_INT(cli_parse_line("r 500", arg, sizeof arg), CLI_RATE);
  CHECK_INT(cli_parse_line("interval 6667", arg, sizeof arg), CLI_INTERVAL);
  CHECK_INT(cli_parse_line("i 6667", arg, sizeof arg), CLI_INTERVAL);
  CHECK_INT(cli_parse_line("channels 8", arg, sizeof arg), CLI_CHANNELS);
  CHECK_INT(cli_parse_line("c 8", arg, sizeof arg), CLI_CHANNELS);
  CHECK_INT(cli_parse_line("port 11001", arg, sizeof arg), CLI_PORT);
  CHECK_INT(cli_parse_line("p 11001", arg, sizeof arg), CLI_PORT);
  CHECK_INT(cli_parse_line("http-port 8080", arg, sizeof arg), CLI_HTTP_PORT);
  CHECK_INT(cli_parse_line("restart", arg, sizeof arg), CLI_RESTART);
  CHECK_INT(cli_parse_line("a", arg, sizeof arg), CLI_RESTART);
  CHECK_INT(cli_parse_line("Status", arg, sizeof arg), CLI_STATUS);
  CHECK_INT(cli_parse_line("web", arg, sizeof arg), CLI_WEB);
  CHECK_INT(cli_parse_line("w", arg, sizeof arg), CLI_WEB);
  CHECK_INT(cli_parse_line("help", arg, sizeof arg), CLI_HELP);
  CHECK_INT(cli_parse_line("?", arg, sizeof arg), CLI_HELP);
  CHECK_INT(cli_parse_line("q", arg, sizeof arg), CLI_QUIT);
  CHECK_INT(cli_parse_line("QUIT", arg, sizeof arg), CLI_QUIT);
  CHECK_INT(cli_parse_line("exit", arg, sizeof arg), CLI_QUIT);
  CHECK_INT(cli_parse_line("\xEF\xBB\xBFq", arg, sizeof arg), CLI_QUIT);     /* a BOM */
  CHECK_INT(cli_parse_line("", arg, sizeof arg), CLI_NONE);
  CHECK_INT(cli_parse_line("   \t\r\n", arg, sizeof arg), CLI_NONE);
  CHECK_INT(cli_parse_line(NULL, arg, sizeof arg), CLI_NONE);
  CHECK_INT(cli_parse_line("quitt", arg, sizeof arg), CLI_UNKNOWN);
  CHECK_INT(cli_parse_line("rate500", arg, sizeof arg), CLI_UNKNOWN);
  CHECK_INT(cli_parse_line("x y z", arg, sizeof arg), CLI_UNKNOWN);
  CHECK_STR(arg, "y z");
  char small[4];
  CHECK_INT(cli_parse_line("t 192.168.100.200", small, sizeof small), CLI_TARGET);
  CHECK_STR(small, "192");                                          /* cut, terminated */
}

static void snap_streaming(eng_snapshot_t *s) {
  memset(s, 0, sizeof *s);
  eng_config_default(&s->cfg);
  s->running = s->listening = s->bound = true;
  s->state = WIFI_ST_STREAMING;
  snprintf(s->status, sizeof s->status, "streaming");
  snprintf(s->source, sizeof s->source, "192.168.1.50");
  s->kind = WIFI_KIND_ELRS;
  snprintf(s->module_name, sizeof s->module_name, "Unified TX");
  s->rate_hz = 249.84;
  s->jitter_ms = 0.416;
  s->nch = 16;
  for (int i = 0; i < 16; i++) s->raw[i] = 16384;
  s->raw[2] = 0;
  s->raw[3] = 32767;
  s->raw[8] = 30000;
  s->raw[9] = 22000;
  s->joy_enabled = s->joy_ok = s->joy_buttons = true;
}

static void plain(void) {
  eng_snapshot_t s;
  char out[512];
  snap_streaming(&s);
  cli_status_plain(&s, out, sizeof out);
  CHECK_STR(out, "streaming from ELRS 192.168.1.50 (Unified TX) | 249.8 Hz, jitter 0.42 ms | asked 250 Hz x 16 ch"
                 " | CH1-8 % 50 50 0 100 50 50 50 50 | buttons X-------");
  s.joy_buttons = false;
  cli_status_plain(&s, out, sizeof out);
  CHECK(strstr(out, "| CH9-16 % 91 67 50 50 50 50 50 50") != NULL);
  s.nch = 8;
  s.module_name[0] = 0;
  s.kind = WIFI_KIND_UNKNOWN;
  cli_status_plain(&s, out, sizeof out);
  CHECK_STR(out, "streaming from WiFi 192.168.1.50 | 249.8 Hz, jitter 0.42 ms | asked 250 Hz x 16 ch"
                 " | CH1-8 % 50 50 0 100 50 50 50 50");
  /* not streaming */
  memset(&s, 0, sizeof s);
  eng_config_default(&s.cfg);
  s.cfg.interval_us = 6667;
  s.cfg.channels = 8;
  s.running = true;
  snprintf(s.status, sizeof s.status, "activating 10.0.0.1");
  cli_status_plain(&s, out, sizeof out);
  CHECK_STR(out, "activating 10.0.0.1 | asked 150 Hz x 8 ch | UDP 11000");
  s.joy_enabled = true;
  cli_status_plain(&s, out, sizeof out);
  CHECK_STR(out, "activating 10.0.0.1 | asked 150 Hz x 8 ch | UDP 11000 | no joystick (monitor only)");
  /* small buffers: cut and terminated */
  char tiny[12];
  cli_status_plain(&s, tiny, sizeof tiny);
  CHECK(strlen(tiny) < sizeof tiny);
}

static void tty(void) {
  eng_snapshot_t s;
  char out[1024];
  bool btn[8] = { 0 };
  snap_streaming(&s);
  cli_status_tty(&s, 200, true, false, btn, out, sizeof out);
  CHECK(strstr(out, "\xe2\x97\x8f streaming") == out);              /* "● streaming" */
  CHECK(strstr(out, "ELRS 192.168.1.50") != NULL);
  CHECK(strstr(out, "249.8 Hz \xc2\xb1" "0.42 ms") != NULL);
  CHECK(strstr(out, "ask 250 Hz\xc3\x97" "16") != NULL);
  /* bars: 16384 -> level 4, 0 -> 0, 32767 -> 7 */
  CHECK(strstr(out, "\xe2\x96\x85\xe2\x96\x85\xe2\x96\x81\xe2\x96\x88\xe2\x96\x85") != NULL);
  CHECK(btn[0] && !btn[1]);                                          /* 30000 pressed, 22000 not */
  CHECK(strstr(out, "\xe2\x97\x8f\xc2\xb7") != NULL);                /* "●·" */
  CHECK(cli_width(out) <= 199);
  CHECK(strchr(out, 0x1b) == NULL);                                  /* no color asked */
  s.rate_hz = 0;                                                     /* not measured yet: no rate */
  cli_status_tty(&s, 200, true, false, btn, out, sizeof out);
  CHECK(strstr(out, " Hz \xc2\xb1") == NULL);
  CHECK(strstr(out, "ask 250 Hz") != NULL);
  s.rate_hz = 249.84;
  /* hysteresis across calls: 22000 after 30000 stays pressed */
  s.raw[8] = 22000;
  cli_status_tty(&s, 200, true, false, btn, out, sizeof out);
  CHECK(btn[0]);
  s.raw[8] = 20000;
  cli_status_tty(&s, 200, true, false, btn, out, sizeof out);
  CHECK(!btn[0]);
  /* ASCII */
  s.raw[8] = 32767;
  cli_status_tty(&s, 200, false, false, btn, out, sizeof out);
  CHECK(!strncmp(out, "* streaming", 11));
  CHECK(strstr(out, "+-0.42 ms") != NULL);
  CHECK(strstr(out, "ask 250 Hz x16") != NULL);
  CHECK(strstr(out, "  55095555 X-------") != NULL);
  for (const char *p = out; *p; p++) CHECK((unsigned char)*p < 0x80);
  /* color: escape codes take no columns */
  cli_status_tty(&s, 200, true, true, btn, out, sizeof out);
  CHECK(strstr(out, "\x1b[32m") == out);
  char nocolor[1024];
  cli_status_tty(&s, 200, true, false, btn, nocolor, sizeof nocolor);
  CHECK_INT(cli_width(out), cli_width(nocolor));
  /* narrow terminals: never wider than width-1; the state always shows */
  for (int w = 10; w <= 120; w++) {
    cli_status_tty(&s, w, true, true, btn, out, sizeof out);
    if (cli_width(out) > w - 1) { CHECK(false); fprintf(stderr, "width %d: %d columns\n", w, cli_width(out)); break; }
    if (!strstr(out, "stream")) { CHECK(false); break; }
  }
  cli_status_tty(&s, 3, true, false, btn, out, sizeof out);         /* below 10 counts as 10 */
  CHECK(cli_width(out) <= 9);
  CHECK(cli_width(out) >= 5);
  /* not streaming: no bars, the buttons forgotten; stopped says how to start */
  memset(&s, 0, sizeof s);
  eng_config_default(&s.cfg);
  snprintf(s.status, sizeof s.status, "stopped");
  s.joy_enabled = true;
  cli_status_tty(&s, 200, true, false, btn, out, sizeof out);
  CHECK(!strncmp(out, "\xe2\x97\x8b stopped", 10));
  CHECK(strstr(out, "(s = start)") != NULL);
  CHECK(strstr(out, "monitor only") != NULL);
  CHECK(strstr(out, "\xe2\x96") == NULL);
  for (int i = 0; i < 8; i++) CHECK(!btn[i]);
  s.state = WIFI_ST_ERROR;
  s.running = true;
  snprintf(s.status, sizeof s.status, "UDP port 11000 in use");
  cli_status_tty(&s, 200, false, true, btn, out, sizeof out);
  CHECK(strstr(out, "\x1b[31mx UDP port 11000 in use") == out);
}

static void times(void) {
  setenv("TZ", "UTC", 1);
  tzset();
  char out[64];
  int64_t t = (int64_t)1729605787 * 1000 + 45;       /* 2024-10-22 14:03:07.045 UTC */
  cli_time(t, false, out, sizeof out);
  CHECK_STR(out, "14:03:07");
  cli_time(t, true, out, sizeof out);
  CHECK_STR(out, "2024-10-22T14:03:07.045+00:00");
  setenv("TZ", "XXX-2", 1);                           /* 2 hours east of UTC, POSIX style */
  tzset();
  cli_time(t, true, out, sizeof out);
  CHECK_STR(out, "2024-10-22T16:03:07.045+02:00");
  setenv("TZ", "YYY+3:30", 1);
  tzset();
  cli_time(t, true, out, sizeof out);
  CHECK_STR(out, "2024-10-22T10:33:07.045-03:30");
}

static void widths(void) {
  CHECK_INT(cli_width(""), 0);
  CHECK_INT(cli_width("abc"), 3);
  CHECK_INT(cli_width("\x1b[1;32mab\x1b[0m"), 2);
  CHECK_INT(cli_width("\xe2\x96\x81\xc3\x97x"), 3);
  CHECK_INT(cli_width("\x1b"), 0);
  CHECK_INT(cli_width("\x1b[31"), 0);                 /* cut sequence: nothing past it counts */
}

int main(void) {
  parse();
  plain();
  tty();
  times();
  widths();
  T_DONE();
}

/* config: command line, value parsers, the settings file (parse, render, save, load), help texts. */
#include "tests/t.h"
#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char err[512];
static cfg_args_t A;

static int parse(int n, const char **v) {
  const char *argv[32] = { "elrs-wifi-joystick" };
  for (int i = 0; i < n; i++) argv[i + 1] = v[i];
  return cfg_parse_args(n + 1, (char **)argv, &A, err, sizeof err);
}
#define NV(...) ((int)(sizeof((const char *[]){__VA_ARGS__}) / sizeof(const char *)))
#define ARGS(...) parse(NV(__VA_ARGS__), (const char *[]){__VA_ARGS__})

/* collected warnings */
static char warns[32][512];
static int nwarn;
static void on_warn(void *ud, const char *m) {
  (void)ud;
  if (nwarn < 32) snprintf(warns[nwarn], sizeof warns[0], "%s", m);
  nwarn++;
}
static bool warned(const char *sub) {
  for (int i = 0; i < nwarn && i < 32; i++) if (strstr(warns[i], sub)) return true;
  return false;
}

static void defaults(void) {
  cfg_settings_t s;
  cfg_defaults(&s);
  CHECK_STR(s.eng.module_ip, "");
  CHECK_INT(s.eng.port, 11000);
  CHECK_INT(s.eng.http_port, 80);
  CHECK_INT(s.eng.interval_us, 4000);
  CHECK_INT(s.eng.channels, 16);
  CHECK(s.web);
  CHECK_STR(s.web_addr, "127.0.0.1");
  CHECK_INT(s.web_port, 11000);
  CHECK(s.joystick && s.buttons);
  CHECK_STR(s.name, "");
  char e2[200];
  CHECK_INT(eng_config_check(&s.eng, e2, sizeof e2), 0);
}

static void values(void) {
  long v;
  CHECK_INT(cfg_parse_int("42", 0, 100, &v), 0);
  CHECK_INT(v, 42);
  CHECK_INT(cfg_parse_int(" 42 ", 0, 100, &v), 0);
  CHECK_INT(cfg_parse_int("+7", 0, 100, &v), 0);
  CHECK_INT(cfg_parse_int("-7", -10, 100, &v), 0);
  CHECK_INT(v, -7);
  CHECK_INT(cfg_parse_int("101", 0, 100, &v), -2);
  CHECK_INT(cfg_parse_int("-1", 0, 100, &v), -2);
  CHECK_INT(cfg_parse_int("", 0, 100, &v), -1);
  CHECK_INT(cfg_parse_int("-", 0, 100, &v), -1);
  CHECK_INT(cfg_parse_int("4a", 0, 100, &v), -1);
  CHECK_INT(cfg_parse_int("0x10", 0, 100, &v), -1);
  CHECK_INT(cfg_parse_int("1 2", 0, 100, &v), -1);
  CHECK_INT(cfg_parse_int("99999999999999999999", 0, 100, &v), -2);   /* no overflow */
  CHECK_INT(cfg_parse_int(NULL, 0, 100, &v), -1);
  CHECK_INT(cfg_parse_int("00000000000000000000000000000000000000001", 0, 100, &v), -1);
  /* 10 to 18 digits: with a 32-bit long (i686, armv7, armv6) these wrapped into range and were taken */
  CHECK_INT(cfg_parse_int("4294971296", 1000, 100000, &v), -2);   /* 2^32 + 4000 */
  CHECK_INT(cfg_parse_int("4294967304", 4, 16, &v), -2);          /* 2^32 + 8 */
  CHECK_INT(cfg_parse_int("2147483648", 0, 2147483647L, &v), -2);
  CHECK_INT(cfg_parse_int("-2147483649", -2147483647L - 1, 0, &v), -2);
  CHECK_INT(cfg_parse_int("18446744073709555616", 1000, 100000, &v), -2);
  CHECK_INT(cfg_parse_int("999999999999999999", 0, 100, &v), -2);
  CHECK_INT(cfg_parse_int("0000004000", 1000, 100000, &v), 0);
  CHECK_INT(v, 4000);
  v = 77;
  CHECK_INT(cfg_parse_int("4294967304", 4, 16, &v), -2);
  CHECK_INT(v, 77);                                  /* out of range: *out untouched */

  char ip[64];
  CHECK_INT(cfg_value_module_ip(" 10.0.0.1\t", ip, err, sizeof err), 0);
  CHECK_STR(ip, "10.0.0.1");
  CHECK_INT(cfg_value_module_ip("", ip, err, sizeof err), 0);
  CHECK_STR(ip, "");
  CHECK_INT(cfg_value_module_ip("10.0.0", ip, err, sizeof err), -1);
  CHECK_STR(err, "\"10.0.0\" is not an IPv4 address - use a.b.c.d, or leave it empty to find the module by its broadcast");
  CHECK_INT(cfg_value_module_ip("0.0.0.0", ip, err, sizeof err), -1);
  CHECK_INT(cfg_value_module_ip("010.0.0.1", ip, err, sizeof err), -1);
  CHECK_INT(cfg_value_module_ip("elrs\x01tx", ip, err, sizeof err), -1);
  CHECK(strstr(err, "elrs?tx") != NULL);                               /* one clean line */

  int iv;
  CHECK_INT(cfg_value_rate("250", &iv, err, sizeof err), 0);
  CHECK_INT(iv, 4000);
  static const int hz[] = { 1000, 500, 333, 250, 200, 150, 100, 50 }, us[] = { 1000, 2000, 3000, 4000, 5000, 6667, 10000, 20000 };
  for (int i = 0; i < 8; i++) {
    CHECK_INT(cfg_rate_to_interval(hz[i]), us[i]);
    CHECK_INT(wifi_interval_hz(cfg_rate_to_interval(hz[i])), hz[i]);
  }
  CHECK_INT(cfg_rate_to_interval(10), 100000);
  CHECK_INT(cfg_rate_to_interval(999), 1001);
  CHECK_INT(cfg_rate_to_interval(300), 3333);
  CHECK_INT(cfg_rate_to_interval(9), 0);
  CHECK_INT(cfg_rate_to_interval(1001), 0);
  for (int h = 10; h <= 1000; h++) {           /* every accepted rate is a supported interval */
    int u = cfg_rate_to_interval(h);
    if (u < WIFI_INTERVAL_MIN || u > WIFI_INTERVAL_MAX) { CHECK(false); break; }
  }
  CHECK_INT(cfg_value_rate("7", &iv, err, sizeof err), -1);
  CHECK_STR(err, "7 is not supported - use 1000, 500, 333, 250, 200, 150, 100 or 50 (any 10 to 1000 works)");
  CHECK_INT(cfg_value_rate("fast", &iv, err, sizeof err), -1);
  CHECK_STR(err, "\"fast\" is not a number - use 1000, 500, 333, 250, 200, 150, 100 or 50 (any 10 to 1000 works)");
  CHECK_INT(cfg_value_interval("999", &iv, err, sizeof err), -1);
  CHECK_STR(err, "999 is not supported - use 1000 to 100000");
  CHECK_INT(cfg_value_interval("100000", &iv, err, sizeof err), 0);
  CHECK_INT(cfg_value_interval("100001", &iv, err, sizeof err), -1);
  CHECK_INT(cfg_value_channels("3", &iv, err, sizeof err), -1);
  CHECK_STR(err, "3 is not supported - use 4 to 16");
  CHECK_INT(cfg_value_channels("17", &iv, err, sizeof err), -1);
  CHECK_INT(cfg_value_channels("4", &iv, err, sizeof err), 0);
  CHECK_INT(iv, 4);
  CHECK_INT(cfg_value_channels("4294967304", &iv, err, sizeof err), -1);
  CHECK_STR(err, "4294967304 is not supported - use 4 to 16");
  CHECK_INT(cfg_value_interval("4294971296", &iv, err, sizeof err), -1);
  CHECK_INT(cfg_value_rate("4294967546", &iv, err, sizeof err), -1);
  uint16_t port;
  CHECK_INT(cfg_value_port("0", &port, err, sizeof err), -1);
  CHECK_STR(err, "0 is not supported - use 1 to 65535");
  CHECK_INT(cfg_value_port("65536", &port, err, sizeof err), -1);
  CHECK_INT(cfg_value_port("65535", &port, err, sizeof err), 0);
  CHECK_INT(port, 65535);
  CHECK_INT(cfg_value_port("4294978297", &port, err, sizeof err), -1);

  bool on;
  char addr[16];
  CHECK_INT(cfg_value_web("8080", &on, addr, &port, err, sizeof err), 0);
  CHECK(on);
  CHECK_STR(addr, "127.0.0.1");
  CHECK_INT(port, 8080);
  CHECK_INT(cfg_value_web("0.0.0.0:11000", &on, addr, &port, err, sizeof err), 0);
  CHECK_STR(addr, "0.0.0.0");
  CHECK_INT(cfg_value_web("localhost:9000", &on, addr, &port, err, sizeof err), 0);
  CHECK_STR(addr, "127.0.0.1");
  CHECK_INT(port, 9000);
  CHECK_INT(cfg_value_web("OFF", &on, addr, &port, err, sizeof err), 0);
  CHECK(!on);
  CHECK_INT(cfg_value_web("example.com:80", &on, addr, &port, err, sizeof err), -1);
  CHECK(strstr(err, "is not a web address - use [ADDR:]PORT") != NULL);
  CHECK_INT(cfg_value_web("1.2.3.4:", &on, addr, &port, err, sizeof err), -1);
  CHECK_INT(cfg_value_web(":80", &on, addr, &port, err, sizeof err), -1);
  CHECK_INT(cfg_value_web("1.2.3.4:0", &on, addr, &port, err, sizeof err), -1);
  CHECK_INT(cfg_value_web("", &on, addr, &port, err, sizeof err), -1);
  CHECK_INT(cfg_value_web("4294978296", &on, addr, &port, err, sizeof err), -1);

  bool b;
  const char *t[] = { "on", "ON", "yes", "true", "1", " on " }, *f[] = { "off", "no", "False", "0" };
  for (int i = 0; i < 6; i++) { b = false; CHECK_INT(cfg_value_bool(t[i], &b, err, sizeof err), 0); CHECK(b); }
  for (int i = 0; i < 4; i++) { b = true; CHECK_INT(cfg_value_bool(f[i], &b, err, sizeof err), 0); CHECK(!b); }
  CHECK_INT(cfg_value_bool("maybe", &b, err, sizeof err), -1);
  CHECK_STR(err, "\"maybe\" is not supported - use on or off");

  char name[64];
  CHECK_INT(cfg_value_name("  My Stick  ", name, err, sizeof err), 0);
  CHECK_STR(name, "My Stick");
  CHECK_INT(cfg_value_name("Ünïcödé Stick", name, err, sizeof err), 0);
  CHECK_INT(cfg_value_name("", name, err, sizeof err), -1);
  CHECK_INT(cfg_value_name("bad\tname", name, err, sizeof err), -1);
  char longname[80];
  memset(longname, 'x', 64);
  longname[64] = 0;
  CHECK_INT(cfg_value_name(longname, name, err, sizeof err), -1);
  longname[63] = 0;
  CHECK_INT(cfg_value_name(longname, name, err, sizeof err), 0);
  /* the limit is in bytes, and the message says so: 36 Cyrillic letters are 64 bytes */
  CHECK_INT(cfg_value_name("\xd0\x92\xd0\xb8\xd1\x80\xd1\x82\xd1\x83\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd1\x8b\xd0\xb9 "
                           "\xd0\xb4\xd0\xb6\xd0\xbe\xd0\xb9\xd1\x81\xd1\x82\xd0\xb8\xd0\xba \xd0\xb4\xd0\xbb\xd1\x8f "
                           "\xd0\xbf\xd0\xb8\xd0\xbb\xd0\xbe\xd1\x82\xd0\xb0 ELRS", name, err, sizeof err), -1);
  CHECK_STR(err, "the name must be 1 to 63 bytes long (a letter outside ASCII takes 2 to 4), without control characters");
}

static void command_line(void) {
  CHECK_INT(ARGS("--no-config"), 0);
  CHECK(A.no_config);
  CHECK_INT(A.action, CFG_RUN);
  CHECK_INT(A.mask, 0);

  CHECK_INT(ARGS("-m", "192.168.4.1", "-r", "500", "-c", "8", "-p", "12000", "--http-port", "8080", "-w", "0.0.0.0:9000",
                 "--no-buttons", "--name", "Sim Stick", "--config", "/x/y", "--daemon"), 0);
  CHECK_STR(err, "");
  CHECK_STR(A.set.eng.module_ip, "192.168.4.1");
  CHECK_INT(A.set.eng.interval_us, 2000);
  CHECK_INT(A.set.eng.channels, 8);
  CHECK_INT(A.set.eng.port, 12000);
  CHECK_INT(A.set.eng.http_port, 8080);
  CHECK(A.set.web);
  CHECK_STR(A.set.web_addr, "0.0.0.0");
  CHECK_INT(A.set.web_port, 9000);
  CHECK(!A.set.buttons);
  CHECK(A.set.joystick);
  CHECK_STR(A.set.name, "Sim Stick");
  CHECK_STR(A.config_path, "/x/y");
  CHECK(A.daemon);
  CHECK_INT(A.mask, CFG_K_MODULE_IP | CFG_K_INTERVAL | CFG_K_CHANNELS | CFG_K_PORT | CFG_K_HTTP_PORT | CFG_K_WEB |
                    CFG_K_BUTTONS | CFG_K_NAME);

  /* --opt=value, -oVALUE, aliases */
  CHECK_INT(ARGS("--module=10.0.0.2", "--rate=333", "--channels=16", "-p11001", "--no-web", "--no-joystick"), 0);
  CHECK_STR(A.set.eng.module_ip, "10.0.0.2");
  CHECK_INT(A.set.eng.interval_us, 3000);
  CHECK_INT(A.set.eng.port, 11001);
  CHECK(!A.set.web);
  CHECK(!A.set.joystick);
  CHECK_INT(ARGS("--tx", "10.0.0.3"), 0);
  CHECK_STR(A.set.eng.module_ip, "10.0.0.3");
  CHECK_INT(ARGS("-m", ""), 0);                    /* explicit discovery over a file's module_ip */
  CHECK_STR(A.set.eng.module_ip, "");
  CHECK(A.mask & CFG_K_MODULE_IP);
  /* --rate and --interval: the later one wins */
  CHECK_INT(ARGS("--rate", "500", "--interval", "6667"), 0);
  CHECK_INT(A.set.eng.interval_us, 6667);
  CHECK_INT(ARGS("-i", "6667", "-r", "1000"), 0);
  CHECK_INT(A.set.eng.interval_us, 1000);
  CHECK_INT(ARGS("--web", "off"), 0);
  CHECK(!A.set.web);
  CHECK_INT(ARGS("--web", "8081"), 0);
  CHECK(A.set.web);
  CHECK_INT(A.set.web_port, 8081);

  /* actions stop the parsing */
  CHECK_INT(ARGS("--help", "--bogus"), 0);
  CHECK_INT(A.action, CFG_HELP);
  CHECK_INT(ARGS("-h"), 0);
  CHECK_INT(A.action, CFG_HELP);
  CHECK_INT(ARGS("-V"), 0);
  CHECK_INT(A.action, CFG_VERSION);
  CHECK_INT(ARGS("--version"), 0);
  CHECK_INT(A.action, CFG_VERSION);
  CHECK_INT(ARGS("--list-rates"), 0);
  CHECK_INT(A.action, CFG_LIST_RATES);
  CHECK_INT(ARGS("-hV"), 0);
  CHECK_INT(A.action, CFG_HELP);

  /* errors: one line, naming the option and what is supported */
  CHECK_INT(ARGS("--channels", "3"), -1);
  CHECK_STR(err, "--channels: 3 is not supported - use 4 to 16");
  CHECK_INT(ARGS("-c", "17"), -1);
  CHECK_STR(err, "--channels: 17 is not supported - use 4 to 16");
  CHECK_INT(ARGS("--rate", "7"), -1);
  CHECK_STR(err, "--rate: 7 is not supported - use 1000, 500, 333, 250, 200, 150, 100 or 50 (any 10 to 1000 works)");
  CHECK_INT(ARGS("--interval", "500"), -1);
  CHECK_STR(err, "--interval: 500 is not supported - use 1000 to 100000");
  CHECK_INT(ARGS("--module", "10.0.0"), -1);
  CHECK(!strncmp(err, "--module: \"10.0.0\" is not an IPv4 address", 41));
  CHECK_INT(ARGS("--tx", "x"), -1);
  CHECK(!strncmp(err, "--tx: ", 6));
  CHECK_INT(ARGS("--port", "0"), -1);
  CHECK_STR(err, "--port: 0 is not supported - use 1 to 65535");
  CHECK_INT(ARGS("--http-port", "70000"), -1);
  CHECK_STR(err, "--http-port: 70000 is not supported - use 1 to 65535");
  CHECK_INT(ARGS("--web", "nowhere"), -1);
  CHECK(!strncmp(err, "--web: \"nowhere\" is not a web address", 37));
  CHECK_INT(ARGS("--name", ""), -1);
  CHECK(!strncmp(err, "--name: ", 8));
  CHECK_INT(ARGS("--config", ""), -1);
  CHECK(!strncmp(err, "--config: ", 10));
  CHECK_INT(ARGS("--bogus"), -1);
  CHECK_STR(err, "unknown option --bogus - see --help");
  CHECK_INT(ARGS("-x"), -1);
  CHECK_STR(err, "unknown option -x - see --help");
  CHECK_INT(ARGS("--rate"), -1);
  CHECK_STR(err, "--rate needs a value - see --help");
  CHECK_INT(ARGS("-r"), -1);
  CHECK_STR(err, "--rate needs a value - see --help");
  CHECK_INT(ARGS("--daemon=yes"), -1);
  CHECK_STR(err, "--daemon takes no value");
  CHECK_INT(ARGS("11000"), -1);
  CHECK_STR(err, "unexpected argument \"11000\" - see --help");
  CHECK_INT(ARGS("--", "x"), -1);
  CHECK_STR(err, "unexpected argument \"x\" - see --help");
  CHECK_INT(ARGS("--"), 0);
  CHECK_INT(ARGS("-"), -1);
  CHECK_INT(ARGS("--rat", "250"), -1);              /* no abbreviations */
  CHECK_INT(ARGS("--channels", "8", "--channels", "x"), -1);
  CHECK_STR(err, "--channels: \"x\" is not a number - use 4 to 16");
  CHECK_INT(ARGS("--bogus\x1b[31m"), -1);
  CHECK(strchr(err, 0x1b) == NULL);                 /* never a terminal escape in an error */
}

static void precedence(void) {
  cfg_settings_t s;
  cfg_defaults(&s);
  nwarn = 0;
  unsigned got = cfg_parse_text("interval_us = 2000\nchannels = 8\nport = 12000\nweb = off\n", &s, "f", on_warn, NULL);
  CHECK_INT(got, CFG_K_INTERVAL | CFG_K_CHANNELS | CFG_K_PORT | CFG_K_WEB);
  CHECK_INT(nwarn, 0);
  CHECK_INT(ARGS("--channels", "12", "--web", "9000"), 0);
  cfg_apply_args(&s, &A);
  CHECK_INT(s.eng.interval_us, 2000);               /* from the file */
  CHECK_INT(s.eng.channels, 12);                    /* the command line wins */
  CHECK_INT(s.eng.port, 12000);
  CHECK(s.web);
  CHECK_INT(s.web_port, 9000);
  CHECK_INT(s.eng.http_port, 80);                   /* default */
  /* --no-web over a file's web address; the address stays what the file said */
  cfg_defaults(&s);
  cfg_parse_text("web = 0.0.0.0:1234\njoystick = off\nname = X\nmodule_ip = 1.2.3.4\n", &s, "f", on_warn, NULL);
  CHECK_INT(ARGS("--no-web", "-m", "5.6.7.8", "--http-port", "81", "--name", "Y"), 0);
  cfg_apply_args(&s, &A);
  CHECK(!s.web);
  CHECK(!s.joystick);
  CHECK_STR(s.name, "Y");
  CHECK_STR(s.eng.module_ip, "5.6.7.8");
  CHECK_INT(s.eng.http_port, 81);
  CHECK_INT(ARGS("--no-joystick", "--no-buttons", "-i", "1000", "-p", "2"), 0);
  cfg_defaults(&s);
  cfg_apply_args(&s, &A);
  CHECK(!s.joystick && !s.buttons);
  CHECK_INT(s.eng.interval_us, 1000);
  CHECK_INT(s.eng.port, 2);
}

static void settings_text(void) {
  cfg_settings_t s;
  cfg_defaults(&s);
  nwarn = 0;
  const char *text =
    "\xEF\xBB\xBF# my settings\r\n"
    "\n"
    "   # indented comment\n"
    "module_ip = 192.168.4.1\r\n"
    "port=12000\n"
    "http_port = 8080\n"
    "interval_us = 2000\n"
    "channels = 8\n"
    "web = 0.0.0.0:9000\n"
    "joystick = off\n"
    "buttons = no\n"
    "name = \"  Spaced Name  \"\n"
    "colour = blue\n"
    "channels = 3\n"
    "this line has no equals sign\n"
    "= 5\n"
    "port = 11000 # a comment after the value\n"
    "interval_us = 3000";                          /* no newline at the end */
  unsigned got = cfg_parse_text(text, &s, "cfg", on_warn, NULL);
  CHECK_INT(got, CFG_K_MODULE_IP | CFG_K_PORT | CFG_K_HTTP_PORT | CFG_K_INTERVAL | CFG_K_CHANNELS | CFG_K_WEB |
                 CFG_K_JOYSTICK | CFG_K_BUTTONS | CFG_K_NAME);
  CHECK_STR(s.eng.module_ip, "192.168.4.1");
  CHECK_INT(s.eng.port, 12000);                     /* the bad later line is ignored */
  CHECK_INT(s.eng.http_port, 8080);
  CHECK_INT(s.eng.interval_us, 3000);               /* duplicates: the last good one wins */
  CHECK_INT(s.eng.channels, 8);
  CHECK(s.web);
  CHECK_STR(s.web_addr, "0.0.0.0");
  CHECK_INT(s.web_port, 9000);
  CHECK(!s.joystick);
  CHECK(!s.buttons);
  CHECK_STR(s.name, "Spaced Name");
  CHECK_INT(nwarn, 5);
  CHECK(warned("cfg line 13: unknown key \"colour\" - ignored"));
  CHECK(warned("cfg line 14: channels: 3 is not supported - use 4 to 16 - ignored"));
  CHECK(warned("cfg line 15: not a \"key = value\" line - ignored"));
  CHECK(warned("cfg line 16: not a \"key = value\" line - ignored"));
  CHECK(warned("cfg line 17: port: \"11000 # a comment after the value\" is not a number"));
  /* an empty module_ip = discovery */
  cfg_parse_text("module_ip =\n", &s, NULL, on_warn, NULL);
  CHECK_STR(s.eng.module_ip, "");
  /* nothing at all */
  cfg_defaults(&s);
  CHECK_INT(cfg_parse_text("", &s, "x", NULL, NULL), 0);
  CHECK_INT(cfg_parse_text(NULL, &s, "x", NULL, NULL), 0);
  CHECK_INT(s.eng.port, 11000);
}

static void render(void) {
  eng_config_t c;
  eng_config_default(&c);
  c.interval_us = 2000;
  snprintf(c.module_ip, sizeof c.module_ip, "10.0.0.9");
  static char out[8192];
  /* a new file: the header, then the keys asked for */
  int n = cfg_render(NULL, &c, CFG_K_INTERVAL, out, sizeof out);
  CHECK(n > 0);
  CHECK_INT(n, (int)strlen(out));
  CHECK(!strncmp(out, "# elrs-wifi-joystick settings", 29));
  CHECK(strstr(out, "\ninterval_us = 2000\n") != NULL);
  CHECK(strstr(out, "\nmodule_ip") == NULL);
  CHECK(strstr(out, "#   web = 127.0.0.1:11000") != NULL);
  /* what was written reads back */
  cfg_settings_t s;
  cfg_defaults(&s);
  nwarn = 0;
  CHECK_INT(cfg_parse_text(out, &s, "x", on_warn, NULL), CFG_K_INTERVAL);
  CHECK_INT(nwarn, 0);
  CHECK_INT(s.eng.interval_us, 2000);
  /* every live key */
  n = cfg_render("", &c, CFG_K_LIVE, out, sizeof out);
  CHECK(strstr(out, "\nmodule_ip = 10.0.0.9\nport = 11000\nhttp_port = 80\ninterval_us = 2000\nchannels = 16\n") != NULL);
  c.module_ip[0] = 0;
  cfg_render("", &c, CFG_K_MODULE_IP, out, sizeof out);
  CHECK(strstr(out, "\nmodule_ip =\n") != NULL);

  /* an existing file: the first line of a key replaced in place, later ones dropped, unknown keys and
   * bad values dropped, comments and the other keys kept as they are, missing keys appended */
  const char *old =
    "# mine\n"
    "web = 0.0.0.0:9000   \n"
    "channels = 8\n"
    "\n"
    "colour = blue\n"
    "port = 12000\n"
    "joystick = maybe\n"
    "channels = 12\n"
    "name = Keep Me\n"
    "port = x\n"
    "garbage line\n"
    "interval_us = 2000";
  eng_config_default(&c);
  c.channels = 6;
  c.http_port = 81;
  n = cfg_render(old, &c, CFG_K_CHANNELS | CFG_K_HTTP_PORT, out, sizeof out);
  CHECK_STR(out,
    "# mine\n"
    "web = 0.0.0.0:9000   \n"
    "channels = 6\n"
    "\n"
    "port = 12000\n"
    "name = Keep Me\n"
    "interval_us = 2000\n"
    "http_port = 81\n");
  /* a BOM is not kept twice, CRLF lines are rewritten as LF */
  n = cfg_render("\xEF\xBB\xBF# x\r\nport = 1\r\n", &c, CFG_K_PORT, out, sizeof out);
  CHECK_STR(out, "# x\r\nport = 11000\n");
  /* too small */
  CHECK_INT(cfg_render(old, &c, CFG_K_LIVE, out, 20), -1);
  CHECK_INT(cfg_render(old, &c, CFG_K_LIVE, out, 0), -1);
  /* keys = 0: the file as it is, minus what is not a setting */
  cfg_render("port = 5\n", &c, 0, out, sizeof out);
  CHECK_STR(out, "port = 5\n");
}

static void diff(void) {
  eng_config_t a, b;
  eng_config_default(&a);
  b = a;
  CHECK_INT(cfg_live_diff(&a, &b), 0);
  b.port = 1;
  b.channels = 4;
  snprintf(b.module_ip, sizeof b.module_ip, "1.2.3.4");
  CHECK_INT(cfg_live_diff(&a, &b), CFG_K_PORT | CFG_K_CHANNELS | CFG_K_MODULE_IP);
  b = a;
  b.http_port = 2;
  b.interval_us = 1000;
  CHECK_INT(cfg_live_diff(&a, &b), CFG_K_HTTP_PORT | CFG_K_INTERVAL);
}

static char dir[256];

static int count_files(const char *d) {
  DIR *dp = opendir(d);
  if (!dp) return -1;
  int n = 0;
  struct dirent *de;
  while ((de = readdir(dp))) n += de->d_name[0] != '.' || (de->d_name[1] && de->d_name[1] != '.');
  closedir(dp);
  return n;
}

static void read_text(const char *path, char *buf, size_t cap) {
  buf[0] = 0;
  FILE *f = fopen(path, "r");
  if (!f) return;
  size_t n = fread(buf, 1, cap - 1, f);
  buf[n] = 0;
  fclose(f);
}

static void save_and_load(void) {
  char path[512], buf[8192];
  snprintf(path, sizeof path, "%s/a/b/elrs-wifi-joystick/config", dir);
  eng_config_t c;
  eng_config_default(&c);
  c.interval_us = 5000;
  /* nothing to write: no file made */
  CHECK_INT(cfg_save(path, &c, 0, err, sizeof err), 0);
  CHECK(access(path, F_OK) != 0);
  /* the directories are made */
  CHECK_INT(cfg_save(path, &c, CFG_K_INTERVAL, err, sizeof err), 0);
  CHECK_STR(err, "");
  read_text(path, buf, sizeof buf);
  CHECK(strstr(buf, "\ninterval_us = 5000\n") != NULL);
  struct stat st;
  CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0644);
  char d[600];
  snprintf(d, sizeof d, "%s/a/b/elrs-wifi-joystick", dir);
  CHECK(stat(d, &st) == 0 && (st.st_mode & 0777) == 0700);
  CHECK_INT(count_files(d), 1);                      /* no temp file left */
  /* load it back */
  cfg_settings_t s;
  cfg_defaults(&s);
  nwarn = 0;
  CHECK_INT(cfg_load(path, &s, on_warn, NULL), 1);
  CHECK_INT(s.eng.interval_us, 5000);
  CHECK_INT(nwarn, 0);
  /* a user's edit survives the next save; the file mode too */
  FILE *f = fopen(path, "a");
  fputs("# my note\nweb = off\n", f);
  fclose(f);
  chmod(path, 0600);
  c.channels = 10;
  CHECK_INT(cfg_save(path, &c, CFG_K_CHANNELS, err, sizeof err), 0);
  read_text(path, buf, sizeof buf);
  CHECK(strstr(buf, "\ninterval_us = 5000\n# my note\nweb = off\nchannels = 10\n") != NULL);
  CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
  cfg_defaults(&s);
  cfg_load(path, &s, on_warn, NULL);
  CHECK(!s.web);
  CHECK_INT(s.eng.channels, 10);
  /* no file: 0, defaults stay */
  char none[600];
  snprintf(none, sizeof none, "%s/none", dir);
  cfg_defaults(&s);
  CHECK_INT(cfg_load(none, &s, on_warn, NULL), 0);
  CHECK_INT(s.eng.port, 11000);
  /* a new file gets 0666 minus the umask (077 here: private), like any file the user makes */
  char priv[600];
  snprintf(priv, sizeof priv, "%s/private/config", dir);
  mode_t um = umask(077);
  CHECK_INT(cfg_save(priv, &c, CFG_K_PORT, err, sizeof err), 0);
  umask(um);
  CHECK(stat(priv, &st) == 0 && (st.st_mode & 0777) == 0600);
  /* a symlinked file is written where it points; the link stays */
  char real[600], link[600];
  snprintf(real, sizeof real, "%s/real.conf", dir);
  snprintf(link, sizeof link, "%s/link.conf", dir);
  f = fopen(real, "w");
  fputs("name = Linked\n", f);
  fclose(f);
  CHECK(symlink(real, link) == 0);
  CHECK_INT(cfg_save(link, &c, CFG_K_PORT, err, sizeof err), 0);
  CHECK(lstat(link, &st) == 0 && S_ISLNK(st.st_mode));
  read_text(real, buf, sizeof buf);
  CHECK_STR(buf, "name = Linked\nport = 11000\n");
  /* a link to a file that does not exist yet (dotfiles): the file is made there, the link stays; a relative
   * link is relative to its directory; a link to a link, too */
  char dots[600], dlink[600], dlink2[600], dfile[700];
  snprintf(dots, sizeof dots, "%s/dots", dir);
  mkdir(dots, 0700);
  snprintf(dlink, sizeof dlink, "%s/dangling.conf", dir);
  CHECK(symlink("dots/sub/elrs.conf", dlink) == 0);
  CHECK_INT(cfg_save(dlink, &c, CFG_K_PORT, err, sizeof err), 0);
  CHECK(lstat(dlink, &st) == 0 && S_ISLNK(st.st_mode));
  snprintf(dfile, sizeof dfile, "%s/sub/elrs.conf", dots);
  read_text(dfile, buf, sizeof buf);
  CHECK(strstr(buf, "\nport = 11000\n") != NULL);
  unlink(dfile);
  snprintf(dlink2, sizeof dlink2, "%s/dangling2.conf", dir);
  CHECK(symlink(dlink, dlink2) == 0);                 /* -> dangling.conf -> dots/sub/elrs.conf (gone again) */
  CHECK_INT(cfg_save(dlink2, &c, CFG_K_CHANNELS, err, sizeof err), 0);
  CHECK(lstat(dlink2, &st) == 0 && S_ISLNK(st.st_mode));
  CHECK(lstat(dlink, &st) == 0 && S_ISLNK(st.st_mode));
  read_text(dfile, buf, sizeof buf);
  CHECK(strstr(buf, "\nchannels = 10\n") != NULL);
  /* errors: a file where a directory must be, a directory where the file must be, a binary file */
  char bad[700];
  snprintf(bad, sizeof bad, "%s/real.conf/sub/config", dir);
  CHECK_INT(cfg_save(bad, &c, CFG_K_PORT, err, sizeof err), -1);
  CHECK(!strncmp(err, "cannot save the settings to ", 28));
  snprintf(bad, sizeof bad, "%s/a", dir);
  CHECK_INT(cfg_save(bad, &c, CFG_K_PORT, err, sizeof err), -1);
  nwarn = 0;
  CHECK_INT(cfg_load(bad, &s, on_warn, NULL), -1);   /* a directory: warned, defaults */
  CHECK_INT(nwarn, 1);
  CHECK(warned("cannot read the settings file"));
  snprintf(bad, sizeof bad, "%s/binary", dir);
  f = fopen(bad, "w");
  fwrite("port = 1\0\x01", 1, 10, f);
  fclose(f);
  nwarn = 0;
  cfg_defaults(&s);
  CHECK_INT(cfg_load(bad, &s, on_warn, NULL), -1);
  CHECK(warned("it is not a text file"));
  CHECK_INT(s.eng.port, 11000);
  CHECK_INT(cfg_save(bad, &c, CFG_K_PORT, err, sizeof err), -1);
  CHECK(strstr(err, "it is not a text file") != NULL);
  /* a read-only directory (root may write anyway: then it just works) */
  char ro[600], rof[700];
  snprintf(ro, sizeof ro, "%s/ro", dir);
  mkdir(ro, 0500);
  snprintf(rof, sizeof rof, "%s/config", ro);
  int r = cfg_save(rof, &c, CFG_K_PORT, err, sizeof err);
  if (geteuid() != 0) {
    CHECK_INT(r, -1);
    CHECK(strstr(err, "permission denied") || strstr(err, "Permission denied"));
  }
  CHECK_INT(count_files(ro), r == 0 ? 1 : 0);
  chmod(ro, 0700);
}

static void default_path(void) {
  char out[512];
  setenv("XDG_CONFIG_HOME", "/xdg", 1);
  CHECK_INT(cfg_default_path(out, sizeof out), 0);
  CHECK_STR(out, "/xdg/elrs-wifi-joystick/config");
  setenv("XDG_CONFIG_HOME", "relative/dir", 1);     /* ignored, as the XDG spec says */
  setenv("HOME", "/home/pilot", 1);
  CHECK_INT(cfg_default_path(out, sizeof out), 0);
  CHECK_STR(out, "/home/pilot/.config/elrs-wifi-joystick/config");
  unsetenv("XDG_CONFIG_HOME");
  CHECK_INT(cfg_default_path(out, sizeof out), 0);
  CHECK_STR(out, "/home/pilot/.config/elrs-wifi-joystick/config");
  unsetenv("HOME");
  int r = cfg_default_path(out, sizeof out);        /* from the password database, when there is one */
  CHECK(r == -1 || out[0] == '/');
  setenv("HOME", "/h", 1);
  CHECK_INT(cfg_default_path(out, 10), -1);          /* does not fit */
  CHECK_STR(out, "");
}

static void lock_path(void) {
  char out[600], run[600];
  CHECK_INT(cfg_lock_path("/run/user/1000", "/nonexistent", 1000, out, sizeof out), 0);
  CHECK_STR(out, "/run/user/1000/elrs-wifi-joystick.lock");
  /* no (or a relative) XDG_RUNTIME_DIR: the user's runtime directory when it is theirs, as in a session */
  snprintf(run, sizeof run, "%s/runuser", dir);
  mkdir(run, 0700);
  unsigned me = (unsigned)getuid();
  CHECK_INT(cfg_lock_path(NULL, run, me, out, sizeof out), 0);
  char want[700];
  snprintf(want, sizeof want, "%s/elrs-wifi-joystick.lock", run);
  CHECK_STR(out, want);
  CHECK_INT(cfg_lock_path("relative", run, me, out, sizeof out), 0);
  CHECK_STR(out, want);
  CHECK_INT(cfg_lock_path("", run, me, out, sizeof out), 0);
  CHECK_STR(out, want);
  /* the same file for an instance with XDG_RUNTIME_DIR = that directory */
  CHECK_INT(cfg_lock_path(run, "/nonexistent", me, out, sizeof out), 0);
  CHECK_STR(out, want);
  /* not there, or someone else's: /tmp */
  CHECK_INT(cfg_lock_path(NULL, "/nonexistent", me, out, sizeof out), 0);
  snprintf(want, sizeof want, "/tmp/elrs-wifi-joystick-%u.lock", me);
  CHECK_STR(out, want);
  CHECK_INT(cfg_lock_path(NULL, run, me + 1, out, sizeof out), 0);
  snprintf(want, sizeof want, "/tmp/elrs-wifi-joystick-%u.lock", me + 1);
  CHECK_STR(out, want);
  CHECK_INT(cfg_lock_path("/run/user/1000", NULL, 1000, out, 10), -1);
}

static void texts(void) {
  char buf[8192];
  FILE *f = fmemopen(buf, sizeof buf, "w");
  cfg_print_help(f);
  fclose(f);
  CHECK(strstr(buf, "Usage: elrs-wifi-joystick [options]") != NULL);
  static const char *const opts[] = { "--module", "--tx", "--rate", "--interval", "--channels", "--port", "--http-port",
                                      "--web", "--no-web", "--no-joystick", "--no-buttons", "--name", "--config",
                                      "--no-config", "--daemon", "--list-rates", "--version", "--help" };
  for (size_t i = 0; i < sizeof opts / sizeof opts[0]; i++) CHECK(strstr(buf, opts[i]) != NULL);
  /* the last line: the project, the author's GitHub profile and Ko-fi */
  static const char links[] = "https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Linux"
                              " - by juricabi: https://github.com/juricabi - Ko-fi: https://ko-fi.com/juricabi\n";
  size_t n = strlen(buf);
  CHECK(n > sizeof links && strcmp(buf + n - (sizeof links - 1), links) == 0 && buf[n - sizeof links] == '\n');
  /* --version: the first line is what install.sh and release.yml read */
  f = fmemopen(buf, sizeof buf, "w");
  cfg_print_version(f, "1.2.3-rc1");
  fclose(f);
  char want[256];
  snprintf(want, sizeof want, "elrs-wifi-joystick 1.2.3-rc1\n%s", links);
  CHECK_STR(buf, want);
  f = fmemopen(buf, sizeof buf, "w");
  cfg_print_rates(f);
  fclose(f);
  CHECK(strstr(buf, "   250 Hz    4000 us  (default)") != NULL);
  CHECK(strstr(buf, "   333 Hz    3000 us") != NULL);
  CHECK(strstr(buf, "   150 Hz    6667 us") != NULL);
  CHECK(strstr(buf, "    50 Hz   20000 us") != NULL);
  CHECK(strstr(buf, "4 to 16 (default 16)") != NULL);
  CHECK(strstr(buf, "Crossfire ignores both") != NULL);
}

int main(void) {
  const char *tmp = getenv("TMPDIR");
  snprintf(dir, sizeof dir, "%s/test_config.XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
  defaults();
  values();
  command_line();
  precedence();
  settings_text();
  render();
  diff();
  umask(022);                                        /* the modes checked below */
  save_and_load();
  default_path();
  lock_path();
  texts();
  char cmd[600];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", dir);
  T_DONE();
}

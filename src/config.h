/* Command line and settings file (DESIGN.md sections 5 and 6): parsing, validation, saving.
 * Pure except cfg_load / cfg_save / cfg_default_path (files and the environment).
 *
 * Precedence: defaults < settings file < command line. The settings file holds "key = value" lines and
 * '#' comment lines; an unknown key or a bad value is a warning and that line is ignored (and dropped
 * on the next save). The program writes only the live keys (module_ip, port, http_port, interval_us,
 * channels), and of those only the ones that changed while it ran: a value given once on the command
 * line is not made permanent by an unrelated change on the web page. Everything else in the file
 * stays as it is. */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "engine.h"

#define CFG_APP "elrs-wifi-joystick"
#define CFG_PROJECT_URL "https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Linux"
#define CFG_KOFI_URL "https://ko-fi.com/juricabi"
/* the last line of --help and --version (web/index.html's footer has the same links) */
#define CFG_LINKS CFG_PROJECT_URL " - Ko-fi: " CFG_KOFI_URL

typedef struct {
  eng_config_t eng;         /* the live settings */
  bool web;                 /* serve the web page */
  char web_addr[16];        /* IPv4 literal */
  uint16_t web_port;
  bool joystick, buttons;
  char name[64];            /* the joystick's name ("" = ENG_DEVICE_NAME) */
} cfg_settings_t;

/* which settings a source gave (command line or file) */
enum {
  CFG_K_MODULE_IP = 1 << 0, CFG_K_PORT = 1 << 1, CFG_K_HTTP_PORT = 1 << 2, CFG_K_INTERVAL = 1 << 3,
  CFG_K_CHANNELS = 1 << 4, CFG_K_WEB = 1 << 5, CFG_K_JOYSTICK = 1 << 6, CFG_K_BUTTONS = 1 << 7,
  CFG_K_NAME = 1 << 8,
  CFG_K_LIVE = CFG_K_MODULE_IP | CFG_K_PORT | CFG_K_HTTP_PORT | CFG_K_INTERVAL | CFG_K_CHANNELS,
};

typedef enum { CFG_RUN, CFG_HELP, CFG_VERSION, CFG_LIST_RATES } cfg_action_t;

typedef struct {
  cfg_action_t action;
  cfg_settings_t set;       /* values given on the command line: only those in mask count */
  unsigned mask;
  char config_path[512];    /* --config FILE ("" = the default) */
  bool no_config;
  bool daemon;
} cfg_args_t;

void cfg_defaults(cfg_settings_t *s);
/* argv -> a. 0, or -1 with one line in err naming the option and the supported values (exit code 2).
 * --help / --version / --list-rates set action and stop parsing. */
int  cfg_parse_args(int argc, char **argv, cfg_args_t *a, char *err, size_t cap);
/* the command-line values over s */
void cfg_apply_args(cfg_settings_t *s, const cfg_args_t *a);

/* ---- values (shared by the command line, the settings file and the CLI's commands) ----
 * Each returns 0, or -1 with err like "3 is not supported - use 4 to 16" (no option name). */
int  cfg_parse_int(const char *s, long min, long max, long *out);   /* strict decimal: 0 ok, -1 not a number, -2 out of range */
int  cfg_value_module_ip(const char *s, char out[64], char *err, size_t cap);   /* "" = discovery */
int  cfg_value_rate(const char *s, int *interval_us, char *err, size_t cap);    /* Hz -> interval */
int  cfg_value_interval(const char *s, int *interval_us, char *err, size_t cap);
int  cfg_value_channels(const char *s, int *channels, char *err, size_t cap);
int  cfg_value_port(const char *s, uint16_t *port, char *err, size_t cap);
int  cfg_value_web(const char *s, bool *on, char addr[16], uint16_t *port, char *err, size_t cap);  /* [ADDR:]PORT or off */
int  cfg_value_bool(const char *s, bool *v, char *err, size_t cap);              /* on/off (yes/no, true/false, 1/0) */
int  cfg_value_name(const char *s, char out[64], char *err, size_t cap);
/* a rate in Hz as the module's interval: the presets exactly (333 -> 3000, 150 -> 6667), else
 * round(1e6 / hz); 0 when hz is outside 10..1000 */
int  cfg_rate_to_interval(int hz);

/* ---- the settings file ---- */
/* $XDG_CONFIG_HOME/elrs-wifi-joystick/config (an absolute XDG_CONFIG_HOME only), else
 * $HOME/.config/...; -1 when there is no home. */
int  cfg_default_path(char *out, size_t cap);
/* The single-instance lock file: <xdg>/elrs-wifi-joystick.lock for an absolute xdg ($XDG_RUNTIME_DIR);
 * else <run_user>/elrs-wifi-joystick.lock when run_user (/run/user/<uid>) is a directory of uid - what
 * XDG_RUNTIME_DIR is in a login session, so a shell without it (su -, sudo -u, cron) finds the lock of an
 * instance started in the session or as the user service; else /tmp/elrs-wifi-joystick-<uid>.lock.
 * 0, or -1 when it does not fit. */
int  cfg_lock_path(const char *xdg, const char *run_user, unsigned uid, char *out, size_t cap);
typedef void (*cfg_warn_fn)(void *ud, const char *msg);
/* Text -> s (only the keys present change); warn gets one line per ignored line, prefixed with
 * "<fname> line N: ". Returns the CFG_K_* mask of the keys that were applied. */
unsigned cfg_parse_text(const char *text, cfg_settings_t *s, const char *fname, cfg_warn_fn warn, void *ud);
/* 1 read, 0 there is no file, -1 could not be read (warned). */
int  cfg_load(const char *path, cfg_settings_t *s, cfg_warn_fn warn, void *ud);
/* The new file text: old with the live keys in `keys` set to c's values (the first line of a key is
 * replaced, later ones dropped, missing ones appended), unknown keys and bad values dropped, the rest
 * as it was. old NULL or "" = a new file with a short header. Returns the length, -1 when it does not fit. */
int  cfg_render(const char *old, const eng_config_t *c, unsigned keys, char *out, size_t cap);
/* the CFG_K_* live keys whose values differ between a and b */
unsigned cfg_live_diff(const eng_config_t *a, const eng_config_t *b);
/* Write the live keys in `keys` (temp file + fsync + rename, directories made as needed; a symlinked
 * file is written where it points, also when that file does not exist yet; an existing file keeps its
 * mode, a new one gets 0666 minus the umask). 0, or -1 with err ("cannot save the settings to <path>: <why>"). */
int  cfg_save(const char *path, const eng_config_t *c, unsigned keys, char *err, size_t cap);

/* ---- text ---- */
void cfg_print_help(FILE *f);
void cfg_print_rates(FILE *f);
/* "elrs-wifi-joystick <version>", then CFG_LINKS. install.sh and release.yml read the first line only. */
void cfg_print_version(FILE *f, const char *version);

#endif

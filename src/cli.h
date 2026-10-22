/* The terminal side (DESIGN.md section 5), on the main thread, through engine.h only.
 *
 * Interactive (stdin and stdout are terminals, no --daemon): a header, the log lines as they come
 * (colored unless NO_COLOR or TERM=dumb), and one status line at the bottom, redrawn ~10 times a
 * second. Single keys in raw mode (restored on exit and on every signal); prompts in cooked mode.
 * Otherwise: log lines with a time, a status line every 2 s while streaming, and line commands on
 * stdin (when stdin is not a terminal, or stdout is not one) like the Windows CLI's scripted stdin.
 * --daemon: ISO timestamps, a status line every 60 s while streaming, stdin ignored. */
#ifndef CLI_H
#define CLI_H

#include <stdbool.h>
#include <stddef.h>
#include "engine.h"
#include "web.h"

typedef struct {
  engine_t *e;
  web_t *web;                 /* NULL: no web page */
  const char *web_note;       /* why there is none ("off (--no-web)", or the error) */
  bool daemon;
  const char *version;
} cli_opts_t;

/* Signal handling for the whole program: SIGINT/SIGTERM/SIGHUP ask to quit (a second one while
 * quitting ends the program at once), SIGPIPE is ignored, Ctrl+Z / fg keep the terminal sane.
 * Call once, early (before any thread starts). 0 ok. */
int  cli_signals_init(void);
bool cli_quit_requested(void);
/* Runs until quit (q, Ctrl+C, a signal, the quit command). Returns the exit code (0). */
int  cli_run(const cli_opts_t *o);

/* ---- pure parts (tests) ---- */
typedef enum {
  CLI_NONE, CLI_START, CLI_STOP, CLI_TOGGLE, CLI_TARGET, CLI_RATE, CLI_INTERVAL, CLI_CHANNELS, CLI_PORT,
  CLI_HTTP_PORT, CLI_RESTART, CLI_STATUS, CLI_WEB, CLI_HELP, CLI_QUIT, CLI_UNKNOWN
} cli_cmd_t;
/* One command line ("rate 500", "t 10.0.0.1", "Quit"): the command and its argument (trimmed). */
cli_cmd_t cli_parse_line(const char *line, char *arg, size_t cap);
/* The status as one plain line (non-interactive output and the status command). */
void cli_status_plain(const eng_snapshot_t *s, char *out, size_t cap);
/* The interactive status line: at most width-1 columns visible; utf8 = block bars, color = ANSI.
 * btn: the button marks' state (hysteresis across calls; 8 entries, updated). */
void cli_status_tty(const eng_snapshot_t *s, int width, bool utf8, bool color, bool *btn, char *out, size_t cap);
/* "2026-10-22T14:03:07.123+02:00" (daemon) or "14:03:07" for a wall time in ms */
void cli_time(int64_t wall_ms, bool iso, char *out, size_t cap);
/* the number of terminal columns a UTF-8 string with ANSI color codes takes (1 per character) */
int  cli_width(const char *s);

#endif

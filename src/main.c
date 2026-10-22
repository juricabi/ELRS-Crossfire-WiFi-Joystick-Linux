/* elrs-wifi-joystick: options, settings file, single instance, signals, and the wiring of the engine,
 * the web page and the terminal UI (DESIGN.md sections 3, 5, 6). */
#include "cli.h"
#include "config.h"
#include "engine.h"
#include "web.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef APP_VERSION
#define APP_VERSION "0.0.0-dev"
#endif

/* exit codes (DESIGN.md section 5) */
#define EXIT_BAD_OPTIONS 2
#define EXIT_RUNNING 3

/* ---------------------------------------------------------------- settings file warnings */
#define MAXWARN 32
static char warns[MAXWARN][512];
static int nwarn;
static void collect_warning(void *ud, const char *msg) {
  (void)ud;
  if (nwarn < MAXWARN) snprintf(warns[nwarn], sizeof warns[0], "%s", msg);
  nwarn++;
}

/* ---------------------------------------------------------------- saving */
/* Only what changed while running is written: base = the settings as they were at the start (or
 * the last save), so a one-off command-line value is not made permanent by an unrelated change. */
typedef struct {
  char path[PATH_MAX];
  eng_config_t base;
} saver_t;

static int save_settings(void *ud, const eng_config_t *c, char *err, size_t cap) {
  saver_t *s = ud;
  unsigned keys = cfg_live_diff(&s->base, c);
  if (!keys) return 0;
  if (cfg_save(s->path, c, keys, err, cap) != 0) return -1;
  s->base = *c;
  return 0;
}

/* ---------------------------------------------------------------- one instance */
/* -1: could not tell (warned, go on); -2: another one runs (message printed); else the lock's fd,
 * held until exit (the kernel lets go of it when the process ends, however it ends). */
static int instance_lock(void) {
  char path[PATH_MAX], run_user[64];
  unsigned uid = (unsigned)getuid();
  int fd = -1;
  /* $XDG_RUNTIME_DIR, else /run/user/<uid> (what it is in a session), else /tmp: the same file for an
   * instance with and one without XDG_RUNTIME_DIR (su -, sudo -u, cron next to the user service) */
  snprintf(run_user, sizeof run_user, "/run/user/%u", uid);
  if (cfg_lock_path(getenv("XDG_RUNTIME_DIR"), run_user, uid, path, sizeof path) == 0)
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    /* no usable runtime directory: one lock per user in /tmp. O_NOFOLLOW: /tmp is shared, a planted
     * symlink must not point the lock (and its truncation) elsewhere */
    snprintf(path, sizeof path, "/tmp/elrs-wifi-joystick-%u.lock", uid);
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  }
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) {
    fprintf(stderr, "elrs-wifi-joystick: warning: cannot use the lock file %s (%s) - not checking for a second "
            "instance\n", path, fd < 0 ? strerror(errno) : "not a file of this user");
    if (fd >= 0) close(fd);
    return -1;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    if (errno != EWOULDBLOCK) {
      fprintf(stderr, "elrs-wifi-joystick: warning: cannot lock %s (%s) - not checking for a second instance\n", path,
              strerror(errno));
      close(fd);
      return -1;
    }
    char pid[32] = "";
    ssize_t n = pread(fd, pid, sizeof pid - 1, 0);
    pid[n > 0 ? n : 0] = 0;
    pid[strcspn(pid, "\r\n")] = 0;
    bool digits = pid[0] != 0;
    for (const char *p = pid; *p; p++) digits = digits && *p >= '0' && *p <= '9';
    fprintf(stderr, "elrs-wifi-joystick is already running%s%s%s - only one may run (both would want the module's "
            "stream and the joystick). Stop it first.\n", digits ? " (process " : "", digits ? pid : "", digits ? ")" : "");
    close(fd);
    return -2;
  }
  char pid[32];
  int len = snprintf(pid, sizeof pid, "%ld\n", (long)getpid());
  if (ftruncate(fd, 0) != 0 || pwrite(fd, pid, (size_t)len, 0) != len) { /* the pid is only a courtesy */ }
  return fd;
}

/* fds 0-2 closed at the start (some init scripts and daemonizers do that): /dev/null there, or the next
 * file opened (the lock) would become stdin or stdout - its pid read as a command, the output written in it */
static void std_fds_open(void) {
  for (int fd = 0; fd < 3; fd++) {
    if (fcntl(fd, F_GETFD) >= 0 || errno != EBADF) continue;
    int n = open("/dev/null", fd == 0 ? O_RDONLY : O_WRONLY);
    if (n >= 0 && n != fd) {
      dup2(n, fd);
      close(n);
    }
  }
}

int main(int argc, char **argv) {
  std_fds_open();
  cfg_args_t a;
  char err[600];
  if (cfg_parse_args(argc, argv, &a, err, sizeof err) != 0) {
    fprintf(stderr, "elrs-wifi-joystick: %s\n", err);
    return EXIT_BAD_OPTIONS;
  }
  switch (a.action) {
    case CFG_HELP: cfg_print_help(stdout); return 0;
    case CFG_VERSION: printf("elrs-wifi-joystick %s\n", APP_VERSION); return 0;
    case CFG_LIST_RATES: cfg_print_rates(stdout); return 0;
    case CFG_RUN: break;
  }

  /* defaults < settings file < command line */
  cfg_settings_t s;
  cfg_defaults(&s);
  static saver_t saver;
  bool have_path = false;
  if (!a.no_config) {
    if (a.config_path[0]) {
      snprintf(saver.path, sizeof saver.path, "%s", a.config_path);
      have_path = true;
    } else {
      have_path = cfg_default_path(saver.path, sizeof saver.path) == 0;
      if (!have_path) collect_warning(NULL, "no home directory: the settings are not saved (use --config FILE)");
    }
    if (have_path) cfg_load(saver.path, &s, collect_warning, NULL);
  }
  cfg_apply_args(&s, &a);

  int lock = instance_lock();
  if (lock == -2) return EXIT_RUNNING;
  if (cli_signals_init() != 0) {
    fprintf(stderr, "elrs-wifi-joystick: cannot set up the signal handling: %s\n", strerror(errno));
    return 1;
  }

  eng_options_t o;
  memset(&o, 0, sizeof o);
  o.joystick = s.joystick;
  o.buttons = s.buttons;
  snprintf(o.device_name, sizeof o.device_name, "%s", s.name);
  if (have_path) {
    saver.base = s.eng;
    o.save = save_settings;
    o.save_ud = &saver;
  }
  engine_t *e = eng_create(&s.eng, &o, err, sizeof err);
  if (!e) {
    fprintf(stderr, "elrs-wifi-joystick: %s\n", err);
    return 1;
  }
  for (int i = 0; i < nwarn && i < MAXWARN; i++) eng_log(e, "!! Settings: %s", warns[i]);
  if (nwarn > MAXWARN) eng_log(e, "!! Settings: %d more warnings", nwarn - MAXWARN);

  web_t *web = NULL;
  char web_note[300] = "it is turned off (--no-web, or web = off in the settings file)";
  if (s.web) {
    web = web_start(e, s.web_addr, s.web_port, web_note, sizeof web_note);
    if (web) {
      char url[160];
      web_url(web, url, sizeof url);
      eng_log(e, "Web page: %s", url);
    } else {
      eng_log(e, "!! No web page: %s", web_note);
    }
  }

  cli_opts_t co = { e, web, web_note, a.daemon, APP_VERSION };
  int code = cli_run(&co);
  web_stop(web);
  eng_destroy(e);
  if (lock >= 0) close(lock);
  return code;
}

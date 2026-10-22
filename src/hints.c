/* Firewall and uinput advice: see hints.h. */
#include "hints.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static bool blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

bool hints_ufw_enabled(const char *t) {
  bool yes = false;
  while (t && *t) {
    const char *nl = strchr(t, '\n');
    const char *s = t, *end = nl ? nl : t + strlen(t);
    t = nl ? nl + 1 : end;
    while (s < end && blank(*s)) s++;
    for (const char *c = s; c < end; c++) if (*c == '#') { end = c; break; }
    while (end > s && blank(end[-1])) end--;
    if (end - s < 8 || strncmp(s, "ENABLED", 7)) continue;
    const char *v = s + 7;
    while (v < end && blank(*v)) v++;
    if (v >= end || *v != '=') continue;
    v++;
    while (v < end && blank(*v)) v++;
    if (end - v >= 2 && (*v == '"' || *v == '\'') && end[-1] == *v) { v++; end--; }
    /* the last ENABLED line wins, as for the shell that sources this file */
    yes = end - v == 3 && !strncasecmp(v, "yes", 3);
  }
  return yes;
}

static bool exists(const char *root, const char *path) {
  char p[512];
  snprintf(p, sizeof p, "%s%s", root, path);
  struct stat st;
  return stat(p, &st) == 0;
}

hints_fw_t hints_detect_firewall(const char *root) {
  if (!root || !*root || !strcmp(root, "/")) root = "";
  char p[512], buf[4096];
  snprintf(p, sizeof p, "%s/etc/ufw/ufw.conf", root);
  int fd = open(p, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    if (hints_ufw_enabled(buf)) return HINTS_FW_UFW;
  }
  /* firewalld writes a pid file while it runs (its location moved between versions) */
  static const char *const pids[] = { "/run/firewalld.pid", "/var/run/firewalld.pid",
                                      "/run/firewalld/firewalld.pid", "/var/run/firewalld/firewalld.pid" };
  for (size_t i = 0; i < sizeof pids / sizeof pids[0]; i++)
    if (exists(root, pids[i])) return HINTS_FW_FIREWALLD;
  return HINTS_FW_UNKNOWN;
}

int hints_firewall(hints_fw_t fw, unsigned port, char *out, size_t cap) {
  int n;
  switch (fw) {
    case HINTS_FW_UFW:
      n = snprintf(out, cap, "The ufw firewall is on and may drop the joystick data - let it in: sudo ufw allow %u/udp",
                   port);
      break;
    case HINTS_FW_FIREWALLD:
      n = snprintf(out, cap, "firewalld is running and may drop the joystick data - let it in: "
                   "sudo firewall-cmd --permanent --add-port=%u/udp && sudo firewall-cmd --reload", port);
      break;
    default:
      n = snprintf(out, cap, "If this machine runs a firewall, it must let UDP port %u in, e.g. "
                   "sudo iptables -I INPUT -p udp --dport %u -j ACCEPT", port, port);
      break;
  }
  return n < 0 ? 0 : (size_t)n >= cap ? (int)(cap ? cap - 1 : 0) : n;
}

void hints_uinput(int err, const char *path, const char *user, char *out, size_t cap) {
  if (!path || !*path) path = "/dev/uinput";
  if (!user || !*user) user = "$USER";
  switch (err) {
    case ENOENT:
      snprintf(out, cap, "%s does not exist - load the uinput kernel module: sudo modprobe uinput "
               "(packaging/install.sh loads it at every boot and gives you access)", path);
      break;
    case EACCES:
    case EPERM:
      /* the rule's uaccess covers only the user at the machine's screen: over SSH, or for a service
       * started at boot, access comes from the input group (the fix the installer prints too) */
      snprintf(out, cap, "no permission to use %s - run the install script with sudo (its udev rule gives access); "
               "over SSH or for a service also: sudo usermod -aG input %s, log in again (this boot only: "
               "sudo setfacl -m u:%s:rw %s)", path, user, user, path);
      break;
    case ENODEV:
    case ENXIO:
      snprintf(out, cap, "%s has no driver - load it: sudo modprobe uinput (if that fails, this kernel was built "
               "without uinput)", path);
      break;
    default:
      snprintf(out, cap, "cannot open %s: %s", path, strerror(err));
      break;
  }
}

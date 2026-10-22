/* hints: firewall detection and advice, uinput advice. The firewall probes run against a fake root
 * directory made here. */
#include "tests/t.h"
#include "hints.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[256];

static void put(const char *rel, const char *text) {
  char p[512];
  snprintf(p, sizeof p, "%s%s", root, rel);
  /* mkdir -p of the parent */
  for (char *s = p + strlen(root) + 1; *s; s++) {
    if (*s != '/') continue;
    *s = 0;
    mkdir(p, 0755);
    *s = '/';
  }
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0);
  if (fd < 0) return;
  CHECK((size_t)write(fd, text, strlen(text)) == strlen(text));
  close(fd);
}

static void del(const char *rel) {
  char p[512];
  snprintf(p, sizeof p, "%s%s", root, rel);
  unlink(p);
}

static void ufw_conf_parsing(void) {
  CHECK(hints_ufw_enabled("ENABLED=yes\n"));
  CHECK(hints_ufw_enabled("ENABLED=yes"));
  CHECK(hints_ufw_enabled("# /etc/ufw/ufw.conf\n#\n\n# Set to yes to start on boot.\nENABLED=yes\n\nLOGLEVEL=low\n"));
  CHECK(!hints_ufw_enabled("# Set to yes to start on boot.\nENABLED=no\nLOGLEVEL=low\n"));
  CHECK(hints_ufw_enabled("  ENABLED = yes  \n"));
  CHECK(hints_ufw_enabled("ENABLED=\"yes\"\n"));
  CHECK(hints_ufw_enabled("ENABLED='yes'\n"));
  CHECK(hints_ufw_enabled("ENABLED=YES\r\n"));
  CHECK(hints_ufw_enabled("ENABLED=yes # comment\n"));
  CHECK(!hints_ufw_enabled("#ENABLED=yes\n"));
  CHECK(!hints_ufw_enabled("ENABLED=yes\nENABLED=no\n"));         /* the last one wins */
  CHECK(hints_ufw_enabled("ENABLED=no\nENABLED=yes\n"));
  CHECK(!hints_ufw_enabled("ENABLEDX=yes\n"));
  CHECK(!hints_ufw_enabled("ENABLED=yesno\n"));
  CHECK(!hints_ufw_enabled("ENABLED=\n"));
  CHECK(!hints_ufw_enabled("ENABLED yes\n"));
  CHECK(!hints_ufw_enabled("ENABLED=\"yes\n"));
  CHECK(!hints_ufw_enabled(""));
  CHECK(!hints_ufw_enabled(NULL));
  CHECK(!hints_ufw_enabled("XENABLED=yes"));
}

static void detect(void) {
  const char *tmp = getenv("TMPDIR");
  snprintf(root, sizeof root, "%s/test_hints.XXXXXX", tmp && *tmp ? tmp : "/tmp");
  CHECK(mkdtemp(root) != NULL);
  CHECK_INT(hints_detect_firewall(root), HINTS_FW_UNKNOWN);         /* nothing there */
  put("/etc/ufw/ufw.conf", "ENABLED=no\n");
  CHECK_INT(hints_detect_firewall(root), HINTS_FW_UNKNOWN);
  put("/etc/ufw/ufw.conf", "ENABLED=yes\n");
  CHECK_INT(hints_detect_firewall(root), HINTS_FW_UFW);
  put("/run/firewalld.pid", "123\n");
  CHECK_INT(hints_detect_firewall(root), HINTS_FW_UFW);             /* ufw first: it says it is on */
  put("/etc/ufw/ufw.conf", "ENABLED=no\n");
  CHECK_INT(hints_detect_firewall(root), HINTS_FW_FIREWALLD);
  del("/run/firewalld.pid");
  CHECK_INT(hints_detect_firewall(root), HINTS_FW_UNKNOWN);
  put("/var/run/firewalld.pid", "1\n");
  CHECK_INT(hints_detect_firewall(root), HINTS_FW_FIREWALLD);
  del("/var/run/firewalld.pid");
  put("/run/firewalld/firewalld.pid", "1\n");
  CHECK_INT(hints_detect_firewall(root), HINTS_FW_FIREWALLD);
  del("/run/firewalld/firewalld.pid");
  put("/var/run/firewalld/firewalld.pid", "1\n");
  CHECK_INT(hints_detect_firewall(root), HINTS_FW_FIREWALLD);
  del("/var/run/firewalld/firewalld.pid");
  /* an unreadable ufw.conf is no ufw */
  del("/etc/ufw/ufw.conf");
  char d[512];
  snprintf(d, sizeof d, "%s/etc/ufw/ufw.conf", root);
  mkdir(d, 0755);                                                     /* a directory: read fails */
  CHECK_INT(hints_detect_firewall(root), HINTS_FW_UNKNOWN);
  rmdir(d);
  /* the real system: whatever it is, it must not crash ("" and "/" are the same) */
  hints_fw_t a = hints_detect_firewall(NULL), b = hints_detect_firewall(""), c = hints_detect_firewall("/");
  CHECK(a == b && b == c);
  char cmd[600];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  CHECK(system(cmd) == 0);
}

static void firewall_text(void) {
  char out[300];
  int n = hints_firewall(HINTS_FW_UFW, 11000, out, sizeof out);
  CHECK_INT(n, (int)strlen(out));
  CHECK(strstr(out, "sudo ufw allow 11000/udp") != NULL);
  hints_firewall(HINTS_FW_FIREWALLD, 12345, out, sizeof out);
  CHECK(strstr(out, "sudo firewall-cmd --permanent --add-port=12345/udp && sudo firewall-cmd --reload") != NULL);
  hints_firewall(HINTS_FW_UNKNOWN, 11000, out, sizeof out);
  CHECK(strstr(out, "UDP port 11000") != NULL);
  CHECK(strstr(out, "sudo iptables -I INPUT -p udp --dport 11000 -j ACCEPT") != NULL);
  /* a small buffer: cut, terminated, the length of what fits */
  char small[10];
  n = hints_firewall(HINTS_FW_UFW, 11000, small, sizeof small);
  CHECK_INT(n, 9);
  CHECK_INT((int)strlen(small), 9);
  for (size_t i = 0; i < strlen(out); i++) CHECK(out[i] >= 0x20 && out[i] < 0x7f);
}

static void uinput_text(void) {
  char out[400];
  hints_uinput(ENOENT, NULL, "pilot", out, sizeof out);
  CHECK(strstr(out, "/dev/uinput does not exist") != NULL);
  CHECK(strstr(out, "sudo modprobe uinput") != NULL);
  CHECK(strstr(out, "packaging/install.sh") != NULL);
  hints_uinput(EACCES, NULL, "pilot", out, sizeof out);
  CHECK(strstr(out, "no permission to use /dev/uinput") != NULL);
  CHECK(strstr(out, "run the install script with sudo") != NULL);       /* a release has install.sh */
  CHECK(strstr(out, "packaging/") == NULL);
  /* over SSH or for a service the udev rule's uaccess does not apply: the input group does */
  CHECK(strstr(out, "sudo usermod -aG input pilot, log in again") != NULL);
  CHECK(strstr(out, "sudo setfacl -m u:pilot:rw /dev/uinput") != NULL);
  /* a 16-character user name still fits the engine's 256-byte joy_error whole */
  char big[1024];
  hints_uinput(EACCES, NULL, "abcdefghijklmnop", big, sizeof big);
  CHECK(strlen(big) < 256);
  hints_uinput(EPERM, "/tmp/x", NULL, out, sizeof out);
  CHECK(strstr(out, "sudo setfacl -m u:$USER:rw /tmp/x") != NULL);
  hints_uinput(EACCES, "", "", out, sizeof out);
  CHECK(strstr(out, "u:$USER:rw /dev/uinput") != NULL);
  hints_uinput(ENODEV, NULL, NULL, out, sizeof out);
  CHECK(strstr(out, "sudo modprobe uinput") != NULL);
  hints_uinput(ENXIO, NULL, NULL, out, sizeof out);
  CHECK(strstr(out, "has no driver") != NULL);
  hints_uinput(EBUSY, NULL, NULL, out, sizeof out);
  CHECK(strstr(out, "cannot open /dev/uinput: ") != NULL);
  char small[8];
  hints_uinput(EACCES, NULL, "pilot", small, sizeof small);
  CHECK_INT((int)strlen(small), 7);
}

int main(void) {
  ufw_conf_parsing();
  detect();
  firewall_text();
  uinput_text();
  T_DONE();
}

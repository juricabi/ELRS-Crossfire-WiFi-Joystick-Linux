/* Linux-specific advice for the two things that most often stop the joystick: a firewall that drops
 * the module's UDP stream, and no access to /dev/uinput. Pure text builders plus small file probes
 * (never called on the receive thread). */
#ifndef HINTS_H
#define HINTS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { HINTS_FW_UNKNOWN, HINTS_FW_UFW, HINTS_FW_FIREWALLD } hints_fw_t;

/* Which firewall is active: ufw when <root>/etc/ufw/ufw.conf says ENABLED=yes, firewalld when its pid
 * file exists under <root>/run or <root>/var/run, else UNKNOWN. root NULL or "" = "/" (tests pass a
 * directory). */
hints_fw_t hints_detect_firewall(const char *root);
/* ufw.conf contents: true when it has ENABLED=yes (quotes and spaces allowed, '#' comments) */
bool hints_ufw_enabled(const char *conf_text);
/* One line: what to run so UDP port reaches this program, for that firewall. Returns the length. */
int  hints_firewall(hints_fw_t fw, unsigned port, char *out, size_t cap);
/* Why the virtual joystick can't be made and what to run, for an open() errno on path (NULL =
 * "/dev/uinput"). user: the login name for the setfacl example (NULL or "" = "$USER"). */
void hints_uinput(int err, const char *path, const char *user, char *out, size_t cap);

#endif

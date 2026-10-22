---
name: Problem report
about: Something doesn't work with elrs-wifi-joystick on Linux
labels: bug
---

**What happened, and what did you expect?**


**The machine**
- Distribution and version (`cat /etc/os-release`, first lines):
- Architecture (`uname -m`):
- Kernel (`uname -r`):
- elrs-wifi-joystick version (`elrs-wifi-joystick --version`):
- How you run it (in a terminal, as the systemd service, other options you use):
- How you installed it (install.sh with or without sudo, a downloaded binary, built from source):
- Desktop session at the screen, or SSH / no desktop:

**The module**
- ExpressLRS: TX module and firmware version (e.g. RadioMaster Ranger, ELRS 3.5.3):
- or TBS Crossfire / Tracer: TX firmware (e.g. 6.36) and WiFi module firmware (e.g. v2.25):
- Rate and channels asked for (the web page, or `--rate` / `--channels`):

**The joystick** (if the problem is with a game or simulator)
- The game, and whether it runs natively, through Steam / Proton, Flatpak or Snap:
- `ls -l /dev/uinput` and `id` (your groups):
- The line about the joystick from `cat /proc/bus/input/devices` (the block named "ELRS Crossfire WiFi Joystick"):

**The log**
In the terminal it is what the program prints; for the service: `journalctl --user -u elrs-wifi-joystick --since -10min`.
The web page shows it too (Log). Remove anything private (WiFi names, IP addresses you don't want to share).

```
paste the log here
```

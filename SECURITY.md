# Security

## What to expect from elrs-wifi-joystick

`elrs-wifi-joystick` is meant for a trusted home network (or the module's own access point). The WiFi
joystick protocol of the modules has no authentication, so the program trusts the local network. Know
these defaults:

- **The web page has no login.** By default it listens on `127.0.0.1:11000`, so only this machine can
  open it. With `--web 0.0.0.0:11000` (or another address that is not loopback) anyone on your network
  can open it, change the settings (also the module IP and HTTP port that the program's requests go to)
  and start, stop or restart the stream. Its checks (`Host`, `Origin`, and `Content-Type: application/json`
  for every change) keep other web sites in your browser out (cross-site requests, DNS rebinding), not
  people on your network. See [docs/USAGE.md](docs/USAGE.md#the-web-page) and
  [docs/WEB_API.md](docs/WEB_API.md).
- **UDP port 11000 listens on every network interface.** Any host on the network can announce itself as
  a module (a beacon), and the program then asks it to stream; any host can send channel data and drive
  the virtual joystick while no other module holds the lock. `--module IP` limits both to that one
  address (UDP source addresses are not authenticated, so this keeps other modules out, not an attacker
  on your network). Opening UDP 11000 in a firewall, as the program suggests, lets the network reach it.
- **The requests to a module are plain HTTP.** Asking a module to stream is an HTTP `POST /udpcontrol`
  to the module's IP (port 80 unless `--http-port` says otherwise), as the module firmware requires. It
  carries only the rate and the channel count. A stop, quit or port change also POSTs
  `action=joystick_end` (nothing else) to the module streaming to the program, to any address that sent
  it channel data in the last 3 s (with `--module`, only one it asked, streamed from or heard a beacon from)
  and to one asked to stream less than 3 s ago, so the module broadcasts again; never to a module known as
  Crossfire.
- **Access to `/dev/uinput`.** The udev rule that `install.sh` installs gives `/dev/uinput` to the user
  logged in at the machine's screen and to the members of the `input` group. Access to `/dev/uinput`
  lets that user's programs create input devices, keyboards included, and members of the `input` group
  can also read every input device (every keystroke). Add users to the `input` group only on machines
  and for accounts you trust; at the screen the rule's `uaccess` is enough.
- **The install script** downloads only from this repository's GitHub releases over HTTPS and checks
  every download against the release's `SHA256SUMS` (that catches a broken download, not a compromised
  release). A `GITHUB_TOKEN`, if you give one, goes to curl on stdin and only to `api.github.com`.
  Nothing runs as root after the install: the program and its systemd user service run as your user,
  the service with `NoNewPrivileges`.
- The settings file (`~/.config/elrs-wifi-joystick/config`) holds no passwords.

## Supported versions

Fixes go into the latest release. Update by running the install script again the way you installed it:
`sudo sh install.sh` for the default install, `sh install.sh` (without sudo) only for a `~/.local/bin`
install (see [docs/INSTALL.md](docs/INSTALL.md#updating)).

## Reporting a vulnerability

Please don't open a public issue for a security problem. Use GitHub's private vulnerability reporting:
"Report a vulnerability" under the repository's **Security** tab
([new report](https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Linux/security/advisories/new)).
Please include:

- the version (`elrs-wifi-joystick --version`), the distribution and the architecture (`uname -m`),
- how it was started (terminal, `--daemon` or the service; its options, `--web` in particular),
- what the problem is, and the steps to reproduce it.

You'll get an answer as soon as possible.

Out of scope: the module firmware (ExpressLRS, TBS Crossfire / Tracer) and the protocol's lack of
authentication, which this program cannot change; please report firmware problems to those projects.

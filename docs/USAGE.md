# Usage reference

Everything `elrs-wifi-joystick` takes: command-line options, keys, stdin commands, the settings file,
the environment, exit codes, the service and the install script. [README.md](../README.md) is the
overview; [DESIGN.md](DESIGN.md) explains how it works; [WEB_API.md](WEB_API.md) is the web page's API.

## Command line

```
elrs-wifi-joystick [options]
```

| option | value | default | |
|---|---|---|---|
| `-m`, `--module`, `--tx` | IP | (none) | The module's IPv4 address. Without it the module is found by its broadcast; with it only that IP counts, and it is asked to stream at once. |
| `-r`, `--rate` | Hz | 250 | Ask the module for this many updates per second. The presets 1000 500 333 250 200 150 100 50 map to exact intervals (333 to 3000 µs, 150 to 6667 µs); any other 10..1000 becomes round(1000000 / Hz). |
| `-i`, `--interval` | µs | 4000 | The same as an interval: 1000..100000. With `--rate`, the later of the two wins. |
| `-c`, `--channels` | N | 16 | Channels to ask for: 4..16. |
| `-p`, `--port` | N | 11000 | UDP port to listen on (the modules always send to 11000). |
| `--http-port` | N | 80 | The module's web server port (for the activation request). |
| `-w`, `--web` | [ADDR:]PORT | 127.0.0.1:11000 | The web page's address. ADDR is an IPv4 literal or `localhost`; a PORT alone means 127.0.0.1:PORT. `0.0.0.0:11000` makes it reachable from the network (it has no password). |
| `--no-web` | | | No web page. |
| `--no-joystick` | | | Monitor only: no virtual joystick. |
| `--no-buttons` | | | CH9..CH16 are not buttons. |
| `--name` | NAME | ELRS Crossfire WiFi Joystick | The joystick's name: 1 to 63 characters, no control characters. |
| `--config` | FILE | see [Settings file](#settings-file) | The settings file. |
| `--no-config` | | | Read and write no settings file. |
| `--daemon` | | | No keys and no status line: log lines with ISO timestamps (for systemd). |
| `--list-rates` | | | Print the supported rates and channel counts, then exit. |
| `-V`, `--version` | | | Print the version, then exit. |
| `-h`, `--help` | | | Print the help, then exit. |

- Long options take their value as `--rate 500` or `--rate=500`; short ones as `-r 500` or `-r500`, and
  flags can be combined (`-hV`). `--` ends the options; nothing may follow it.
- Numbers are plain decimal (spaces around them are ignored). An unsupported value is a one-line error
  that names the option and the supported values, with exit code 2, e.g.
  `elrs-wifi-joystick: --channels: 3 is not supported - use 4 to 16`.
- Precedence: built-in defaults, then the settings file, then the command line. A value given on the
  command line counts for that run only: it is not written to the settings file unless you change it
  while the program runs.

## The terminal

**Interactive** (stdin and stdout are terminals, no `--daemon`): a header, the log lines as they come
(colored unless `NO_COLOR` is set or `TERM=dumb`), and one status line at the bottom, redrawn about 10
times a second: the state, the module, the measured rate and jitter, the request (rate x channels), 8
bars for CH1..CH8 and marks for the 8 buttons. Keys (upper or lower case):

| key | |
|---|---|
| `s` | start / stop listening |
| `t` | prompt for the module's IP; empty = find it by its broadcast |
| `r` | prompt for the rate (the presets, or 10..1000 Hz) |
| `c` | prompt for the channels (4..16) |
| `p` | prompt for the UDP port |
| `a` | restart the stream: send our request again (ELRS: `joystick_end`, then `joystick_begin`; Crossfire: nothing is sent) |
| `w` | print the web page's address |
| `h`, `?` | help |
| `q`, Ctrl+C | quit |

In a prompt, Enter alone keeps the value (for `t` it means "find it by its broadcast"), and Esc then
Enter cancels. The terminal is restored on exit, on every signal and around Ctrl+Z.

**Not interactive** (stdin or stdout is not a terminal): log lines with the time, and a plain status
line every 2 s while streaming, for example

```
14:03:07  streaming from ELRS 192.168.1.50 (Unified TX) | 249.8 Hz, jitter 0.42 ms | asked 250 Hz x 16 ch | CH1-8 % 50 50 0 50 0 0 0 0 | buttons X-------
```

Commands are then read from stdin, one per line (case does not matter, a UTF-8 byte order mark before
the first one is ignored, an unknown command prints an error line). The end of stdin ends the commands,
not the program, so `elrs-wifi-joystick < /dev/null` keeps running.

| command | |
|---|---|
| `start`, `stop`, `s` | listen, stop, toggle |
| `t IP`, `module IP` | the module's IP; `t` alone: find it by its broadcast |
| `rate HZ`, `r HZ` | the rate |
| `interval US`, `i US` | the rate as an interval |
| `channels N`, `c N` | the channels |
| `port N`, `p N` | the UDP port |
| `http-port N` | the module's web server port |
| `restart`, `a` | restart the stream |
| `status` | print one status line now |
| `web`, `w` | print the web page's address |
| `help`, `h`, `?` | the list of commands |
| `quit`, `q`, `exit` | quit |

**`--daemon`**: log lines only, each with an ISO 8601 timestamp, a status line every 60 s while
streaming, and stdin is not read. This is what the service runs.

## Settings file

`$XDG_CONFIG_HOME/elrs-wifi-joystick/config` (an absolute `XDG_CONFIG_HOME` only), else
`~/.config/elrs-wifi-joystick/config`; `--config FILE` uses another file, `--no-config` none.

```
# a comment
module_ip =
interval_us = 4000
channels = 16
web = 127.0.0.1:11000
```

| key | value | default |
|---|---|---|
| `module_ip` | dotted IPv4, or empty (find the module by its broadcast) | empty |
| `port` | 1..65535: the UDP port | 11000 |
| `http_port` | 1..65535: the module's web server port | 80 |
| `interval_us` | 1000..100000: the request's interval (4000 = 250 Hz) | 4000 |
| `channels` | 4..16: the request's channels | 16 |
| `web` | `[ADDR:]PORT`, or `off` | 127.0.0.1:11000 |
| `joystick` | `on` or `off` (also yes/no, true/false, 1/0) | on |
| `buttons` | `on` or `off` | on |
| `name` | the joystick's name (in `"quotes"` to keep spaces at its ends) | ELRS Crossfire WiFi Joystick |

- One `key = value` per line; lines starting with `#` are comments; spaces around keys and values do
  not matter. An unknown key or a bad value is a warning in the log, and that line is ignored (and
  left out the next time the file is written); it never stops the program.
- The program writes only the first five keys, and only those you changed while it ran (keys, stdin
  commands, web page); every other line stays as it is. A new file starts with a short explanation.
- Writes are atomic (a temporary file next to it, flushed to disk, then renamed); the folder is
  created when needed, a symlinked file is written where the link points, and the file keeps its mode.
  If the file cannot be written, the change still applies and the log says why it was not saved.

## Environment

| variable | |
|---|---|
| `XDG_CONFIG_HOME`, `HOME` | where the settings file is (above) |
| `XDG_RUNTIME_DIR` | where the single-instance lock is: `$XDG_RUNTIME_DIR/elrs-wifi-joystick.lock`, else `/tmp/elrs-wifi-joystick-<uid>.lock` |
| `NO_COLOR`, `TERM` | no colors when `NO_COLOR` is set or `TERM` is `dumb` |
| `LC_ALL`, `LC_CTYPE`, `LANG` | a UTF-8 locale gets block-character bars in the status line |

## Exit codes and signals

| code | |
|---|---|
| 0 | normal end (`q`, `quit`, Ctrl+C, SIGTERM, SIGHUP) |
| 1 | something fatal (e.g. a thread could not start) |
| 2 | bad command-line options |
| 3 | another copy already runs for this user (both would need UDP port 11000 and the joystick) |

SIGINT, SIGTERM and SIGHUP end the program cleanly: the joystick is centered and removed, the socket
closed. A second signal while it is quitting ends it at once. SIGPIPE is ignored.

## The web page

`http://127.0.0.1:11000/` by default: the sticks and buttons live, the rate and jitter, the module, the
request (rate and channels, with the supported values), the module IP and ports, start / stop /
restart, and the log. It polls the program 10 times a second while it is visible and loads nothing
from the internet. Changes apply at once and are saved like the keys' changes.

It accepts requests only for `localhost`, `127.0.0.1`, `[::1]` or an IPv4 address in the `Host`
header, refuses other sites' pages (`Origin`), and answers changes only to `application/json`
requests, so other web pages in your browser cannot use it. With `--web 0.0.0.0:11000` anyone on your
network can open it and change the settings: there is no password. The API is in [WEB_API.md](WEB_API.md).

## The service

`packaging/elrs-wifi-joystick.service` is a systemd **user** unit that runs
`elrs-wifi-joystick --daemon` (restarted 5 s after a failure, but not after exit codes 2 and 3).
`install.sh --service` installs it (system-wide in `/etc/systemd/user/` with sudo, or in
`~/.config/systemd/user/` for a `~/.local/bin` install) and enables it for one user.

```sh
systemctl --user status elrs-wifi-joystick
systemctl --user stop|start|restart elrs-wifi-joystick
systemctl --user disable --now elrs-wifi-joystick
journalctl --user -u elrs-wifi-joystick -f
sudo loginctl enable-linger $USER       # start it at boot, without a login
```

A service started at boot has no desktop session, so `/dev/uinput` access comes from the `input` group
(`sudo usermod -aG input $USER`), not from the udev rule's `uaccess`.

## install.sh

```
sh install.sh [options]
```

| option | |
|---|---|
| `--version X` | install release X (`1.2.3` or `v1.2.3`; default: the latest) |
| `--file PATH` | install this binary instead of downloading one |
| `--user` | install for this user only, in `~/.local/bin` (the default without root; with sudo it is refused - run it as the user) |
| `--service` | the systemd user service, for the user who ran sudo (or you, without sudo); enabled, and started when that user has a running systemd user session (else at the next login) |
| `--no-udev` | leave `/dev/uinput` access and the uinput module alone |
| `--uninstall` | remove what this script installed |
| `--arch A` | the binary for A instead of this machine's: `x86_64 i686 aarch64 armv7 armv6 riscv64` |
| `--root DIR` | install into DIR instead of / (images, tests): nothing is loaded, reloaded or started, the commands are printed instead |
| `-n`, `--dry-run` | only print what would be done |
| `-h`, `--help` | help |

| environment | |
|---|---|
| `GITHUB_TOKEN` | a token that can read the repository: needed while it is private (with curl) |
| `ELRS_WIFI_JOYSTICK_REPO` | `owner/name` to install from (default `juricabi/elrs-crossfire-wifi-joystick-linux`) |
| `TMPDIR` | where the download is checked before it is installed |

What it installs, as root: `/usr/local/bin/elrs-wifi-joystick`,
`/etc/udev/rules.d/60-elrs-wifi-joystick.rules`, `/etc/modules-load.d/elrs-wifi-joystick-uinput.conf`,
with `--service` `/etc/systemd/user/elrs-wifi-joystick.service`, and its list of these in
`/usr/local/share/elrs-wifi-joystick/installed`. Without root: `~/.local/bin/elrs-wifi-joystick`, with
`--service` `~/.config/systemd/user/elrs-wifi-joystick.service`, and the list in
`~/.local/share/elrs-wifi-joystick/installed`.

- The architecture comes from `uname -m`: `x86_64`/`amd64`, `i386`..`i686`, `aarch64`/`arm64`,
  `armv7l`/`armv8l` (a 32-bit system on a 64-bit ARM CPU), `armv6l`, `riscv64`. Anything else stops
  with a message (build from source then).
- A download is checked against the release's `SHA256SUMS` (with `sha256sum` or `shasum -a 256`), and
  the program must run here (`--version`) before it replaces anything. Files are installed atomically
  (a temporary file next to the target, then renamed). Downloads use curl, else wget (wget only for a
  public repository).
- Running it again updates the program (and the unit), and restarts a running service.
- `--uninstall` stops and disables the service, removes the files on its list (and nothing else, even
  when the list was edited), and reloads udev. Your settings (`~/.config/elrs-wifi-joystick/`) stay;
  `/dev/uinput` keeps its current access until the next reboot.
- Exit codes: 0 done, 1 failed (nothing was installed when the download or its check failed),
  2 bad options.

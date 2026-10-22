# Design

`elrs-wifi-joystick` turns the WiFi joystick stream of an ExpressLRS or TBS Crossfire TX module into a
Linux joystick (uinput), with a terminal UI and a web page. It is the Linux sibling of
[ELRS-Crossfire-WiFi-Joystick-Windows](https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Windows)
(same protocol, same behaviour, same axis order) and shares its protocol core with
[sticktime-pi](https://github.com/juricabi/sticktime-pi).

Goals, in order: correct, low latency, runs on every common Linux machine as one static file,
easy to set up, easy to update.

## 1. Layout

```
core/                 the shared protocol core (C, MIT): the source of truth for sticktime-pi too
  wifi_proto.c/.h     protocol engine: pure logic (beacons, activation, source lock, Crossfire
                      placeholders, stats, the request and restarts, activation queue)
  wifi_net.c/.h       activation POST client (blocking, for a worker thread), socket helpers
  tests/              t.h, test_wifi_proto.c, test_wifi_net.c
src/                  the Linux program
  main.c              options, settings file, single instance, signals, wiring, the run loop
  config.c/.h         command line + settings file parsing/validation/saving (pure where possible)
  engine.c/.h         receive thread + worker thread + snapshot + commands + log ring (CONTRACT: engine.h)
  joymap.c/.h         pure: channels -> axis/button values and the events to emit
  uinput.c/.h         the virtual joystick device
  cli.c/.h            terminal UI (interactive keys + status line, or line commands on stdin)
  web.c/.h            HTTP server + JSON API (CONTRACT: web.h, docs/WEB_API.md)
  hints.c/.h          Linux-specific advice (firewall, uinput access), pure + small file probes
web/index.html        the page (self-contained: inline CSS/JS, no external requests), embedded at build
tests/                unit tests (test_*.c), fake_elrs.py (fake module), e2e tests (*.py)
packaging/            install.sh, udev rule, modules-load file, systemd user unit
docs/                 DESIGN.md (this), WEB_API.md, USAGE.md, INSTALL.md, TROUBLESHOOTING.md, DEVELOPMENT.md
build.sh              native + static cross builds (zig cc, musl), tests
```

`core/` is vendored unmodified by sticktime-pi (`host/third_party/elrs-wifi/`). Any change to it must
keep it free of Linux-app specifics, keep compiling on Windows (mingw / zig `x86_64-windows-gnu`) and
macOS, and keep `core/tests` passing; sticktime-pi then re-vendors it.

## 2. Protocol (what the core does)

See the header comment of `core/wifi_proto.h`. In short:

- UDP 11000 receives beacons (`ELRS...` every 5 s while the ELRS module is idle; `VELOCIDRONE...` about
  every 8 s from Crossfire, also while streaming) and channel frames (`u8 type, u8 count (4..16), count x
  u16le 0..32767`).
- Activation: `POST /udpcontrol` with `action=joystick_begin&interval=<us>&channels=<n>`; the module
  streams to the IP that posted. One POST per module per 5 s, on a worker thread.
- **The request** (interval and channels) is a setting everywhere. Supported: interval 1000..100000 µs
  (presets 1000/500/333/250/200/150/100/50 Hz; default 4000 = 250 Hz), channels 4..16 (default 16).
  ELRS applies it only while not streaming, so changing it while an ELRS module streams restarts the
  stream (`joystick_end`, then `joystick_begin`). Crossfire ignores it (16 channels, ~90 Hz) and, once
  its beacon says Crossfire, is never sent `joystick_end`; a locked 16-channel module with no beacon yet
  is restarted only after 20 s of lock without a Crossfire beacon. A module that is surely not Crossfire
  and streams a channel count other than the request is restarted once per session (another app's
  leftover request); a leftover rate is not detected (startup values apply at the next joystick_begin).
- Source lock: the first module with real channel data; others are ignored until it is silent for 3 s.
  With a module IP set, only that IP counts.
- Crossfire placeholder frames (any channel = 0xF26A) and all-out-of-range frames are dropped.

## 3. Threads and data flow

```
            UDP 11000                          uinput
module ───────────────▶ receive thread ────────────────▶ /dev/input/eventN ──▶ games / sims
   ▲                     │  wifi_proto (owner)
   │ POST /udpcontrol    │  joystick (owner)
   └──── worker thread ◀─┘  activation queue (mutex)
                         │
                         ▼ snapshot + log ring (mutex)        commands (mutex + wake pipe)
                    CLI (main thread) · web thread  ─────────────────────────────▶ receive thread
```

- **Receive thread** (engine.c): `poll()` on the UDP socket and a wake pipe, timeout = time to the
  next tick (≤ 100 ms). On data: drain the socket (`recvmsg` with `MSG_DONTWAIT`, kernel receive
  timestamps via `SO_TIMESTAMPNS`, at most 256 datagrams), `wifi_proto_packet` each; every applied frame
  goes to the joystick at once. Then: activation results, commands, `wifi_proto_tick`, stats, bind
  retry, snapshot. No allocation, no file I/O, no blocking calls in this loop (logging = copy into the
  ring under a mutex).
- **Worker thread**: takes requests from the `wifi_actq_t`, runs `wifi_net_post` (5 s timeout per
  POST, abort when the session changes or on shutdown), puts results back, wakes the receive thread.
- **CLI** (main thread) and **web** (its own thread) only use `engine.h`.
- Bind: UDP `INADDR_ANY:port`, no `SO_REUSEADDR` (a second instance must fail), 256 KiB receive buffer,
  `FD_CLOEXEC`. Bind failures: 5 tries 300 ms apart, then state Error "UDP port N in use" and a try
  every 5 s.
- Single instance: an exclusive `flock` on `$XDG_RUNTIME_DIR/elrs-wifi-joystick.lock` (else
  `/tmp/elrs-wifi-joystick-<uid>.lock`); a second instance exits with a clear message (exit code 3).
- Signals: SIGINT/SIGTERM/SIGHUP end the program cleanly (joystick centered and removed); SIGPIPE ignored.

## 4. The virtual joystick (uinput)

- Name `ELRS Crossfire WiFi Joystick` (configurable), `BUS_VIRTUAL`, vendor 0x0000, product 0x0000,
  version 1. (No real vendor/product: a borrowed ID could make SDL apply some gamepad's mapping;
  SDL then identifies it by name, which never changes.)
- Axes, the order of the Windows app's vJoy mapping, so bindings carry over:
  CH1 `ABS_X`, CH2 `ABS_Y`, CH3 `ABS_RX`, CH4 `ABS_RY`, CH5 `ABS_RZ`, CH6 `ABS_Z`, CH7 `ABS_THROTTLE`,
  CH8 `ABS_RUDDER`. Range 0..32767, **fuzz 0, flat 0** (the kernel must not drop small moves or add a
  dead zone), resolution 0, initial value 16384 (center).
- Buttons: CH9..CH16 = `BTN_TRIGGER`..`BTN_BASE2` (0x120..0x127, "joystick buttons 1-8", like the
  ELRS BLE joystick). Pressed at raw ≥ 24576 (75 %), released at raw < 20480 (62.5 %): hysteresis, so
  a switch sitting near a threshold never chatters. With `--no-buttons` the buttons are still declared
  but never pressed: udev and SDL take a device with X/Y/Z axes and no `EV_KEY` for an accelerometer,
  not a joystick. A channel the frame does not carry (fewer channels asked for, or sent) is not driven:
  its axis goes to the center and its button is released, so nothing stays held from a longer frame.
- Each applied frame = one `write()` of the changed axes/buttons + `SYN_REPORT` (nothing when nothing
  changed). The kernel timestamps events at that write.
- When the lock is released (3 s of silence), on stop, and at exit: every axis to 16384, every button
  released (what vJoy's ResetVJD does in the Windows app). Short gaps hold the last values.
- `/dev/uinput` missing or not writable: not fatal. The program runs as a monitor (CLI + web still
  show the sticks) and says exactly why and what to run (`sudo modprobe uinput`, the udev rule from
  `packaging/`, or the install script). Device node discovery: after `UI_DEV_CREATE`, `UI_GET_SYSNAME`
  → `/sys/devices/virtual/input/<name>/` → the `eventN` entry → `/dev/input/eventN` (best effort).

## 5. Command line

```
elrs-wifi-joystick [options]
  -m, --module IP       the module's IP (default: find it by its broadcast); also --tx
  -r, --rate HZ         ask the module for HZ updates per second: 1000 500 333 250 200 150 100 50
                        (default 250); any 10..1000 is accepted
  -i, --interval US     the same as an interval in microseconds: 1000..100000 (default 4000)
  -c, --channels N      channels to ask for: 4..16 (default 16)
  -p, --port N          UDP port to listen on (default 11000)
      --http-port N     the module's web server port (default 80)
  -w, --web [ADDR:]PORT web page address (default 127.0.0.1:11000); 0.0.0.0:11000 = reachable from
                        the network
      --no-web          no web page
      --no-joystick     monitor only: no virtual joystick
      --no-buttons      CH9-CH16 are not buttons
      --name NAME       the joystick's name (default "ELRS Crossfire WiFi Joystick")
      --config FILE     settings file (default $XDG_CONFIG_HOME/elrs-wifi-joystick/config)
      --no-config       don't read or write a settings file
      --daemon          no keys and no status line: log lines only, with timestamps (systemd)
      --list-rates      print the supported rates and channel counts, then exit
  -V, --version         print the version, then exit
  -h, --help            this help, then exit
```

- Precedence: defaults < settings file < command line. Invalid values: a one-line error naming the
  option and the supported values, exit code 2. `--rate` and `--interval` together: the later one wins.
- Interactive (stdin and stdout are terminals, in the foreground, not `--daemon`): a header, then log
  lines as they come (colored when `TERM` is set and not `dumb` and `NO_COLOR` is empty or unset), and
  one status line at the bottom
  redrawn ~10 times a second: state, module, rate/jitter, asked rate × channels, and 8 small bars
  (CH1-8) + CH9-16 button marks. Keys (raw mode, restored on exit and on every signal):
  `s` start/stop, `t` module IP (prompt; empty = discovery), `r` rate (prompt listing the presets),
  `c` channels (prompt 4-16), `p` UDP port, `a` restart the stream (re-send our request), `w` print the
  web address, `h`/`?` help, `q` quit (also Ctrl+C). Prompts use cooked mode with echo; Esc/empty input
  cancels where noted.
- Not interactive (stdin or stdout not a terminal, or `--daemon`): log lines only, plus a status line
  every 2 s while streaming (like the Windows CLI when redirected); `--daemon` adds ISO timestamps
  to every line and prints the status line only every 60 s. Line commands on stdin (when stdin is not
  a terminal and not `--daemon`), like the Windows CLI's scripted stdin: `start`, `stop`, `s`
  (toggle), `t <ip>` / `t` (discovery), `rate <hz>`, `interval <us>`, `channels <n>`, `port <n>`,
  `restart`, `status` (prints one status line), `q`/`quit`; a UTF-8 BOM before the first command is
  ignored; unknown commands print an error line. EOF on stdin ends the command reading, not the
  program (so `cmd < /dev/null` keeps running).
- Settings changed through the keys, stdin commands or the web page are saved to the settings file
  (unless `--no-config`): temp file + fsync + rename, directory created as needed.
- Exit codes: 0 normal, 2 bad command-line options, 3 already running, 1 anything else fatal.

## 6. Settings file

`$XDG_CONFIG_HOME/elrs-wifi-joystick/config` (default `~/.config/...`): `key = value` lines, `#`
comments; an unknown key or a bad value is a warning (that line is ignored), never fatal, and is
dropped on the next save. Keys: `module_ip`, `port`, `http_port`, `interval_us`, `channels`, `web` (`ADDR:PORT` or
`off`), `joystick` (`on`/`off`), `buttons` (`on`/`off`), `name`. The live keys (first five) are
rewritten on every change; the others are kept as they are.

## 7. Web page and API

Default `http://127.0.0.1:11000/` (this machine only). The contract is `docs/WEB_API.md`; summary:

- `GET /` the page; `GET /api/status`, `GET /api/config`, `POST /api/config`, `POST /api/start`,
  `POST /api/stop`, `POST /api/restart`, `GET /api/log?since=N`.
- Security: `Host` must be `localhost`, `127.0.0.1`, `[::1]` or an IPv4 literal (with an optional
  port), else 403 (DNS rebinding). An `Origin` header, when present, must name the same host:port as
  `Host` (else 403). POSTs need `Content-Type: application/json` (else 415): a cross-site form can't
  send that without a CORS preflight, which is never answered. No CORS headers. Limits: head 8 KiB,
  body 4 KiB, 32 connections, 5 s idle per connection, `Connection: close` after each response.
- The page polls `/api/status` every 100 ms while visible and `/api/log` every second; it never loads
  anything from the internet.

## 8. Builds

`./build.sh native|x86_64|i686|aarch64|armv7|armv6|riscv64|all [all|prog|tests|test|clean]`.
Static targets use `zig cc` (0.15.x) with musl: one file per architecture, no runtime dependencies,
runs on any distribution and C library. `web/index.html` is embedded as `build/<t>/gen/webpage.h`.
Tests that replace a module with their own stubs say so in a first-line comment `/* STUBS: engine */`
(object names), and build.sh leaves those objects out of that test.

| target   | for                                                        |
|----------|------------------------------------------------------------|
| x86_64   | PCs, laptops, Steam Deck                                   |
| i686     | old 32-bit PCs                                             |
| aarch64  | Raspberry Pi 3/4/5 (64-bit OS, and 32-bit OS on a 64-bit kernel), ARM SBCs, ARM laptops |
| armv7    | Raspberry Pi 2/3/4 with a 32-bit OS, most 32-bit ARM boards |
| armv6    | Raspberry Pi 1 and Zero / Zero W                           |
| riscv64  | RISC-V boards (VisionFive, Milk-V, ...)                    |

## 9. Tests

- `core/tests`: protocol (every C# engine test, ported) and the POST client.
- `tests/test_*.c`: joymap, config/options, hints, engine over loopback (fake module in-process, no
  uinput), web (HTTP parsing, security checks, JSON, with a stub engine), uinput event encoding.
- `tests/e2e.py` (stdlib Python 3.8+): the real binary with `--no-joystick` against `fake_elrs.py`
  modules on 127.0.0.2/127.0.0.3: discovery, streaming, rate change → restart, Crossfire, module IP,
  stop/start, CLI stdin commands, settings file, web API, single instance, signals.
- `tests/uinput_e2e.py`: with `/dev/uinput` (CI: `sudo modprobe uinput`): the device appears with the
  right name, axes and ranges, follows the fake module's sticks, centers on lock loss, disappears at exit.
- `tests/test_ui.py` (Playwright, headless Chromium): the page against the real binary + fake module.
- Every unit test on every architecture under qemu-user in CI.

## 10. Install and update

`packaging/install.sh`: detects the architecture (`uname -m`; `armv6l`/`armv7l`/`aarch64`/...),
downloads the matching binary from the GitHub release (or uses `--file`), checks it against
`SHA256SUMS`, installs it to `/usr/local/bin` (root) or `~/.local/bin`, installs the udev rule
(`/dev/uinput` for the logged-in user and the `input` group) and `uinput` in
`/etc/modules-load.d/`, loads the module, and optionally (`--service`) a systemd user unit. Running it
again updates; `--uninstall` removes everything it installed. A private repository needs
`GITHUB_TOKEN`.

## 11. Conventions

C (gnu99), `-Wall -Wextra` clean (CI uses `-Werror`), no allocation in the receive loop, every file
write is temp + fsync + rename, never block the receive thread. User-facing text is plain English and
says what to do. Match the style of `core/` (comments say why, not what).

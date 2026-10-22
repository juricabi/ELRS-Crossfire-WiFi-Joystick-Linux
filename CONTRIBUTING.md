# Contributing to ELRS / Crossfire WiFi Joystick for Linux

Thanks for helping. Reports from real hardware are the most valuable thing: which modules, firmware
versions, distributions, machines and simulators work, and which don't.

## Reporting a problem

Open an issue (the "Problem report" template asks for all of this):
- the version (`elrs-wifi-joystick --version`), the distribution and the architecture (`uname -m`),
- how it was started (in a terminal, `--daemon` or the systemd service) and its options,
- the module and its firmware (ExpressLRS version, or the Crossfire TX and WiFi module firmware),
- what you expected and what happened,
- the log: the terminal, the web page's Log, or `journalctl --user -u elrs-wifi-joystick` (remove WiFi
  names or IP addresses you don't want to share).

Crossfire / Tracer with "no stick data": check the
[firmware table](docs/TROUBLESHOOTING.md#crossfire-firmware-compatibility-important) first; some TBS
firmware versions never send stick data, and no program can fix that.

Security problems: see [SECURITY.md](SECURITY.md), not a public issue. Problems with the Windows app
belong to [ELRS-Crossfire-WiFi-Joystick-Windows](https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Windows/issues),
problems with the Raspberry Pi simulator to [sticktime-pi](https://github.com/juricabi/sticktime-pi/issues).

## Pull requests

1. Read [CLAUDE.md](CLAUDE.md) (the project's rules), [docs/DESIGN.md](docs/DESIGN.md) (how it works)
   and [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) (building and every test suite).
2. Keep changes focused; one topic per pull request. For a big change, open an issue first.
3. Run the tests that cover your change, at least `SAN=1 WERROR=1 ./build.sh native test` and
   `python3 tests/e2e.py`; for the web page also `python3 tests/test_ui.py` (Playwright); for the
   virtual joystick `python3 tests/uinput_e2e.py` (needs `/dev/uinput`); for the installer or the
   scripts `sh tests/test_install.sh` and `shellcheck build.sh packaging/install.sh tests/test_install.sh`.
   Mention in the pull request what you ran, and what you tested with a real module (which one, which
   firmware, which machine).
4. C code: gnu99, `-Wall -Wextra` clean (CI builds with `WERROR=1`). Nothing between the UDP socket and
   the joystick may block, allocate or do file I/O (the receive thread); file writes are a temporary
   file, fsync, then rename. Messages for users are plain English and say what to do.
5. `core/` (and `tests/fake_elrs.py`) is shared with [sticktime-pi](https://github.com/juricabi/sticktime-pi),
   which vendors it unmodified: keep it free of anything specific to this Linux program, and it must
   keep compiling on Windows and macOS (CI's `core` job) with its tests passing. See
   [the shared core](docs/DEVELOPMENT.md#the-shared-core).
6. Behaviour stays in step with the
   [Windows app](https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Windows): the axis order,
   the defaults (250 Hz, 16 channels), CH9-CH16 = buttons 1-8, and the rules for the request
   ([docs/USAGE.md](docs/USAGE.md#the-request-rate-and-channels)). A change there should go into both.
7. A new option or setting goes into `src/config.c` (and `src/engine.h`/`src/engine.c` for an engine
   setting) and [docs/USAGE.md](docs/USAGE.md), and, when the web page shows it, into `src/web.c`,
   `web/index.html` and [docs/WEB_API.md](docs/WEB_API.md).
8. `packaging/install.sh` carries copies of the udev rule, the modules-load file and the service unit in
   `packaging/`: change both (`tests/test_install.sh` compares them byte for byte).

By contributing you agree that your contribution is licensed under the MIT License, like the rest of
the project.

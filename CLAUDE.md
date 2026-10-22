# ELRS / Crossfire WiFi Joystick for Linux

`elrs-wifi-joystick` turns the WiFi joystick stream of an ExpressLRS or TBS Crossfire TX module into a
Linux joystick (uinput), with a terminal UI and a local web page. One static binary per architecture
(x86_64, i686, aarch64, armv7, armv6, riscv64). The owner (juricabi) also wrote the Windows app
(github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Windows, `JoystickEngine.cs` = the reference
behaviour) and sticktime-pi (github.com/juricabi/sticktime-pi), which vendors `core/`.

Open source (MIT): no secrets or personal paths in git. Your own notes go in `.local/` (ignored).

## Read first

- `docs/DESIGN.md`: layout, threads, protocol summary, joystick mapping, CLI, settings, builds, tests.
- `docs/WEB_API.md`: the web API contract. `src/engine.h`, `src/web.h`: the module contracts.
- `core/wifi_proto.h`: the protocol and "the request" (interval/channels, restarts, Crossfire).

## Rules

- `core/` is shared with sticktime-pi (vendored unmodified to `host/third_party/elrs-wifi/`). Keep it
  free of Linux-app specifics, compiling on Linux, Windows (`zig cc -target x86_64-windows-gnu`) and
  macOS, and its tests passing. After changing it, sticktime-pi must be re-vendored.
- Low latency: nothing between the UDP socket and the uinput write but wifi_proto. Never block, allocate
  or do file I/O on the receive thread.
- C gnu99, `-Wall -Wextra` clean (CI: `WERROR=1`). File writes: temp + fsync + rename.
- Behaviour follows the Windows app unless DESIGN.md says otherwise; keep the two in step (axis order,
  defaults: 250 Hz, 16 channels; CH9-16 = buttons 1-8).

## Build and test

```
./build.sh native test                 # unit tests (SAN=1 for ASan/UBSan, WERROR=1 like CI)
./build.sh all                         # static binaries in build/<arch>/
RUN=qemu-aarch64-static ./build.sh aarch64 test
python3 tests/e2e.py                   # the real binary against fake modules (no hardware)
```

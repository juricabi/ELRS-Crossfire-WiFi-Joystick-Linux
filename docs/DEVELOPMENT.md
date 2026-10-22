# Development

Building, testing, CI, releases, and the protocol core shared with sticktime-pi. [DESIGN.md](DESIGN.md)
explains how the program works, what each test covers and the [conventions](DESIGN.md#11-conventions);
[INSTALL.md](INSTALL.md#building-from-source) is the short version of building for users.

## Building

```
./build.sh [target] [what] [test names...]
```

- target: `native` (the default: this machine, with `cc`), or a static binary made with `zig cc`:
  `x86_64`, `i686`, `aarch64`, `armv7`, `armv6`, `riscv64`; `all` = every static target.
- what: `all` (the default: the program and the tests), `prog`, `tests`, `test` (build both, then run
  the unit tests; test names such as `test_joymap` limit which), `clean`.
- The output: `build/<target>/elrs-wifi-joystick` and `build/<target>/test_*`.

| environment | |
|---|---|
| `CC` | the compiler for `native` (default `cc`); the static targets always use `zig cc` |
| `OPT` | optimization flags (default `-O2`) |
| `WERROR=1` | warnings are errors (CI) |
| `SAN=1` | a native build with AddressSanitizer + UBSan (tests) |
| `VERSION` | the version compiled in (default: the `VERSION` file) |
| `RUN` | a prefix for running cross-built tests (e.g. `qemu-aarch64-static`); empty = run them directly |
| `BUILD` | the output directory (default `build`) |

A native build needs a C compiler with the Linux kernel headers ([INSTALL.md](INSTALL.md#building-from-source)
lists the packages). The static release binaries are made with [zig](https://ziglang.org/) 0.15.x as
the cross compiler: `./build.sh all prog` builds all six into `build/<arch>/`.

## Tests

```sh
./build.sh native test                       # unit tests (SAN=1: AddressSanitizer + UBSan; WERROR=1 like CI)
python3 tests/e2e.py                         # the program against fake modules (no hardware)
python3 tests/uinput_e2e.py                  # the virtual joystick through the kernel (needs /dev/uinput)
python3 tests/test_ui.py                     # the web page in headless Chromium (Playwright)
sh tests/test_install.sh                     # install.sh in a fake root with a fake GitHub
./build.sh all                               # the six static binaries (zig cc), in build/<arch>/
RUN="qemu-aarch64-static" ./build.sh aarch64 test    # unit tests of another architecture
```

- **Unit tests**: `core/tests/test_*.c` and `tests/test_*.c`, e.g. `./build.sh native test test_joymap`
  for one.
- **`tests/e2e.py [-v] [case ...]`**: the real binary (`--no-joystick`) against fake modules on
  127.0.0.2 and 127.0.0.3; Python standard library only. `BIN` = the binary (default
  `build/native/elrs-wifi-joystick`), `RUN` = a prefix like build.sh's
  (`RUN=qemu-aarch64-static BIN=build/aarch64/elrs-wifi-joystick`).
- **`tests/uinput_e2e.py [--require] [-v]`**: needs write access to `/dev/uinput` and read access to
  the new `/dev/input/eventN`; without them it prints SKIP and exits 0, unless `--require` (CI). `BIN`
  as above.
- **`tests/test_ui.py [--demo | --real] [-k name]`**: against the real program with fake modules when
  it is built, else against `tests/web_demo.c` (src/web.c with a fake engine), which it builds itself;
  `--demo` / `--real` force one. Environment: `BIN`, `CC` (for the demo), `CHROMIUM` (a Chromium
  binary, when Playwright's own download is missing), `HEADED=1` (watch it).
- **`tests/test_install.sh [-v]`**: no root and no network (fake system commands, a fake GitHub). Every
  case runs under each shell found among sh, dash, bash --posix and busybox sh (`SHELLS="dash"` picks;
  a busybox sh with its own `id`/`uname` applets only gets a smoke test). A pass with busybox's tools
  (sed, awk, grep, ...) runs when a busybox is installed; `BUSYBOX=/path/to/busybox` picks one.

`tests/fake_elrs.py` is the fake module the tests use; it also runs as a program, as a fake module
next to a real machine (its header comment has the options). `tests/web_demo.c` serves the real page
with a fake engine, for working on the page (how to build and drive it: its header comment).

## CI

GitHub Actions (`.github/workflows/build.yml`) runs all of it on every push and pull request:

| job | |
|---|---|
| native | the unit tests with gcc and clang under ASan and UBSan (warnings are errors), then `tests/e2e.py` |
| uinput | the virtual joystick through a real kernel, with the udev rule installed by `install.sh`, then the systemd user service and `--uninstall` |
| static | the six static binaries, with every unit test under qemu-user on an old CPU of each target (and natively for x86_64 and i686); uploaded as artifacts |
| web | the web page in Chromium, against the program and against the demo |
| core | `core/` compiled for Windows and macOS |
| scripts | shellcheck, the installer tests, actionlint, Python syntax |

## Releases

A tag `vX.Y.Z` that matches `VERSION` publishes a release with the six binaries, `install.sh` and
`SHA256SUMS` (`.github/workflows/release.yml`). It runs all of build.yml first; a version with a `-`
(`1.1.0-rc1`) is published as a pre-release, and running it again for the same tag replaces the assets
and the notes.

1. Set the new version in `VERSION`, commit it and push.
2. Tag that commit and push the tag: `git tag v1.1.0 && git push origin v1.1.0`.

## The shared core

`core/` (`wifi_proto`, `wifi_net` and their tests) is shared with
[sticktime-pi](https://github.com/juricabi/sticktime-pi), which vendors it unmodified into
`host/third_party/elrs-wifi/`, together with `tests/fake_elrs.py` (as `host/tests/fake_elrs.py`: keep
the two copies the same). A change to `core/` must keep it free of Linux-app specifics, compiling on
Linux, Windows and macOS (CI's `core` job), and its tests passing. Then vendor it again:

1. Commit the change here, after `./build.sh native test` (also with `SAN=1 WERROR=1`).
2. In sticktime-pi, checked out next to this repo: `tools/vendor_elrs_wifi.sh`, then
   `host/build.sh native test` and `python host/tests/wifi_live_test.py`, and commit
   `host/third_party/elrs-wifi/` and `host/tests/fake_elrs.py` (if it changed). sticktime-pi's CI fails
   when those files differ from what its `VENDOR` file recorded; its `host/third_party/elrs-wifi/README.md`
   has the steps in full.

# ExpressLRS / TBS Crossfire WiFi Joystick for Linux

**Use your ExpressLRS or TBS Crossfire / Tracer radio as a wireless joystick on Linux.**

`elrs-wifi-joystick` turns the WiFi joystick stream of an ExpressLRS (ELRS) or TBS Crossfire / Tracer
TX module into a Linux joystick, so you can fly simulators and use anything else that takes joystick
input, without a cable. It is the Linux sibling of
[ELRS-Crossfire-WiFi-Joystick-Windows](https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Windows):
same protocol, same behaviour, same axis order, so bindings carry over. Its protocol core (`core/`) is
shared with [sticktime-pi](https://github.com/juricabi/sticktime-pi), the Raspberry Pi FPV simulator.

## Features

- **Virtual joystick** (uinput): 8 axes in the Windows app's vJoy order, CH9-CH16 as 8 buttons, raw values.
- **ELRS and Crossfire / Tracer**, found automatically by their broadcast, or by a module IP you give.
- **Rate and channels**: you choose what the module is asked for (10 to 1000 Hz, 4 to 16 channels).
- **Single-source lock**: with two modules on the network, only one drives the joystick.
- **Terminal UI**: live stick bars, rate and jitter, single-key commands; line commands on stdin; `--daemon`.
- **Web page** on this machine: the sticks live, the settings, the log. It loads nothing from the internet.
- **Settings file** that the keys, the stdin commands and the web page keep up to date.
- **One static binary per architecture**: no dependencies, any distribution.
- **Install script**: the right binary, checked against `SHA256SUMS`, `/dev/uinput` access, an optional service.

## Machines

| binary    | for                                                                                     |
|-----------|-----------------------------------------------------------------------------------------|
| `x86_64`  | PCs, laptops, Steam Deck                                                                |
| `i686`    | old 32-bit PCs (Pentium Pro class CPU or newer)                                         |
| `aarch64` | Raspberry Pi 3/4/5 with a 64-bit OS (or a 32-bit OS on a 64-bit kernel), ARM boards, ARM laptops |
| `armv7`   | Raspberry Pi 2/3/4 with a 32-bit OS, most 32-bit ARM boards                             |
| `armv6`   | Raspberry Pi 1 and Zero / Zero W                                                        |
| `riscv64` | RISC-V boards (VisionFive, Milk-V, ...)                                                 |

Each one is a single static file (musl libc), so it runs on any distribution and C library. Other
machines: see [Building](#building).

## Quick start

1. Install it (as root the script also sets up access to `/dev/uinput`; every option and the other ways
   are in [docs/INSTALL.md](docs/INSTALL.md)):
   ```sh
   curl -fsSLO https://github.com/juricabi/elrs-crossfire-wifi-joystick-linux/releases/latest/download/install.sh
   sudo sh install.sh
   ```
   While the repository is private, the downloads need a `GITHUB_TOKEN`: see
   [docs/INSTALL.md](docs/INSTALL.md#while-the-repository-is-private).
2. Put the module on the same WiFi network as this machine (or join this machine to the module's own
   access point). The module's WiFi is 2.4 GHz only.
3. Run `elrs-wifi-joystick`. It finds the module by its broadcast, asks it to stream, and the virtual
   joystick follows your sticks. The status line at the bottom shows the module, the rate and the channels.
4. Open <http://127.0.0.1:11000/> to watch the sticks and change the settings, or use the keys (`h` lists them).
5. In the simulator, pick the joystick named **ELRS Crossfire WiFi Joystick** and calibrate it.

> **Crossfire / Tracer:** some TBS firmware versions break the WiFi joystick entirely. See the
> [compatibility table](docs/TROUBLESHOOTING.md#crossfire-firmware-compatibility-important).

## Rate and channels

The program asks the module for a rate and a channel count: by default **250 Hz and 16 channels**, the
same as the Windows app. The presets are 1000 500 333 250 200 150 100 50 Hz; any rate from 10 to 1000 Hz
is accepted, and 4 to 16 channels. Changing them with the keys `r` / `c` or the web page restarts a
streaming ELRS module so that the change takes effect (after up to 20 s if the module's kind is not known
yet). `--rate` / `--channels` and the settings file only set the next request: if the module is still
streaming from before, press `a` (or restart in the web page) to apply them. Crossfire / Tracer ignores
both: it always sends 16 channels at about 90 Hz. Details in
[docs/USAGE.md](docs/USAGE.md#the-request-rate-and-channels).

## The joystick

| channel | axis / button in SDL and `/dev/input/js*` | Windows app (vJoy) | usual (AETR) |
|---|---|---|---|
| CH1 | axis 0 | X | roll |
| CH2 | axis 1 | Y | pitch |
| CH3 | axis 3 | RX | throttle |
| CH4 | axis 4 | RY | yaw |
| CH5 | axis 5 | RZ | AUX1 |
| CH6 | axis 2 | Z | AUX2 |
| CH7 | axis 6 | Slider 0 | AUX3 |
| CH8 | axis 7 | Slider 1 | AUX4 |
| CH9-CH16 | buttons 0-7 ("buttons 1-8") | - | AUX5-AUX12 |

The axes carry the module's raw values (0 to 32767); a button is pressed from 75 %. The Linux codes,
the buttons' hysteresis and what happens when the module goes quiet: [docs/USAGE.md](docs/USAGE.md#the-joystick).

## Building

For other machines, or to change the code, [build it from source](docs/INSTALL.md#building-from-source):
it needs a C compiler and the Linux kernel headers. The static release builds, the tests and CI are in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Documentation

- [docs/USAGE.md](docs/USAGE.md): the command line, the keys, the stdin commands, the web page, the
  settings file, the request and the joystick in detail.
- [docs/INSTALL.md](docs/INSTALL.md): the install script and its options, installing by hand, the systemd
  user service, updating, uninstalling, building from source.
- [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md): Crossfire firmware, `/dev/uinput` access, no module
  or no data (firewalls), two modules, games, Steam and Proton.
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md): building, the tests, CI, releases, the shared core.
- [docs/DESIGN.md](docs/DESIGN.md): how it works.
- [docs/WEB_API.md](docs/WEB_API.md): the web page's API.

## Support

If you like it, you can buy me a coffee: [Ko-fi](https://ko-fi.com/juricabi)

## License

MIT - see [LICENSE](LICENSE).

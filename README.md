# ExpressLRS / TBS Crossfire WiFi Joystick for Linux

**Use your ExpressLRS or TBS Crossfire / Tracer radio as a wireless joystick on Linux.**

`elrs-wifi-joystick` turns the WiFi joystick stream of an ExpressLRS (ELRS) or TBS Crossfire / Tracer
TX module into a Linux joystick, so you can fly simulators and use anything else that takes joystick
input, without a cable. It is the Linux sibling of
[ELRS-Crossfire-WiFi-Joystick-Windows](https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Windows):
same protocol, same behaviour, same axis order, so bindings carry over. Its protocol core (`core/`) is
shared with [sticktime-pi](https://github.com/juricabi/sticktime-pi), the Raspberry Pi FPV simulator.

Both radios speak the same "WiFi joystick" protocol (the one VelociDrone Mobile uses): put the module
on your WiFi network and run the program. ELRS announces itself with an `ELRS` beacon, Crossfire with
`VELOCIDRONE`; the program handles both.

## Features

- **Virtual joystick** through the kernel's uinput: 8 axes in the Windows app's vJoy order, and
  CH9-CH16 as 8 buttons (like the ELRS Bluetooth joystick). Raw values, no scaling, no filtering, no
  dead zone.
- **ELRS and Crossfire / Tracer**, found automatically by their broadcast, or by a module IP you give.
- **The request**: you choose the rate (1000 to 50 Hz) and the channel count (4 to 16) the module is
  asked for; a streaming ELRS module is restarted so a change takes effect.
- **Single-source lock**: with two modules on the network, only one drives the joystick.
- **Terminal UI**: live stick bars, rate and jitter, single-key commands; scriptable with line
  commands on stdin; a quiet `--daemon` mode for services.
- **Web page** on this machine (`http://127.0.0.1:11000/`): the sticks and buttons live, rate and
  jitter, settings, start/stop, the log. It loads nothing from the internet.
- **Settings file** that the keys, the stdin commands and the web page keep up to date.
- **One static binary per architecture**: no dependencies, any distribution. Without access to
  `/dev/uinput` it still runs, as a monitor, and says what to do.
- **Install script**: picks the right binary, checks it against the release's `SHA256SUMS`, sets up
  `/dev/uinput` access, and optionally a systemd user service. Run it again to update.

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
machines can build from source (below).

## Install

### With the install script

```sh
curl -fsSLO https://github.com/juricabi/elrs-crossfire-wifi-joystick-linux/releases/latest/download/install.sh
sudo sh install.sh
```

As root (sudo) it:

- downloads the binary for this machine (`uname -m`) from the latest release, checks it against
  `SHA256SUMS`, checks that it runs here, and installs it as `/usr/local/bin/elrs-wifi-joystick`;
- installs the udev rule `/etc/udev/rules.d/60-elrs-wifi-joystick.rules`, which gives the user logged
  in at the screen (and members of the `input` group) access to `/dev/uinput`, and applies it at once;
- loads the `uinput` kernel module, now and at every boot (`/etc/modules-load.d/elrs-wifi-joystick-uinput.conf`);
- with `--service`: a systemd user service for the user who ran sudo (see [Service](#service)).

Without sudo it installs the program for you only, in `~/.local/bin`, and prints the commands that
need root. Run the script again to update; `sudo sh install.sh --uninstall` removes exactly what it
installed (it keeps a list), and leaves your settings.

| option | |
|---|---|
| `--version X` | install release X (default: the latest) |
| `--file PATH` | install a binary you already have instead of downloading one |
| `--user` | install for this user only, in `~/.local/bin` (the default without root) |
| `--service` | also the systemd user service: enabled, and started now if that user is logged in |
| `--no-udev` | leave `/dev/uinput` access and the uinput module alone |
| `--uninstall` | remove what the script installed |
| `--arch A`, `--root DIR`, `--dry-run` | another machine's binary, install into a directory tree, only show what would be done |

**While the repository is private** the downloads need a token that can read it. With the GitHub CLI:

```sh
gh release download -R juricabi/elrs-crossfire-wifi-joystick-linux -p install.sh
GITHUB_TOKEN=$(gh auth token) sudo --preserve-env=GITHUB_TOKEN sh install.sh
```

The script passes the token to curl on stdin (never on a command line) and only to api.github.com.

### By hand

1. Download `elrs-wifi-joystick-<version>-linux-<arch>` and `SHA256SUMS` from the
   [releases](../../releases), and check it: `sha256sum -c SHA256SUMS --ignore-missing`.
2. Install it: `sudo install -m 0755 elrs-wifi-joystick-*-linux-<arch> /usr/local/bin/elrs-wifi-joystick`.
3. Give access to `/dev/uinput`: `sudo sh install.sh --file elrs-wifi-joystick-*-linux-<arch>` does
   steps 2 and 3 with the checked binary; or by hand, with the files from [`packaging/`](packaging/):
   ```sh
   sudo install -m 0644 packaging/60-elrs-wifi-joystick.rules /etc/udev/rules.d/
   sudo install -m 0644 packaging/elrs-wifi-joystick-uinput.conf /etc/modules-load.d/
   sudo modprobe uinput
   sudo udevadm control --reload
   sudo udevadm trigger --action=change --subsystem-match=misc --sysname-match=uinput
   ```

### From source

You need a C compiler (gcc or clang) with the Linux kernel headers, and a POSIX shell (Debian, Ubuntu,
Raspberry Pi OS: `sudo apt install build-essential`; Fedora: `sudo dnf install gcc kernel-headers`;
Alpine: `apk add build-base linux-headers`).

```sh
git clone https://github.com/juricabi/elrs-crossfire-wifi-joystick-linux
cd elrs-crossfire-wifi-joystick-linux
./build.sh native prog                     # -> build/native/elrs-wifi-joystick
sudo sh packaging/install.sh --file build/native/elrs-wifi-joystick
```

The static release binaries are made with [zig](https://ziglang.org/) 0.15.x as the cross compiler:
`./build.sh all prog` builds all six into `build/<arch>/`.

## Usage

1. Put the module on the same WiFi network as this machine (or join this machine to the module's own
   access point). The module's WiFi is 2.4 GHz only.
2. Run `elrs-wifi-joystick`. It listens on UDP port 11000, finds the module by its broadcast, asks it
   to stream, and the virtual joystick follows your sticks. The status line at the bottom shows the
   module, the measured rate and jitter, the request, and the channels.
3. Open <http://127.0.0.1:11000/> to watch the sticks and change the settings, or use the keys.
4. In the simulator, pick the joystick named **ELRS Crossfire WiFi Joystick** and calibrate it.

| key | |
|---|---|
| `s` | start / stop listening |
| `t` | the module's IP (empty: find it by its broadcast) |
| `r` | the rate: 1000 500 333 250 200 150 100 50 Hz, or any 10 to 1000 |
| `c` | the channels: 4 to 16 |
| `p` | the UDP port |
| `a` | restart the stream (send our request again) |
| `w` | print the web page's address |
| `h`, `?` | help |
| `q`, Ctrl+C | quit (the joystick is centered and removed) |

Common options (all of them, and the settings file, in [docs/USAGE.md](docs/USAGE.md)):

```
elrs-wifi-joystick --module 192.168.1.50      # this module only, without waiting for its broadcast (also --tx)
elrs-wifi-joystick --rate 500 --channels 8    # ask for 500 Hz and 8 channels
elrs-wifi-joystick --web 0.0.0.0:11000        # the web page for other machines too (no password!)
elrs-wifi-joystick --no-joystick              # monitor only
elrs-wifi-joystick --list-rates               # the supported rates and channel counts
```

Settings changed with the keys, the stdin commands or the web page are saved to
`~/.config/elrs-wifi-joystick/config` (`$XDG_CONFIG_HOME/elrs-wifi-joystick/config`) and used the next
time; command-line options win over the file for that run.

**The module keeps streaming** after you stop or quit the program: it sends until it is powered off,
or until something asks it to stop. The program only sends ELRS a stop (`joystick_end`) as part of a
restart, to change the request.

### Service

`sudo sh install.sh --service` (or `sh install.sh --service` for a `~/.local/bin` install) installs
the systemd user service `elrs-wifi-joystick.service`, which runs `elrs-wifi-joystick --daemon` while
you are logged in:

```sh
systemctl --user status elrs-wifi-joystick       # state and the last log lines
systemctl --user restart elrs-wifi-joystick
journalctl --user -u elrs-wifi-joystick -f       # the log
sudo loginctl enable-linger $USER                # run it from boot, without logging in (headless)
```

Change its settings on the web page. Only one copy may run: stop the service before running the
program in a terminal (`systemctl --user stop elrs-wifi-joystick`). Started from boot there is no
desktop session, so the joystick needs the `input` group: `sudo usermod -aG input $USER`.

## The request: rate and channels

The program asks the module for an update interval and a channel count (`action=joystick_begin&
interval=<us>&channels=<n>`). Both are settings (keys `r`/`c`, `--rate`/`--interval`/`--channels`, the
web page):

| rate | 1000 Hz | 500 Hz | 333 Hz | **250 Hz** | 200 Hz | 150 Hz | 100 Hz | 50 Hz |
|---|---|---|---|---|---|---|---|---|
| interval | 1000 µs | 2000 µs | 3000 µs | **4000 µs** | 5000 µs | 6667 µs | 10000 µs | 20000 µs |

Any rate from 10 to 1000 Hz (interval 1000 to 100000 µs) is accepted. Channels: 4 to 16. The default
is **250 Hz and 16 channels**, the same as the Windows app.

- **ExpressLRS** (3.4.0 and later) streams what it is asked for, but takes a new request only while it
  is not streaming: a request to a streaming module just moves the stream. So when you change the rate
  or the channels while an ELRS module streams, the program restarts its stream (`joystick_end`, then
  `joystick_begin`). A module that is surely ELRS (by its beacon, or because it streams a channel count
  other than 16, which Crossfire never does) and streams a different channel count than the request -
  left over from another app - is restarted once per module and session.
- **Crossfire / Tracer** starts on the same request but ignores the rate and the channel count: it
  always sends 16 channels at about 90 Hz. It is never sent `joystick_end` (what Crossfire does with it
  is unknown).
- With fewer than 16 channels, the buttons of the missing channels stay released (and with fewer than
  8, the missing axes stay centered).

The web page and the status line show the measured rate and jitter, so you can compare settings.

## The joystick

| channel | Linux code | axis / button number in SDL and `/dev/input/js*` | Windows app (vJoy) | usual (AETR) |
|---|---|---|---|---|
| CH1 | `ABS_X` | axis 0 | X | roll |
| CH2 | `ABS_Y` | axis 1 | Y | pitch |
| CH3 | `ABS_RX` | axis 3 | RX | throttle |
| CH4 | `ABS_RY` | axis 4 | RY | yaw |
| CH5 | `ABS_RZ` | axis 5 | RZ | AUX1 |
| CH6 | `ABS_Z` | axis 2 | Z | AUX2 |
| CH7 | `ABS_THROTTLE` | axis 6 | Slider 0 | AUX3 |
| CH8 | `ABS_RUDDER` | axis 7 | Slider 1 | AUX4 |
| CH9-CH16 | `BTN_TRIGGER` ... `BTN_BASE2` (0x120-0x127) | buttons 0-7 ("buttons 1-8") | - | AUX5-AUX12 |

- Axes: 0 to 32767, exactly the module's raw values (center 16384), with no kernel dead zone or fuzz.
  SDL and the js interface number the axes in the order of their codes, which is why CH6 (`ABS_Z`) is
  axis 2.
- Buttons: pressed at 24576 (75 %) and above, released below 20480 (62.5 %); in between a button keeps
  its state, so a switch near a threshold never chatters. A switch at +100 % presses its button; at 0 %
  or -100 % it releases it. `--no-buttons` turns this off.
- The device: name "ELRS Crossfire WiFi Joystick" (`--name`), bus virtual, vendor and product 0. When
  the module goes quiet for more than 3 s, on stop and at exit, every axis goes to the center and
  every button is released; shorter gaps hold the last values.

## Crossfire firmware compatibility (important)

**Some TBS firmware versions break the WiFi joystick feature entirely.** The module still streams
frames at full rate, but they never contain stick data: every channel holds a frozen placeholder
(`0xF26A` = 62058). This affects every client the same way, including TBS's own reference client
VelociDrone Mobile, so no software on the computer can work around it.

| Component | Works | Broken |
|-----------|-------|--------|
| **Crossfire TX (XF)** | **6.31**, **6.36** | **6.42 public, 6.48 beta, 6.48 public** |
| **WiFi module** | **v2.17** up to **v3.10** (v2.25 recommended) | **v3.20** |

VelociDrone's developers confirmed the TX regression and reported it to TBS - see
[TBS firmware breaks VelociDrone mobile wi-fi support](https://batcavegames.freshdesk.com/support/solutions/articles/16000225491).
Downgrade with TBS Agent (desktop, or Agent Lite on the radio); the TX and the WiFi module firmware are
updated separately.

On an affected version a placeholder frame is not channel data: it never takes the lock and never
reaches the joystick. The status stays *searching* with "module found, but it is sending no stick
data", and the log names the firmware fix. The values are deliberately not clamped into range - that
would show every axis at full deflection and give your simulator full throttle.

## Troubleshooting

| problem | what to do |
|---|---|
| **"no permission to use /dev/uinput"** | Run `sudo sh install.sh` once (its udev rule gives the user at the screen access). Over SSH, without a desktop, or for a service started at boot: `sudo usermod -aG input $USER` and log in again. For this boot only: `sudo setfacl -m u:$USER:rw /dev/uinput`. |
| **"/dev/uinput does not exist"** | `sudo modprobe uinput`. If that fails, the kernel was built without uinput. The program still runs as a monitor. |
| **Searching, no module found** | Is the module on the same network (check its IP in your router, ping it)? Guest WiFi networks often isolate clients: use your main network. VPNs and virtual network adapters can get in the way; the program prints this machine's addresses at start. Or give the IP: `--module 192.168.1.50`. |
| **The module is found and activated, but no data arrives** | A firewall drops the incoming UDP stream (the activation is outgoing, so it works). Let UDP port 11000 in: `sudo ufw allow 11000/udp` (ufw), or `sudo firewall-cmd --permanent --add-port=11000/udp && sudo firewall-cmd --reload` (firewalld). The program detects ufw and firewalld and prints the right command. |
| **"module found, but it is sending no stick data"** | Crossfire firmware: see the [compatibility table](#crossfire-firmware-compatibility-important). |
| **Two modules on the network** | The first one that streams real stick data takes the joystick; the other is ignored until the first has been silent for 3 s. Pick one with `--module IP`, the `t` key or the web page. |
| **"UDP port 11000 in use"** | Another program listens there (`ss -ulpn 'sport = :11000'` shows which). It retries every 5 s. |
| **"already running"** (exit code 3) | Only one copy may run, e.g. the service: `systemctl --user stop elrs-wifi-joystick`. |
| **The game does not see the joystick** | Check that it exists: `grep -A4 'ELRS Crossfire' /proc/bus/input/devices`, and test it with `evtest` or `jstest-gtk`. Games read `/dev/input/eventN`: at the screen udev gives your user access to joysticks; over SSH or headless you need the `input` group. |
| **Steam** | If Steam shows the joystick as a gamepad, or a game sees axes missing or doubled, turn Steam Input off for that game (the game's Properties, Controller). |
| **Windows games (Proton, Wine)** | They get the joystick through Wine's joystick support; `wine control joy.cpl` (for Proton: `protontricks -c 'wine control joy.cpl' <appid>`) shows whether Wine sees it. Bind and calibrate the axes in the game. |
| **Axes look wrong or half-throw** | The axes carry the raw 0-32767 values; calibrate them in the simulator. Check that your radio's channel order matches the table above. |

## Building and testing

```sh
./build.sh native test                       # unit tests (SAN=1: AddressSanitizer + UBSan; WERROR=1 like CI)
python3 tests/e2e.py                         # the program against fake modules (no hardware)
python3 tests/uinput_e2e.py                  # the virtual joystick through the kernel (needs /dev/uinput)
python3 tests/test_ui.py                     # the web page in headless Chromium (Playwright)
sh tests/test_install.sh                     # install.sh in a fake root with a fake GitHub
./build.sh all                               # the six static binaries (zig cc), in build/<arch>/
RUN="qemu-aarch64-static" ./build.sh aarch64 test    # unit tests of another architecture
```

CI (GitHub Actions) runs all of it on every push: the unit tests with gcc and clang under ASan and
UBSan, the end-to-end tests, the virtual joystick through a real kernel with the udev rule installed by
`install.sh` and the systemd user service, the six static binaries with every unit test under qemu on
the oldest CPU each is for, the web page in Chromium, `core/` compiled for Windows and macOS, and
shellcheck. A tag `vX.Y.Z` that matches `VERSION` publishes a release with the six binaries,
`install.sh` and `SHA256SUMS`. [docs/DESIGN.md](docs/DESIGN.md) describes how it works,
[docs/WEB_API.md](docs/WEB_API.md) the web page's API.

## Support

If you like it, you can buy me a coffee: [Ko-fi](https://ko-fi.com/juricabi)

## License

MIT - see [LICENSE](LICENSE).

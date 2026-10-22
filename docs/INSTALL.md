# Install

Installing `elrs-wifi-joystick`, running it as a service, updating and removing it. [README.md](../README.md)
is the overview (and lists the [machines](../README.md#machines) there are binaries for);
[USAGE.md](USAGE.md) explains how to use it; [TROUBLESHOOTING.md](TROUBLESHOOTING.md) helps when
`/dev/uinput` access or the network gets in the way.

## With the install script

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
- with `--service`: a systemd user service for the user who ran sudo (see [The service](#the-systemd-user-service)).

Without sudo it installs the program for you only, in `~/.local/bin`, and prints the commands that
need root. Run it again to [update](#updating); `--uninstall` [removes](#uninstalling) exactly what it
installed.

### Options

```
sh install.sh [options]
```

| option | |
|---|---|
| `--version X` | install release X (`1.2.3` or `v1.2.3`; default: the latest) |
| `--file PATH` | install this binary (one you already have) instead of downloading one |
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

### While the repository is private

The downloads need a token that can read it. With the GitHub CLI:

```sh
gh release download -R juricabi/elrs-crossfire-wifi-joystick-linux -p install.sh
GITHUB_TOKEN=$(gh auth token) sudo --preserve-env=GITHUB_TOKEN sh install.sh
```

The script passes the token to curl on stdin (never on a command line) and only to api.github.com.

### What it installs and checks

As root: `/usr/local/bin/elrs-wifi-joystick`, `/etc/udev/rules.d/60-elrs-wifi-joystick.rules`,
`/etc/modules-load.d/elrs-wifi-joystick-uinput.conf`, with `--service`
`/etc/systemd/user/elrs-wifi-joystick.service`, and its list of these in
`/usr/local/share/elrs-wifi-joystick/installed`. Without root: `~/.local/bin/elrs-wifi-joystick`, with
`--service` `~/.config/systemd/user/elrs-wifi-joystick.service`, and the list in
`~/.local/share/elrs-wifi-joystick/installed`.

- The architecture comes from `uname -m`: `x86_64`/`amd64`, `i386`..`i686`, `aarch64`/`arm64`,
  `armv7l`/`armv8l` (a 32-bit system on a 64-bit ARM CPU), `armv6l`, `riscv64`. Anything else stops
  with a message ([build from source](#building-from-source) then).
- A download is checked against the release's `SHA256SUMS` (with `sha256sum` or `shasum -a 256`), and
  the program must run here (`--version`) before it replaces anything (unless `--arch` or `--root` says
  it is for another machine). Files are installed atomically
  (a temporary file next to the target, then renamed). Downloads use curl, else wget (wget only for a
  public repository).
- Exit codes: 0 done, 1 failed (nothing was installed when the download or its check failed),
  2 bad options.

## By hand

1. Download `elrs-wifi-joystick-<version>-linux-<arch>` and `SHA256SUMS` from the
   [releases](https://github.com/juricabi/elrs-crossfire-wifi-joystick-linux/releases), and check it:
   `sha256sum -c SHA256SUMS --ignore-missing`.
2. Install it: `sudo install -m 0755 elrs-wifi-joystick-*-linux-<arch> /usr/local/bin/elrs-wifi-joystick`.
3. Give access to `/dev/uinput`: `sudo sh install.sh --file elrs-wifi-joystick-*-linux-<arch>` does
   steps 2 and 3 with the checked binary; or by hand, with the files from [`packaging/`](../packaging/):
   ```sh
   sudo install -m 0644 packaging/60-elrs-wifi-joystick.rules /etc/udev/rules.d/
   sudo install -m 0644 packaging/elrs-wifi-joystick-uinput.conf /etc/modules-load.d/
   sudo modprobe uinput
   sudo udevadm control --reload
   sudo udevadm trigger --action=change --subsystem-match=misc --sysname-match=uinput
   ```

## The systemd user service

`sudo sh install.sh --service` (or `sh install.sh --service` for a `~/.local/bin` install) installs
the systemd user service `elrs-wifi-joystick.service` ([`packaging/elrs-wifi-joystick.service`](../packaging/elrs-wifi-joystick.service)):
system-wide in `/etc/systemd/user/` with sudo, or in `~/.config/systemd/user/` for a `~/.local/bin`
install, and enabled for one user. It runs `elrs-wifi-joystick --daemon` while you are logged in, and
is restarted 5 s after a failure, but not after exit codes 2 and 3.

```sh
systemctl --user status elrs-wifi-joystick       # state and the last log lines
systemctl --user stop|start|restart elrs-wifi-joystick
systemctl --user disable --now elrs-wifi-joystick
journalctl --user -u elrs-wifi-joystick -f       # the log
sudo loginctl enable-linger $USER                # run it from boot, without logging in (headless)
```

Change its settings on the web page. Only one copy may run: stop the service before running the
program in a terminal (`systemctl --user stop elrs-wifi-joystick`). Started from boot there is no
desktop session, so `/dev/uinput` access comes from the `input` group (`sudo usermod -aG input $USER`),
not from the udev rule's `uaccess`.

## Updating

Run the install script again: it updates the program (and the service's unit), and restarts a running
service. `--version X` installs a given release instead of the latest.

## Uninstalling

`sudo sh install.sh --uninstall` (without sudo for a `~/.local/bin` install) stops and disables the
service, removes the files on its list (and nothing else, even when the list was edited), and reloads
udev. Your settings (`~/.config/elrs-wifi-joystick/`) stay; `/dev/uinput` keeps its current access
until the next reboot.

## Building from source

You need a C compiler (gcc or clang) with the Linux kernel headers, and a POSIX shell (Debian, Ubuntu,
Raspberry Pi OS: `sudo apt install build-essential`; Fedora: `sudo dnf install gcc kernel-headers`;
Alpine: `apk add build-base linux-headers`).

```sh
git clone https://github.com/juricabi/elrs-crossfire-wifi-joystick-linux
cd elrs-crossfire-wifi-joystick-linux
./build.sh native prog                     # -> build/native/elrs-wifi-joystick
sudo sh packaging/install.sh --file build/native/elrs-wifi-joystick
```

The static release binaries, the build options and the tests are in [DEVELOPMENT.md](DEVELOPMENT.md).

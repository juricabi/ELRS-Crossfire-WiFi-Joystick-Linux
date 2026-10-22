# Troubleshooting

What to do when something does not work. [USAGE.md](USAGE.md) explains how the program behaves,
[INSTALL.md](INSTALL.md) how it is installed.

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
reaches the joystick. The status stays *searching* with "no stick data from <ip>", and the log says
"<ip> is streaming placeholder frames - no stick data" and names the firmware fix. The values are deliberately not clamped into range - that
would show every axis at full deflection and give your simulator full throttle.

## The module

### Searching, no module found

Is the module on the same network (check its IP in your router, ping it)? Guest WiFi networks often
isolate clients: use your main network. VPNs and virtual network adapters can get in the way; the
program prints this machine's addresses at start. Or give the IP: `--module 192.168.1.50`.

### The module is found and activated, but no data arrives

A firewall drops the incoming UDP stream (the activation is outgoing, so it works). Let UDP port 11000
in. The program detects ufw and firewalld and prints the right command:

```sh
sudo ufw allow 11000/udp                                                          # ufw
sudo firewall-cmd --permanent --add-port=11000/udp && sudo firewall-cmd --reload  # firewalld
```

### "no stick data from <ip>" (placeholder frames)

The radio is not feeding the module right now, or it is the Crossfire firmware: see [Crossfire firmware compatibility](#crossfire-firmware-compatibility-important).
The status goes back to "listening" (with a module IP set: "connecting to <ip>") 3 s after the
placeholder frames stop.

### The rate or the channel count does not change

The status line and the web page show the measured rate and the channels the module sends. The rules
are in [USAGE.md](USAGE.md#the-request-rate-and-channels); in short:

- **Crossfire / Tracer** always sends 16 channels at about 90 Hz, whatever is asked ("Crossfire module
  <ip> ignores the rate and channel count").
- **"<ip> may be Crossfire (no beacon from it yet) ..."**: a module streaming 16 channels that sent no
  beacon is restarted only once it has streamed 20 s without a Crossfire beacon. Wait, or power the
  module off and on: an ELRS module beacons until it streams, and its first stream takes the request.
- **A module still streaming from before** the program started keeps its old rate: press `a` (or
  restart in the web page). Only a different channel count is fixed by itself.
- **"The restart of <ip> failed ..."**: the module did not answer the restart with a 2xx (look at
  "Stream end POST" and "Activation POST" in the log). It is sent again every 5 s while the module streams
  (3 tries in all), then the program gives up: check the module's web server port (`--http-port`) and
  that nothing else (another app, a phone) talks to the module, then press `a`.

### Two modules on the network

The first one that streams real stick data takes the joystick; the other is ignored until the first
has been silent for 3 s. Pick one with `--module IP`, the `t` key or the web page: the one streaming
now keeps the joystick; another one takes over at once (the joystick centers until its data arrives).

### "UDP port 11000 in use"

Another program listens there (`ss -ulpn 'sport = :11000'` shows which). It retries every 5 s.

### "already running" (exit code 3)

Only one copy may run, e.g. the service: `systemctl --user stop elrs-wifi-joystick`.

## The web page

### "Open this page by its IP address" (or `host not allowed`)

The page answers only to an IP address or localhost, not to a host name such as `raspberrypi.local`
(that keeps other web sites out: DNS rebinding). Open `http://<the machine's IP>:11000/` (with
`--web 0.0.0.0:11000`) or `http://127.0.0.1:11000/` on that machine; the program prints the address
when it starts ("Web page: ...", key `w`).

### "No web page: cannot listen on ..."

Another program (or another copy) uses that port, or the address is not one of this machine: pick
another with `--web ADDR:PORT`, or `--web off`.

## The joystick

Without access to `/dev/uinput` the program still runs, as a monitor, and says what to do.

### "no permission to use /dev/uinput"

Run `sudo sh install.sh` once (its udev rule gives the user at the screen access). Over SSH, without a
desktop, or for a service started at boot: `sudo usermod -aG input $USER` and log in again. For this
boot only: `sudo setfacl -m u:$USER:rw /dev/uinput`.

### "/dev/uinput does not exist"

`sudo modprobe uinput`. If that fails, the kernel was built without uinput.

### The game does not see the joystick

Check that it exists: `grep -A4 'ELRS Crossfire' /proc/bus/input/devices` (or the name you gave with
`--name`; the startup log says `Virtual joystick "<name>" ready`), and test it with `evtest` or
`jstest-gtk`. Games read `/dev/input/eventN`: at the screen udev gives your user access to joysticks;
over SSH or headless you need the `input` group.

### Steam

If Steam shows the joystick as a gamepad, or a game sees axes missing or doubled, turn Steam Input off
for that game (the game's Properties, Controller).

### Windows games (Proton, Wine)

They get the joystick through Wine's joystick support; `wine control joy.cpl` (for Proton:
`protontricks -c 'wine control joy.cpl' <appid>`) shows whether Wine sees it. Bind and calibrate the
axes in the game.

### Axes look wrong or half-throw

The axes carry the raw 0-32767 values; calibrate them in the simulator. Check that your radio's channel
order matches the [joystick table](USAGE.md#the-joystick).

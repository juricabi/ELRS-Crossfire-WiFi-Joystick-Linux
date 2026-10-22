# Usage reference

Everything `elrs-wifi-joystick` takes and does: command-line options, keys, stdin commands, the web
page, the settings file, the request (rate and channels), the joystick, the environment and exit codes.
[README.md](../README.md) is the overview; [INSTALL.md](INSTALL.md) covers installing, the service and
updates; [TROUBLESHOOTING.md](TROUBLESHOOTING.md) helps when something does not work;
[DESIGN.md](DESIGN.md) explains how it works; [WEB_API.md](WEB_API.md) is the web page's API.

## How it finds the module

Both radios speak the same "WiFi joystick" protocol (the one VelociDrone Mobile uses). The program
listens on UDP port 11000, finds the module by its broadcast, asks it to stream (see
[The request](#the-request-rate-and-channels)), and the virtual joystick follows your sticks. ELRS
announces itself with an `ELRS` beacon, Crossfire with `VELOCIDRONE`; the program handles both.

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
| `-w`, `--web` | [ADDR:]PORT | 127.0.0.1:11000 | The web page's address. ADDR is an IPv4 literal or `localhost`; a PORT alone means 127.0.0.1:PORT. `0.0.0.0:11000` makes it reachable from the network (it has no password); open it there by this machine's IP address (a host name such as `raspberrypi.local` gets a page that says so). |
| `--no-web` | | | No web page. |
| `--no-joystick` | | | Monitor only: no virtual joystick. |
| `--no-buttons` | | | CH9..CH16 press no buttons (the device keeps its 8 buttons, never pressed). |
| `--name` | NAME | ELRS Crossfire WiFi Joystick | The joystick's name: 1 to 63 bytes (a letter outside ASCII takes 2 to 4), no control characters. |
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

Examples:

```
elrs-wifi-joystick --module 192.168.1.50      # this module only, without waiting for its broadcast (also --tx)
elrs-wifi-joystick --rate 500 --channels 8    # ask for 500 Hz and 8 channels
elrs-wifi-joystick --web 0.0.0.0:11000        # the web page for other machines too (no password!)
elrs-wifi-joystick --no-joystick              # monitor only
elrs-wifi-joystick --list-rates               # the supported rates and channel counts
```

## The terminal

**Interactive** (stdin and stdout are terminals, in the foreground, no `--daemon`): a header, the log
lines as they come (colored unless `NO_COLOR` is set to a non-empty value, or `TERM` is unset or `dumb`),
and one status line at the bottom, redrawn about 10
times a second: the state, the module, the measured rate and jitter, the request (rate x channels), 8
bars for CH1..CH8 and marks for the 8 buttons. Keys (upper or lower case):

| key | |
|---|---|
| `s` | start / stop listening |
| `t` | prompt for the module's IP; empty = find it by its broadcast |
| `r` | prompt for the rate (the presets, or 10..1000 Hz) |
| `c` | prompt for the channels (4..16) |
| `p` | prompt for the UDP port |
| `a` | restart the stream, so that a new rate or channel count takes effect (a module known as Crossfire by its beacon is never sent `joystick_end`); see [the request](#the-request-rate-and-channels) |
| `w` | print the web page's address |
| `h`, `?` | help |
| `q`, Ctrl+C | quit (the joystick is centered and removed) |

In a prompt, Enter alone keeps the value (for `t` it means "find it by its broadcast"), and Esc then
Enter cancels; the rest of an answer longer than the prompt takes (a paste) is dropped, never run as keys.
The terminal is restored on exit, on every signal (also `Ctrl+\`, SIGQUIT) and around Ctrl+Z. Ctrl+Z
stops listening first, so the joystick is centered while the program is stopped ("Suspended: not
listening (the joystick is centered) until the program goes on (fg)"); it listens again after `fg` or
`bg`. In the background (after `bg`, or started with `&`) it neither reads the keys nor draws the status
line: it runs on, its log lines show after `fg`, and `kill %1` ends it.

**Not interactive** (stdin or stdout is not a terminal, or it runs in the background): log lines with
the time, and a plain status line every 2 s while streaming, for example

```
14:03:07  streaming from ELRS 192.168.1.50 (Unified TX) | 249.8 Hz, jitter 0.42 ms | asked 250 Hz x 16 ch | CH1-8 % 50 50 0 50 0 0 0 0 | buttons X-------
```

Commands are then read from stdin, one per line (case does not matter, a UTF-8 byte order mark before
the first one is ignored, an unknown command prints an error line). The end of stdin ends the commands,
not the program, so `elrs-wifi-joystick < /dev/null` keeps running. A terminal stdin is read only while
the program is in the foreground: started with `&`, it reads its commands after `fg`. Commands in a row
see each other (`s` twice is a stop, then a start; `status` right after `stop` says stopped). A value
already in effect is said so and changes nothing ("Asking for 8 channels already", "Listening on UDP port
11000 already", or while stopped "The UDP port is 11000 already (used when listening starts)"). When
stdout is a pipe whose reader goes away (`| head -5`), the program quits (not with `--daemon`).

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
streaming, and stdin is not read. This is what the service runs ([INSTALL.md](INSTALL.md#the-systemd-user-service)).

## The web page

`http://127.0.0.1:11000/` by default: the sticks and buttons live, the rate and jitter, the module, the
request (rate and channels, with the supported values), the module IP and ports, start / stop /
restart, and the log. It polls the program 10 times a second while it is visible and loads nothing
from the internet. When the program restarts, the page starts its log over and reads the settings again.
Changes apply at once and are saved like the keys' changes.

It accepts requests only for `localhost`, `127.0.0.1`, `[::1]` or an IPv4 address in the `Host`
header, refuses other sites' pages (`Origin`), and answers changes only to `application/json`
requests, so other web pages in your browser cannot use it. With `--web 0.0.0.0:11000` anyone on your
network can open it and change the settings: there is no password. Opened by a host name
(`http://raspberrypi.local:11000/` with `--web 0.0.0.0:11000`), it shows a short page that says to open
it by the IP address instead (e.g. `http://192.168.1.20:11000/`): host names are refused to keep other
web sites out (DNS rebinding). The API is in [WEB_API.md](WEB_API.md).

## Settings file

Settings changed with the keys, the stdin commands or the web page are saved here and used the next
time. The file is `$XDG_CONFIG_HOME/elrs-wifi-joystick/config` (an absolute `XDG_CONFIG_HOME` only),
else `~/.config/elrs-wifi-joystick/config`; `--config FILE` uses another file, `--no-config` none.

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
| `name` | the joystick's name: 1 to 63 bytes (a letter outside ASCII takes 2 to 4), no control characters; spaces at its ends are dropped | ELRS Crossfire WiFi Joystick |

- One `key = value` per line; lines starting with `#` are comments; spaces around keys and values do
  not matter. An unknown key, a bad value or a line that is not `key = value` is a warning in the log,
  and that line is ignored (and left out the next time the file is written); it never stops the program.
- The program writes only the first five keys, and only those you changed while it ran (keys, stdin
  commands, web page): the first line of such a key gets the new value and its later lines are dropped.
  Every other line stays as it is. A new file starts with a short explanation.
- Writes are atomic (a temporary file next to it, flushed to disk, then renamed); the folder is
  created when needed, a symlinked file is written where the link points (also when that file does not
  exist yet); an existing file keeps its mode, a new one gets 0666 minus the umask (0644 with the usual
  umask 022).
  If the file cannot be written, the change still applies and the log says why it was not saved.

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
  or the channels (keys, stdin commands, web page) while a module that is surely ELRS streams to the
  program - known by its beacon, or streaming a channel count other than 16, which Crossfire never
  sends - the program restarts its stream at once: `joystick_end`, then `joystick_begin` with the new
  values ("Restarting the stream of <ip>: 500 Hz, 16 channels").
- **Crossfire / Tracer** starts on the same request but ignores the rate and the channel count: it
  always sends 16 channels at about 90 Hz. A module known as Crossfire by its beacon is never sent
  `joystick_end` (what Crossfire does with it is unknown): a change while it streams is only logged, once
  per session ("Crossfire module <ip> ignores the rate and channel count ...").
- **A module of unknown kind** that streams 16 channels and sent no beacon (ELRS beacons only while it
  is not streaming, so an ELRS module still streaming from before has sent none) might be Crossfire. Its
  restart (a change, `a`, or one of the cases below) waits until 20 s after the program locked on to it,
  and is dropped when a Crossfire beacon arrives first; the log says so ("<ip> may be Crossfire (no beacon
  from it yet) ..."). Crossfire beacons about every 8 s also while streaming, so after 20 s without one
  it is taken as ELRS. A restart waiting like this is kept when the module goes quiet and goes out when it
  streams to the program again (after its 20 s); it is dropped when the module's ELRS beacon meanwhile
  gets it a `joystick_begin` with the request, or when another module takes over.
- **A change while the module does not stream to the program** (during its activation, a WiFi dropout
  of more than 3 s, a stop, or before switching the module IP to it) goes into its next
  `joystick_begin`. The program remembers what it last asked each module and what their beacons said
  (up to 8 modules, never forgetting the kind of the one streaming now; with more modules the one heard
  from longest ago is forgotten), and keeps both across a stop and start or a port change (those clear the
  lock, the 5 s limits and waiting restarts): a module that streams to the program again with an older
  request (it streams on through those) is restarted then, by the rules above ("<ip> still streams
  250 Hz, 16 channels from before the change - restarting its stream with ..."; a rate that is not one of
  the presets is shown with its interval, "286 Hz (3500 us)").
- **At startup** the values from the command line and the settings file only set the next
  `joystick_begin`. A module still streaming from before (it keeps streaming after the program quits,
  below) keeps its old rate: press `a` (or restart in the web page) to apply them. Only a different
  channel count is fixed by itself, once per module and session: a module that is surely ELRS and
  streams another channel count than the request (left over from another app or run) is restarted at
  once ("<ip> streams 8 channels, 16 asked for ..."); one of unknown kind that streams 16 while fewer are
  asked, once its 20 s passed without a Crossfire beacon.
- **`a` (restart, also the web page's Restart and the `restart` command)** restarts the module streaming
  to the program by the rules above (Crossfire: logged, nothing sent). When none streams, the module IP
  (`--module`, `t`) is restarted if its beacon said ELRS; otherwise (no beacon yet, or Crossfire) it only
  gets a `joystick_begin` at once, never `joystick_end` ("<ip> has sent no beacon yet (it may be
  Crossfire, which is never sent joystick_end): activating it with ..."): a module that is not streaming
  takes the request from that alone. Without a module IP nothing is sent ("No module to restart ...");
  stopped, nothing either ("Not running - nothing to restart"), nor while the UDP port is not open yet
  ("Not listening - nothing to restart").
- **A restart that fails** (its `joystick_end` or its `joystick_begin` gets no 2xx answer) changed
  nothing. It is sent again every 5 s while the module still streams to the program and is not known as
  Crossfire, 3 sends per change in all ("The restart of <ip> failed - sending it again in 5 s"), then the
  program gives up and says so ("... failed 3 times - giving up ..."); press `a` to try again. A plain
  `joystick_begin` that fails is never turned into a restart: the next beacon, or the module IP's next
  try, sends it again after 5 s. A restart that did not take (it failed, or a stop or port change cut it
  short before its answer came) and could not be sent again because the module stopped streaming to the
  program (a WiFi dropout of more than 3 s, switching the module IP, a stop or a port change) leaves the
  program not knowing what the module streams: when it streams to the program again it is restarted once
  more, by the rules above ("<ip> may still stream its old request (its restart failed) ...", or "...
  (Stop cut its restart short) ...").
- **Switching the module IP** to the module that streams now keeps it: nothing is sent, nothing
  centered ("Target module set to <ip> - streaming from it already"). Another module IP lets go of the
  module streaming now (the joystick centers), asks the new one at once ("connecting to <ip>") and sends
  the module being left nothing: a restart waiting for it is dropped, also when the rate or the channels
  change in the same go (the new module's `joystick_begin` carries them).
- **Timing**: one activation per module per 5 s for the automatic ones (beacons, the module IP's
  retries); setting the module IP, `a`, and a restart go out at once and start the 5 s over. A restart
  decided while a request to the same module is still under way goes out as soon as that one is
  answered. Lock loss, sending again and waiting restarts are checked at least 10 times a second,
  whatever arrives.

The web page and the status line show the measured rate and jitter, so you can compare settings.

**The module keeps streaming** after you stop or quit the program: it sends until it is powered off,
or until something asks it to stop. The program only sends ELRS a stop (`joystick_end`) as part of a
restart, to change the request.

## The joystick

A virtual joystick through the kernel's uinput: 8 axes in the order of the Windows app's vJoy mapping,
so bindings carry over, and CH9-CH16 as 8 buttons (like the ELRS Bluetooth joystick).

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

- Axes: 0 to 32767, exactly the module's raw values (center 16384): no scaling, no filtering, and no
  kernel dead zone or fuzz. SDL and the js interface number the axes in the order of their codes, which
  is why CH6 (`ABS_Z`) is axis 2.
- Buttons: pressed at 24576 (75 %) and above, released below 20480 (62.5 %); in between a button keeps
  its state, so a switch near a threshold never chatters. A switch at +100 % presses its button; at 0 %
  or -100 % it releases it. With `--no-buttons` the device still has its 8 buttons (so that udev and SDL
  take it for a joystick), but they are never pressed.
- Fewer channels: with fewer than 16 channels, the buttons of the missing channels stay released (and
  with fewer than 8, the missing axes stay centered).
- The device: name "ELRS Crossfire WiFi Joystick" (`--name`), bus virtual, vendor and product 0.
- When the module goes quiet for more than 3 s, on stop (also Ctrl+Z), at exit and on a switch to
  another module IP, every axis goes to the center and every button is released; shorter gaps hold the
  last values. Setting the module IP to the module that streams now keeps it (nothing is centered).

## Environment

| variable | |
|---|---|
| `XDG_CONFIG_HOME`, `HOME` | where the settings file is (above) |
| `XDG_RUNTIME_DIR` | where the single-instance lock is: `$XDG_RUNTIME_DIR/elrs-wifi-joystick.lock`, else `/run/user/<uid>/elrs-wifi-joystick.lock` when that directory is yours (what it is in a login session: `su -`, `sudo -u` or cron find the same lock), else `/tmp/elrs-wifi-joystick-<uid>.lock` |
| `NO_COLOR`, `TERM` | no colors when `NO_COLOR` is set to a non-empty value, or `TERM` is unset or `dumb` |
| `LC_ALL`, `LC_CTYPE`, `LANG` | a UTF-8 locale gets block-character bars in the status line |

## Exit codes and signals

| code | |
|---|---|
| 0 | normal end (`q`, `quit`, Ctrl+C, SIGTERM, SIGHUP) |
| 1 | something fatal (e.g. a thread could not start) |
| 2 | bad command-line options |
| 3 | another copy already runs for this user (both would need UDP port 11000 and the joystick) |
| 128 + N | ended at once by the third quit signal N (e.g. 130 for SIGINT), below |

SIGINT, SIGTERM and SIGHUP end the program cleanly: the joystick is centered and removed, the socket
closed. If it hangs while quitting, the third of these signals ends it at once, with exit code
128 + the signal number. SIGHUP and SIGINT that were ignored when it started stay ignored (`nohup`, a
script's `cmd &`). SIGPIPE is ignored. Ctrl+Z (SIGTSTP) centers the joystick and stops listening before
the program stops. Without `--daemon`, a stdout pipe whose reader went away ends it (exit code 0).

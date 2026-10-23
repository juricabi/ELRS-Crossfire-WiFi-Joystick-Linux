# Web API

The program serves its page and this JSON API on `http://127.0.0.1:11000/` by default (`--web`).
Every response has `Cache-Control: no-store` and `Connection: close`; JSON responses are
`application/json; charset=utf-8`. Numbers are plain JSON numbers; strings are UTF-8 with control
characters escaped.

## Security (every request)

| check | failure |
|---|---|
| `Host` is `localhost`, `127.0.0.1`, `[::1]` or an IPv4 literal, with an optional `:port` | 403 `{"ok":false,"error":"host not allowed"}`; for `GET`/`HEAD` of `/` and `/index.html` (a person who opened the page by a host name) a short `text/html` page that says to open it by its IP address (this server's URL, never the name that was sent) |
| `Origin`, when present (and not `null`), is `http://<the same host:port as Host>` | 403 `{"ok":false,"error":"origin not allowed"}` |
| POST has `Content-Type: application/json` (parameters like `; charset=utf-8` allowed) | 415 `{"ok":false,"error":"use Content-Type: application/json"}` |
| request head ≤ 8 KiB, body ≤ 4 KiB | 413 |
| `POST /api/config` has a `Content-Length`; no `Transfer-Encoding` anywhere (chunked bodies are refused) | 411 |
| a well-formed HTTP/1.0 or 1.1 request (HTTP/1.1: exactly one `Host`; one `Content-Length` value) | 400 (505 for other versions) |
| method allowed for the path | 405 with `Allow` |

No `Access-Control-*` headers are ever sent. Every error is JSON `{"ok":false,"error":"..."}` except a 404
outside `/api/`. Connections: one request each; a request must arrive within 5 s of connecting (else 408,
or the connection just closes when nothing came); an answer that makes no progress for 5 s is dropped.
With 32 connections open, further ones wait (they are not refused).

## `GET /`

The page (`text/html; charset=utf-8`). `GET /index.html` is the same. `HEAD` is accepted wherever `GET` is.

## `GET /api/status`

```json
{
  "version": "1.1.0",
  "instance": "1f3a-6230a4c5e1b2c",
  "seq": 1234,
  "uptime_s": 12.5,
  "running": true,
  "listening": true,
  "state": "streaming",
  "status": "streaming",
  "detail": "192.168.1.50",
  "module": { "ip": "192.168.1.50", "kind": "ELRS", "name": "Unified TX" },
  "rate_hz": 249.8,
  "jitter_ms": 0.42,
  "frames_last_s": 250,
  "nch": 16,
  "raw": [16393, 16393, 19, 16393, 0, 0, 0, 0, 32767, 0, 0, 0, 0, 0, 0, 0],
  "request": { "interval_us": 4000, "rate_hz": 250, "channels": 16 },
  "config": { "module_ip": "", "port": 11000, "http_port": 80, "interval_us": 4000, "channels": 16 },
  "counters": { "frames": 3120, "rx": 3125, "beacons": 2, "malformed": 0, "dropped_filter": 0,
                "dropped_other": 0, "posts": 1, "posts_ok": 1, "posts_failed": 0, "restarts": 0,
                "gaps": 0, "gap_max_ms": 0 },
  "joystick": { "enabled": true, "ok": true, "name": "ELRS Crossfire WiFi Joystick",
                "device": "/dev/input/event7", "buttons": true, "error": "" },
  "log_seq": 42
}
```

- `instance`: the same text for the whole run of the program, another one after it restarted (compare it,
  not `uptime_s`, to know: then read `/api/config` again and start the log over from `since=0`).
- `state`: `stopped` | `searching` | `streaming` | `error` (wifi_proto). `status`: the one-line text of
  `engine.h` (`eng_snapshot_t.status`). `module`: `null` when not locked; `kind`: `ELRS` | `Crossfire` |
  `WiFi` (not identified by a beacon yet); `name`: the ELRS device name or `""`.
- `raw`: always 16 numbers (0 beyond `nch`). `nch` 0 = no frame yet.
- `counters.posts`, `posts_ok`, `posts_failed`: activation POSTs (a restart's `joystick_end` and
  `joystick_begin` are one); a stop's release (`joystick_end` alone) counts in none of them: its result is
  in the log.
- `rate_hz` / `jitter_ms`: one decimal / two decimals. `request.rate_hz` = round(1e6 / interval_us).

## `GET /api/config`

```json
{
  "config": { "module_ip": "", "port": 11000, "http_port": 80, "interval_us": 4000, "channels": 16 },
  "defaults": { "module_ip": "", "port": 11000, "http_port": 80, "interval_us": 4000, "channels": 16 },
  "supported": {
    "interval_us": { "min": 1000, "max": 100000,
                     "presets": [ { "interval_us": 1000, "rate_hz": 1000 }, { "interval_us": 2000, "rate_hz": 500 },
                                  { "interval_us": 3000, "rate_hz": 333 }, { "interval_us": 4000, "rate_hz": 250 },
                                  { "interval_us": 5000, "rate_hz": 200 }, { "interval_us": 6667, "rate_hz": 150 },
                                  { "interval_us": 10000, "rate_hz": 100 }, { "interval_us": 20000, "rate_hz": 50 } ] },
    "channels": { "min": 4, "max": 16 },
    "port": { "min": 1, "max": 65535 },
    "http_port": { "min": 1, "max": 65535 }
  },
  "crossfire": { "channels": 16, "rate_hz": 90, "note": "Crossfire ignores the rate and channel count" }
}
```

## `POST /api/config`

Body: a JSON object with any of `module_ip` (string), `port`, `http_port`, `interval_us`, `channels`
(integers); missing keys keep their current value, also when another client (the terminal, another
page) changed them meanwhile: only the keys named change (`eng_update_config`). Applied live and saved
to the settings file. A `port` change while running is a stop and a start: what streams to the program is
released first, as by `POST /api/stop` (to the new `http_port` when the same request changes it; the log
says how it went), then it listens on the new port.

- 200 `{"ok":true,"config":{...},"warning":""}`: `config` is the whole config now in effect, as the
  program has it (`module_ip` in its plain form: `" 10.0.0.5\r\n"` answers `"10.0.0.5"`); `warning`: e.g.
  the settings file could not be written.
- 400 `{"ok":false,"error":"channels: 3 is not supported - use 4 to 16"}` for an unsupported value,
  `{"ok":false,"error":"unknown key \"foo\""}`, `{"ok":false,"error":"channels must be a number"}`
  (`... must be a whole number` for 16.0 or 1e3, `module_ip must be a string`, `channels is given twice`),
  `{"ok":false,"error":"the body must be a JSON object"}`, `{"ok":false,"error":"the body is not valid JSON"}`.
  Nothing changes on any error. `{}` changes nothing and answers the current config.

## `POST /api/start`, `POST /api/stop`, `POST /api/restart`

Body ignored (send `{}`). `start`/`stop` listen or not. `stop` also releases the module (rule 14 of
[DESIGN.md](DESIGN.md#2-protocol-what-the-core-does), [USAGE.md](USAGE.md#using-the-module-from-another-app)):
`action=joystick_end` alone to the module streaming to the program (and to the others rule 14 names: one
whose frames it ignores, one asked to stream less than 3 s ago), so it broadcasts again and another app can
find it; never to a module known as Crossfire by its beacon. 200 `{"ok":true}` comes once the stop is
taken, before the module answers; `/api/status` then says `"state": "stopped"`, and the log says what the
module did: "Releasing <ip>: joystick_end, so it stops streaming and broadcasts again", then "Released <ip>: it
broadcasts again, any app can connect to it" or "Releasing <ip> failed: <why> - it may stream on until its
WiFi or power is cycled"; for Crossfire "Not releasing <ip>: Crossfire broadcasts while streaming, other apps
find it as it is". A module gets 2 s to answer ("... failed: timed out after 2 s - ..."). The next `start`
finds the module by its next broadcast, or at once with a module IP set (within those 2 s when the release
has no answer yet: the requests to the modules go out one at a time).
`restart` re-sends our request to the module (`eng_restart_stream`). 200 `{"ok":true}` once the command is
taken; what the module gets is in the log:
a restart (`joystick_end`, then `joystick_begin`); nothing for a locked Crossfire; for a locked module that
may be Crossfire (16 channels, no beacon from it yet) the restart waits until it has streamed 20 s without
a Crossfire beacon; the module IP, not locked and not known as ELRS by its beacon (no beacon yet, or
Crossfire), only a `joystick_begin`; nothing when no module is locked and no module IP is set, nor while
stopped ("Not running - nothing to restart") or not listening yet ("Not listening - nothing to restart").
A restart that fails is sent again every 5 s while the module streams, 3 sends in all (rule 6 of
[DESIGN.md](DESIGN.md#2-protocol-what-the-core-does)).

## `GET /api/log?since=N`

```json
{ "seq": 42, "lines": [ { "seq": 41, "time_ms": 1729590000123, "text": "Listening for joystick data on UDP 11000" } ],
  "instance": "1f3a-6230a4c5e1b2c" }
```

Lines with `seq > since` (default 0), oldest first, at most 200 (the newest 200 when more). `seq` is the
newest line's seq (pass it as `since` next time); with no new lines, the program's newest seq, which is
below `since` after the program was restarted (start over from 0). `instance`: as in `/api/status` (lines
of another instance than the ones shown: start over). `time_ms`: wall clock, ms since 1970.
`since` that is not a number 0..4294967295: 400 `{"ok":false,"error":"since must be a number"}`.

## Errors

Unknown `/api/...` path: 404 `{"ok":false,"error":"not found"}`. Anything else unknown: 404 plain text.
Malformed request: 400 and the connection closes.

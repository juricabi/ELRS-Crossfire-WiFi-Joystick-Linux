#!/usr/bin/env python3
"""The web page (web/index.html) in headless Chromium, with Playwright.

Runs against the real program with fake modules (tests/fake_elrs.py) when it is built (env BIN, default
build/native/elrs-wifi-joystick), else against tests/web_demo.c (src/web.c + a fake engine), which it
builds itself. --demo / --real force one. Checks: the page loads without console errors, the bars
follow the sticks (CH9-16 as buttons with the joystick's hysteresis), the rate and channel selects
offer exactly the supported values, applying a change works and errors show inline, Crossfire
disables rate/channels with its note, start/stop (Stop releases the module, and says how that went, from
the log: waiting, released, refused; not Crossfire; not lines older than the press), restart, the log panel
(autoscroll unless scrolled up), polling only while visible, phone width without sideways scrolling, light
and dark, keyboard use, no layout shift while numbers change, the footer's links (a new tab; nothing is
loaded from them or from anywhere but the program), and the lost-connection banner.

    python3 tests/test_ui.py [--demo | --real] [-k name]
Environment: BIN (the program), CC (for the demo), CHROMIUM (a Chromium binary, when Playwright's own
download is missing), HEADED=1 (watch it).
"""
import argparse
import glob
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request

from playwright.sync_api import expect, sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import fake_elrs  # noqa: E402

PRESETS = [1000, 2000, 3000, 4000, 5000, 6667, 10000, 20000]
# the footer's links: text, href
FOOT_LINKS = [("Project on GitHub", "https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Linux"),
              ("juricabi", "https://github.com/juricabi"), ("Support on Ko-fi", "https://ko-fi.com/juricabi")]
RATE_TEXTS = ["1000 Hz (1 ms)", "500 Hz (2 ms)", "333 Hz (3 ms)", "250 Hz (4 ms) – default", "200 Hz (5 ms)",
              "150 Hz (6.667 ms)", "100 Hz (10 ms)", "50 Hz (20 ms)"]


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def http(base, path, body=None, timeout=3.0):
    data = None if body is None else json.dumps(body).encode()
    rq = urllib.request.Request(base.rstrip("/") + path, data=data, method="POST" if body is not None else "GET")
    if body is not None:
        rq.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(rq, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def wait_until(fn, timeout, what):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            last = fn()
            if last:
                return last
        except (OSError, ValueError, KeyError) as e:
            last = e
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what} (last: {last!r})")


# ------------------------------------------------------------------------------------ harnesses
class Demo:
    """src/web.c + tests/web_demo.c's fake engine, driven through its stdin commands."""
    name = "demo"
    slow = 1.0

    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="webdemo-")
        gen = os.path.join(self.tmp, "gen")
        os.makedirs(gen)
        page = open(os.path.join(ROOT, "web", "index.html"), "rb").read()
        with open(os.path.join(gen, "webpage.h"), "w") as f:
            f.write("static const unsigned char WEBPAGE[] = {%s};\n#define WEBPAGE_LEN (sizeof WEBPAGE)\n"
                    % ",".join(str(b) for b in page))
        exe = os.path.join(self.tmp, "web_demo")
        cc = os.environ.get("CC", "cc").split()
        subprocess.check_call(cc + ["-std=gnu99", "-O1", "-g", "-D_GNU_SOURCE", "-DAPP_VERSION=\"ui-test\"",
                                    "-I" + os.path.join(ROOT, "core"), "-I" + os.path.join(ROOT, "src"), "-I" + gen,
                                    "-o", exe, os.path.join(HERE, "web_demo.c"), os.path.join(ROOT, "src", "web.c"),
                                    os.path.join(ROOT, "core", "wifi_proto.c"), os.path.join(ROOT, "core", "wifi_net.c"),
                                    "-lpthread", "-lm"])
        self.p = subprocess.Popen([exe, "--port", "0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                  bufsize=1)
        line = self.p.stdout.readline().strip()
        assert line.startswith("URL "), line
        self.url = line[4:]
        self.lock = threading.Lock()

    def cmd(self, c):
        with self.lock:
            self.p.stdin.write(c + "\n")
            self.p.stdin.flush()
            r = self.p.stdout.readline().strip()
        assert r == "ok", f"{c!r} -> {r!r}"

    def wait_streaming(self):
        pass

    def set_raw(self, values):
        for ch, v in values.items():
            self.cmd(f"raw {ch} {v}")

    def crossfire(self, on):
        self.cmd("kind xf" if on else "kind elrs")

    def make_log_lines(self, n, tag):
        for i in range(n):
            self.cmd(f"log {tag} {i}")
        return tag

    def module_ip(self):
        return "127.0.0.2"

    def identify(self):
        pass

    def restart(self, power_cycle=True):
        """as if the program restarted: a new instance, the log and the uptime from 0"""
        self.cmd("restart")

    def kill(self):
        self.p.kill()
        self.p.wait(5)

    def close(self):
        if self.p.poll() is None:
            try:
                self.p.stdin.write("quit\n")
                self.p.stdin.flush()
                self.p.wait(3)
            except (OSError, subprocess.TimeoutExpired):
                self.p.kill()
                self.p.wait(3)
        shutil.rmtree(self.tmp, ignore_errors=True)    # the demo's build


class Real:
    """The program (--no-joystick --no-config) with a fake ELRS module on 127.0.0.2 (and a Crossfire one on 127.0.0.3)."""
    name = "real"
    slow = 3.0

    def __init__(self, binary):
        self.udp = free_port(socket.SOCK_DGRAM)
        self.http_port = free_port()
        self.web = free_port()
        self.elrs = fake_elrs.FakeModule(ip="127.0.0.2", http_port=self.http_port, kind="elrs", stream_port=self.udp,
                                         beacon_to=("127.0.0.1", self.udp)).start()
        self.xf = None
        self.out = []
        self.binary = binary
        self.url = f"http://127.0.0.1:{self.web}/"
        self._run()

    def _run(self):
        self.p = subprocess.Popen([self.binary, "--no-joystick", "--no-config", "--daemon", "--port", str(self.udp),
                                   "--http-port", str(self.http_port), "--web", f"127.0.0.1:{self.web}"],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        threading.Thread(target=self._read, args=(self.p,), daemon=True).start()
        try:
            wait_until(lambda: http(self.url, "/api/status")[0] == 200, 8, "the web page of the program")
        except AssertionError:
            self.close()
            raise AssertionError("the program did not serve its page; its output:\n" + "".join(self.out[-30:]))

    def _read(self, p):
        for line in p.stdout:
            self.out.append(line)

    def kind(self):
        return (http(self.url, "/api/status")[1]["module"] or {}).get("kind")

    def identify(self):
        """the ELRS module known by its beacon (it beacons only while not streaming): powered off and on when the
        program does not know it (a new program, while it streamed on)"""
        if self.kind() == "ELRS":
            return
        with self.elrs.lock:
            self.elrs.active = False
        wait_until(lambda: self.kind() == "ELRS", 20, "the module identified by its beacon")

    def restart(self, power_cycle=True):
        """the program ends and starts again (a service restart, an update); power_cycle: the module too (it
        beacons again), else the program is killed (SIGKILL: no release, R14) and the module streams on to the
        new program, which then knows nothing about it"""
        if power_cycle:
            self.p.terminate()
        else:
            self.p.kill()
        self.p.wait(5)
        if power_cycle:
            with self.elrs.lock:
                self.elrs.active = False
        self._run()
        if power_cycle:
            self.wait_streaming()
            self.identify()

    def wait_streaming(self):
        wait_until(lambda: http(self.url, "/api/status")[1]["state"] == "streaming", 10, "streaming")

    def set_raw(self, values):
        with self.elrs.lock:
            for ch, v in values.items():
                self.elrs.values[ch - 1] = v

    def crossfire(self, on):
        if on:
            self.xf = fake_elrs.FakeModule(ip="127.0.0.3", http_port=self.http_port, kind="xf", stream_port=self.udp,
                                           beacon_to=("127.0.0.1", self.udp), beacon_interval=0.5).start()
            assert http(self.url, "/api/config", {"module_ip": "127.0.0.3"})[0] == 200
        else:
            assert http(self.url, "/api/config", {"module_ip": ""})[0] == 200
            if self.xf:
                self.xf.stop()
                self.xf = None

    def make_log_lines(self, n, tag):
        # each restart of an identified ELRS module logs at once (one that may be Crossfire would wait)
        self.identify()
        for _ in range(n):
            assert http(self.url, "/api/restart", {})[0] == 200
            time.sleep(0.06)
        return "Restarting the stream"

    def module_ip(self):
        return "127.0.0.2"

    def kill(self):
        self.p.terminate()
        self.p.wait(5)

    def close(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(5)
            except subprocess.TimeoutExpired:
                self.p.kill()
        time.sleep(0.2)                            # the reader thread gets the last lines
        self.elrs.stop()
        if self.xf:
            self.xf.stop()


# ------------------------------------------------------------------------------------ helpers
class Page:
    """A page with its console collected; problems() = console errors/warnings, page errors, failed requests."""

    def __init__(self, browser, url, **ctx):
        self.ctx = browser.new_context(**ctx)
        self.page = self.ctx.new_page()
        self.msgs = []
        self.requests = []
        self.page.on("console", lambda m: self.msgs.append(f"console.{m.type}: {m.text}") if m.type in ("error", "warning") else None)
        self.page.on("pageerror", lambda e: self.msgs.append(f"pageerror: {e}"))
        self.page.on("requestfailed", lambda r: self.msgs.append(f"requestfailed: {r.url} {r.failure}"))
        self.page.on("request", lambda r: self.requests.append((time.monotonic(), r.method, r.url)))
        self.page.goto(url)
        self.url = url

    def problems(self, allow=()):
        return [m for m in self.msgs if not any(re.search(a, m) for a in allow)]

    def close(self):
        self.ctx.close()


def row(page, i):
    return page.locator("#axes .row, #btns .row").nth(i)


def pct(page, i):
    return row(page, i).locator(".pct")


def wait_streaming_page(pg, h):
    expect(pg.page.locator("#bState")).to_have_text("Connected - streaming", timeout=10000 * h.slow)


# ------------------------------------------------------------------------------------ tests
def t_load(br, h):
    """loads, no console errors, the banner and the rate line"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    expect(p.locator("#banner")).to_have_class("banner st-streaming")
    expect(p.locator("#bDetail")).to_contain_text("127.0.0.2 · ELRS · Unified TX")
    expect(p.locator("#rate")).to_have_text(re.compile(r"^\d+\.\d$"))
    expect(p.locator("#jit")).to_have_text(re.compile(r"^\d+\.\d\d$"))
    expect(p.locator("#pps")).to_have_text(re.compile(r"^\d+$"))
    expect(p.locator("#ask")).to_have_text(re.compile(r"^asked 250 Hz × 16 ch – getting 2[0-9]{2} Hz × 16 ch$"))
    assert "Connected - streaming" in p.title()
    expect(p.locator("#ver")).to_have_text(re.compile(r"^elrs-wifi-joystick \S+ · 127\.0\.0\.1:\d+$"))
    expect(p.locator("#log")).to_contain_text("Listening for joystick data on UDP", timeout=3000)
    # accessible names of the controls
    for label in ("Module IP", "Rate", "Channels", "UDP port"):
        expect(p.get_by_label(label, exact=True)).to_have_count(1)
    expect(p.get_by_role("button", name="Apply")).to_be_enabled()
    expect(p.get_by_role("meter", name="CH1 Roll")).to_have_count(1)
    expect(p.get_by_role("meter", name="Button 8 (CH16)")).to_have_count(1)
    p.wait_for_timeout(1500)
    assert not pg.problems(), pg.problems()
    pg.close()


def t_bars(br, h):
    """the bars follow the sticks; CH9-16 are buttons with the joystick's hysteresis"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    h.set_raw({1: 0, 2: 32767, 3: 16384, 4: 8192, 9: 30000, 10: 30000, 11: 22000, 12: 0})
    for i, t in ((0, "0%"), (1, "100%"), (2, "50%"), (3, "25%"), (8, "91%"), (9, "91%"), (10, "67%"), (11, "0%")):
        expect(pct(p, i)).to_have_text(t, timeout=3000)
    scale = lambda i: float(re.search(r"scaleX\(([\d.]+)\)", row(p, i).locator(".fill").get_attribute("style")).group(1))
    assert scale(0) == 0 and scale(1) == 1 and abs(scale(2) - 0.5) < 0.001 and abs(scale(3) - 0.25) < 0.001
    mark = lambda i: row(p, i).locator(".mark")
    expect(mark(8)).to_have_class("mark on")
    expect(mark(9)).to_have_class("mark on")
    expect(mark(10)).to_have_class("mark")           # 67 % from below: not pressed yet
    expect(mark(11)).to_have_class("mark")
    expect(p.get_by_role("meter", name="Button 1 (CH9)")).to_have_attribute("aria-valuetext", "91%, pressed")
    expect(p.get_by_role("meter", name="CH2 Pitch")).to_have_attribute("aria-valuenow", "100")
    # hysteresis: 22000 keeps a pressed button pressed; 25000 presses; below 20480 releases
    h.set_raw({10: 22000, 11: 25000})
    expect(pct(p, 9)).to_have_text("67%", timeout=3000)
    expect(pct(p, 10)).to_have_text("76%", timeout=3000)
    expect(mark(9)).to_have_class("mark on")
    expect(mark(10)).to_have_class("mark on")
    h.set_raw({10: 20000})
    expect(pct(p, 9)).to_have_text("61%", timeout=3000)
    expect(mark(9)).to_have_class("mark")
    # they keep following
    for v, t in ((1000, "3%"), (31000, "94%"), (16393, "50%")):
        h.set_raw({1: v})
        expect(pct(p, 0)).to_have_text(t, timeout=3000)
    h.set_raw({ch: (16393 if h.name == "real" else "auto") for ch in range(1, 17)})
    assert not pg.problems(), pg.problems()
    pg.close()


def t_selects(br, h):
    """the rate and channel selects offer exactly the supported values; a custom rate shows as one more"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    opts = lambda sel: p.eval_on_selector_all(sel + " option", "os => os.map(o => [o.value, o.textContent])")
    expect(p.locator("#fRate option")).to_have_count(8)
    assert opts("#fRate") == [[str(v), t] for v, t in zip(PRESETS, RATE_TEXTS)], opts("#fRate")
    assert opts("#fCh") == [[str(n), f"{n}" + (" – default" if n == 16 else "")] for n in range(4, 17)], opts("#fCh")
    expect(p.locator("#fRate")).to_have_value("4000")
    expect(p.locator("#fCh")).to_have_value("16")
    expect(p.locator("#fPort")).to_have_value(str(getattr(h, "udp", 11000)))
    # a rate set elsewhere (CLI --interval 2500): one more option, selected; gone when back on a preset
    assert http(h.url, "/api/config", {"interval_us": 2500})[0] == 200
    expect(p.locator("#fRate")).to_have_value("2500", timeout=3000)
    expect(p.locator("#fRate option[value='2500']")).to_have_text("400 Hz (2.5 ms) – custom")
    expect(p.locator("#fRate option")).to_have_count(9)
    expect(p.locator("#ask")).to_contain_text("asked 400 Hz × 16 ch")
    assert http(h.url, "/api/config", {"interval_us": 4000})[0] == 200
    expect(p.locator("#fRate option")).to_have_count(8, timeout=3000)
    expect(p.locator("#fRate")).to_have_value("4000")
    assert not pg.problems(), pg.problems()
    pg.close()


def t_apply(br, h):
    """applying a change: the program gets it, the request line follows, the stream restarts"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    before = http(h.url, "/api/status")[1]["counters"]["restarts"]
    p.select_option("#fRate", "2000")
    p.select_option("#fCh", "8")
    expect(p.locator("#revert")).to_be_visible()
    p.get_by_role("button", name="Apply").click()
    expect(p.locator("#cfgMsg")).to_have_text("Applied.", timeout=3000)
    expect(p.locator("#cfgMsg")).to_have_class("msg ok")
    cfg = http(h.url, "/api/config")[1]["config"]
    assert cfg["interval_us"] == 2000 and cfg["channels"] == 8, cfg
    expect(p.locator("#ask")).to_contain_text("asked 500 Hz × 8 ch", timeout=3000)
    expect(p.locator("#ask")).to_have_text(re.compile(r"getting (4[5-9][0-9]|5[01][0-9]) Hz × 8 ch$"), timeout=8000 * h.slow)
    expect(p.locator("#revert")).to_be_hidden()
    # the bars beyond the 8 channels show no data
    expect(pct(p, 8)).to_have_text("--", timeout=3000)
    after = http(h.url, "/api/status")[1]["counters"]["restarts"]
    assert after > before, (before, after)
    p.locator("#details summary").click()
    expect(p.locator("#counters")).to_contain_text("Stream restarts")
    # back, with Enter in a field: the form submits
    p.select_option("#fRate", "4000")
    p.select_option("#fCh", "16")
    p.locator("#fPort").press("Enter")
    expect(p.locator("#cfgMsg")).to_have_text("Applied.", timeout=3000)
    expect(p.locator("#ask")).to_have_text(re.compile(r"asked 250 Hz × 16 ch – getting 2[0-9]{2} Hz × 16 ch$"), timeout=8000 * h.slow)
    # the module IP, applied with Enter, and back to discovery
    p.locator("#fIp").fill(h.module_ip())
    p.locator("#fIp").press("Enter")
    expect(p.locator("#cfgMsg")).to_have_text("Applied.", timeout=3000)
    assert http(h.url, "/api/config")[1]["config"]["module_ip"] == h.module_ip()
    p.locator("#fIp").fill("")
    p.locator("#fIp").press("Enter")
    expect(p.locator("#cfgMsg")).to_have_text("Applied.", timeout=3000)
    assert http(h.url, "/api/config")[1]["config"]["module_ip"] == ""
    expect(p.locator("#fIp")).to_have_attribute("placeholder", "found by its broadcast")
    # nothing changed
    p.get_by_role("button", name="Apply").click()
    expect(p.locator("#cfgMsg")).to_have_text("Nothing to change.")
    wait_streaming_page(pg, h)
    assert not pg.problems(), pg.problems()
    pg.close()


def t_errors(br, h):
    """errors show inline next to the field; nothing changes; a field being edited is not overwritten"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    posts = lambda: [r for r in pg.requests if r[1] == "POST" and r[2].endswith("/api/config")]
    # from the API
    p.locator("#fIp").fill("10.0.0")
    p.get_by_role("button", name="Apply").click()
    expect(p.locator("#cfgMsg")).to_have_text(re.compile(r'^module_ip: "10\.0\.0" is not an IPv4 address - use a\.b\.c\.d'), timeout=3000)
    expect(p.locator("#cfgMsg")).to_have_class("msg err")
    expect(p.locator("#fIp")).to_have_attribute("aria-invalid", "true")
    expect(p.locator("#fIp")).to_be_focused()
    assert http(h.url, "/api/config")[1]["config"]["module_ip"] == ""
    n = len(posts())
    # checked on the page: no request
    p.locator("#fIp").fill("")
    for bad in ("abc", "0", "70000", "1.5", ""):
        p.locator("#fPort").fill(bad)
        p.get_by_role("button", name="Apply").click()
        expect(p.locator("#cfgMsg")).to_have_text(f'UDP port: "{bad}" is not a port - use 1 to 65535')
        expect(p.locator("#fPort")).to_have_attribute("aria-invalid", "true")
    assert len(posts()) == n, posts()
    # the Advanced port opens its disclosure to show the error
    p.locator("#fPort").fill(str(getattr(h, "udp", 11000)))
    p.locator("#adv summary").click()
    p.locator("#fHttp").fill("x")
    p.locator("#adv summary").click()
    expect(p.locator("#fHttp")).to_be_hidden()
    p.get_by_role("button", name="Apply").click()
    expect(p.locator("#fHttp")).to_be_visible()
    expect(p.locator("#cfgMsg")).to_have_text('Module web port: "x" is not a port - use 1 to 65535')
    # undo
    p.get_by_role("button", name="Undo changes").click()
    expect(p.locator("#fHttp")).to_have_value(str(getattr(h, "http_port", 80)))
    expect(p.locator("#fHttp")).not_to_have_attribute("aria-invalid", "true")
    expect(p.locator("#cfgMsg")).to_have_text("")
    expect(p.locator("#revert")).to_be_hidden()
    # a field being edited is not overwritten by the polling; untouched ones follow other clients
    p.locator("#fIp").fill("1.2.3")
    assert http(h.url, "/api/config", {"channels": 12})[0] == 200
    expect(p.locator("#fCh")).to_have_value("12", timeout=3000)
    p.wait_for_timeout(500)
    expect(p.locator("#fIp")).to_have_value("1.2.3")
    p.get_by_role("button", name="Undo changes").click()
    expect(p.locator("#fIp")).to_have_value("")
    assert http(h.url, "/api/config", {"channels": 16})[0] == 200
    # the one console message: Chromium logs the 400 of the refused config
    probs = pg.problems(allow=(r"status of 400",))
    assert not probs, probs
    pg.close()


def t_crossfire(br, h):
    """Crossfire: rate and channels disabled with the note, no restart"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    expect(p.locator("#xfNote")).to_be_hidden()
    h.crossfire(True)
    try:
        expect(p.locator("#bDetail")).to_contain_text("Crossfire", timeout=12000 * h.slow)
        wait_streaming_page(pg, h)
        expect(p.locator("#fRate")).to_be_disabled()
        expect(p.locator("#fCh")).to_be_disabled()
        expect(p.locator("#xfNote")).to_be_visible()
        expect(p.locator("#xfNote")).to_have_text("Crossfire always sends 16 channels at ~90 Hz")
        expect(p.locator("#restart")).to_be_disabled()
        expect(p.locator("#ask")).to_contain_text("(Crossfire ignores the request)")
        expect(p.locator("#ask")).to_contain_text(re.compile(r"getting (8[5-9]|9[0-5]) Hz × 16 ch"), timeout=5000)
        expect(p.locator("#fPort")).to_be_enabled()
        expect(p.locator("#fIp")).to_be_enabled()
        # Stop: a Crossfire module is not released (it broadcasts while streaming), and the page says so
        p.locator("#startStop").click()
        xf_ip = "127.0.0.3" if h.name == "real" else "127.0.0.2"
        text = "Stopped. Not releasing " + xf_ip + ": Crossfire broadcasts while streaming, other apps find it as it is."
        if h.name == "real":
            # the ELRS module left by the switch to the Crossfire module IP still streams to us: released (R14)
            text += " Released 127.0.0.2: it broadcasts again, any app can connect to it."
        expect(p.locator("#actMsg")).to_have_text(text, timeout=3000)
        expect(p.locator("#actMsg")).to_have_class("msg")
        if h.name == "real":
            assert not h.elrs.state()["active"]
        expect(p.locator("#log")).to_contain_text("Not releasing " + xf_ip + ": Crossfire broadcasts while streaming",
                                                  timeout=3000)
        p.locator("#startStop").click()
        wait_streaming_page(pg, h)
        if h.name == "real":
            assert h.xf.npost("joystick_end") == 0
    finally:
        h.crossfire(False)
    expect(p.locator("#bDetail")).to_contain_text("ELRS", timeout=15000 * h.slow)
    expect(p.locator("#fRate")).to_be_enabled()
    expect(p.locator("#fCh")).to_be_enabled()
    expect(p.locator("#xfNote")).to_be_hidden()
    expect(p.locator("#restart")).to_be_enabled()
    assert not pg.problems(), pg.problems()
    pg.close()


def t_start_stop(br, h):
    """Stop and Start; Stop releases the module (R14), and says so once the log does"""
    h.identify()                               # (whatever ran before: an ELRS module known by its beacon)
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    ss = p.locator("#startStop")
    expect(ss).to_have_text("Stop")
    expect(p.locator("#stopHint")).to_have_text("Stop releases the module, so another app can connect to it.")
    expect(ss).to_have_accessible_description("Stop releases the module, so another app can connect to it.")
    ss.click()
    expect(p.locator("#bState")).to_have_text("Stopped", timeout=3000)
    expect(p.locator("#banner")).to_have_class("banner st-stopped")
    expect(ss).to_have_text("Start")
    expect(ss).to_be_enabled()
    expect(p.locator("#restart")).to_be_disabled()
    expect(pct(p, 0)).to_have_text("--")
    expect(p.locator("#rate")).to_have_text("--")
    # (the message shows the log lines as logged)
    expect(p.locator("#actMsg")).to_have_text("Stopped. Released 127.0.0.2: it broadcasts again, any app can connect "
                                              "to it.", timeout=3000)
    expect(p.locator("#log")).to_contain_text("Released 127.0.0.2: it broadcasts again, any app can connect to it")
    assert http(h.url, "/api/status")[1]["running"] is False
    if h.name == "real":
        assert h.elrs.npost("joystick_end") >= 1 and not h.elrs.state()["active"]
    ss.click()
    expect(ss).to_have_text("Stop", timeout=3000)
    wait_streaming_page(pg, h)                 # (real: the released module is found again by its beacon)
    expect(pct(p, 0)).not_to_have_text("--")
    # known as ELRS (its beacon), and a restart goes out at once (not after the 20 s wait for a module that may
    # be Crossfire)
    expect(p.locator("#bDetail")).to_contain_text("127.0.0.2 · ELRS · Unified TX", timeout=3000)
    before = http(h.url, "/api/status")[1]["counters"]["restarts"]
    p.get_by_role("button", name="Restart stream").click()
    expect(p.locator("#actMsg")).to_have_text("Asked the module to restart its stream.", timeout=3000)
    wait_until(lambda: http(h.url, "/api/status")[1]["counters"]["restarts"] > before, 5, "the restart counter")
    assert not pg.problems(), pg.problems()
    pg.close()


def t_stop_release(br, h):
    """Stop's message says what the log says about the release, not more: waiting for the answer, then released;
    refused (the module streams on: said so, as a warning)"""
    h.identify()                               # (whatever ran before: an ELRS module known by its beacon)
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    ss = p.locator("#startStop")
    msg = p.locator("#actMsg")

    def click():                               # (a press while the button's last request runs is ignored)
        expect(ss).not_to_have_attribute("aria-busy", "true")
        ss.click()

    # the answer takes a while: the page waits for it (real: within the 2 s a release gets)
    if h.name == "real":
        with h.elrs.lock:
            h.elrs.http_delay = 1.2
    else:
        h.cmd("release wait")
    click()
    expect(msg).to_have_text("Stopped. Releasing 127.0.0.2 (joystick_end): waiting for its answer.", timeout=3000)
    expect(msg).to_have_class("msg")
    if h.name == "demo":
        h.cmd("log Released 127.0.0.2: it broadcasts again, any app can connect to it")
    expect(msg).to_have_text("Stopped. Released 127.0.0.2: it broadcasts again, any app can connect to it.",
                             timeout=5000)
    if h.name == "real":
        assert not h.elrs.state()["active"]
        with h.elrs.lock:
            h.elrs.http_delay = 0.0
    else:
        h.cmd("release ok")
    click()
    expect(msg).to_have_text("Started.", timeout=3000)
    wait_streaming_page(pg, h)                 # (real: found again by its beacon)
    # refused: the page says so, and the module streams on
    if h.name == "real":
        with h.elrs.lock:
            h.elrs.http_status = 500
    else:
        h.cmd("release fail HTTP 500")
    try:
        click()
        expect(msg).to_have_text("Stopped. Releasing 127.0.0.2 failed: HTTP 500 - it may stream on until its WiFi or "
                                 "power is cycled.", timeout=3000)
        expect(msg).to_have_class("msg warn")
        expect(p.locator("#log")).to_contain_text("Releasing 127.0.0.2 failed: HTTP 500 - it may stream on until its "
                                                  "WiFi or power is cycled")
        if h.name == "real":
            assert h.elrs.state()["active"]
    finally:
        if h.name == "real":
            with h.elrs.lock:
                h.elrs.http_status = 200
        else:
            h.cmd("release ok")
    click()
    expect(msg).to_have_text("Started.", timeout=3000)
    expect(msg).to_have_class("msg")
    wait_streaming_page(pg, h)                 # (real: it streamed on: locked again at once)
    # the next action's message is its own: a release answered after it does not overwrite it
    if h.name == "real":
        with h.elrs.lock:
            h.elrs.http_delay = 1.5
    else:
        h.cmd("release wait")
    released = p.locator("#log div").filter(has_text="Released 127.0.0.2")
    n0 = released.count()
    try:
        click()
        expect(msg).to_have_text("Stopped. Releasing 127.0.0.2 (joystick_end): waiting for its answer.", timeout=3000)
        click()
        expect(msg).to_have_text("Started.", timeout=3000)
        if h.name == "demo":
            h.cmd("log Released 127.0.0.2: it broadcasts again, any app can connect to it")
        expect(released).to_have_count(n0 + 1, timeout=5000)
        expect(msg).to_have_text("Started.")
    finally:
        if h.name == "real":
            with h.elrs.lock:
                h.elrs.http_delay = 0.0
        else:
            h.cmd("release ok")
    wait_streaming_page(pg, h)                 # (real: found again by its beacon)
    assert not pg.problems(), pg.problems()
    pg.close()


def t_stop_log_in_flight(br, h):
    """Stop pressed while a log read is under way: what that read brings (older than the press: here an earlier
    stop's release) is not taken for this stop's, nor is a late answer to a module this stop did not release (demo
    only: its log takes any line)"""
    if h.name != "demo":
        return "skipped (demo only)"
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    ss = p.locator("#startStop")
    msg = p.locator("#actMsg")
    # the first log answer that carries 127.0.0.7 is held 1.5 s in the page (a slow link): it is in flight when
    # Stop is pressed
    p.evaluate("""() => {
      const f = window.fetch;
      window.held = 0;
      window.fetch = async (u, o) => {
        const r = await f(u, o);
        if (!window.held && String(u).startsWith('/api/log') && (await r.clone().text()).includes('127.0.0.7')) {
          window.held = 1;
          await new Promise((ok) => setTimeout(ok, 1500));
          window.held = 2;
        }
        return r;
      };
    }""")
    h.cmd("log Releasing 127.0.0.7: joystick_end, so it stops streaming and broadcasts again")
    wait_until(lambda: p.evaluate("() => window.held") == 1, 3, "the log read held")   # (the page's CSP: no eval)
    h.cmd("release wait")
    try:
        ss.click()
        expect(msg).to_have_text("Stopped. Releasing 127.0.0.2 (joystick_end): waiting for its answer.", timeout=4000)
        wait_until(lambda: p.evaluate("() => window.held") == 2, 3, "the held log read done")
        expect(p.locator("#log")).to_contain_text("Releasing 127.0.0.7: joystick_end")
        # a late answer to a module this stop did not release
        h.cmd("log Releasing 127.0.0.9 failed: HTTP 500 - it may stream on until its WiFi or power is cycled")
        expect(p.locator("#log")).to_contain_text("Releasing 127.0.0.9 failed: HTTP 500", timeout=3000)
        h.cmd("log Released 127.0.0.2: it broadcasts again, any app can connect to it")
        expect(msg).to_have_text("Stopped. Released 127.0.0.2: it broadcasts again, any app can connect to it.",
                                 timeout=3000)
        expect(msg).to_have_class("msg")
    finally:
        h.cmd("release ok")
    expect(ss).not_to_have_attribute("aria-busy", "true")
    ss.click()
    expect(msg).to_have_text("Started.", timeout=3000)
    assert not pg.problems(), pg.problems()
    pg.close()


def t_restart(br, h):
    """Restart stream: enabled while an ELRS module streams; it asks the module again"""
    h.identify()                               # (whatever ran before: an ELRS module known by its beacon)
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    before = http(h.url, "/api/status")[1]["counters"]["restarts"]
    b = p.get_by_role("button", name="Restart stream")
    expect(b).to_be_enabled()
    b.click()
    expect(p.locator("#actMsg")).to_have_text("Asked the module to restart its stream.", timeout=3000)
    expect(p.locator("#log")).to_contain_text("Restarting the stream", timeout=3000)
    wait_until(lambda: http(h.url, "/api/status")[1]["counters"]["restarts"] > before, 5, "the restart counter")
    wait_streaming_page(pg, h)
    assert not pg.problems(), pg.problems()
    pg.close()


def t_restart_deferred(br, h):
    """Restart stream of a module not identified yet (16 channels, no beacon: maybe Crossfire): the page says
    the restart waits; the program restarts it within 20 s"""
    if h.name == "demo":
        h.cmd("kind wifi")
    else:
        h.restart(power_cycle=False)           # a new program: the module streams on and sends no beacon
    try:
        pg = Page(br, h.url)
        p = pg.page
        wait_streaming_page(pg, h)
        expect(p.locator("#bDetail")).to_contain_text("127.0.0.2 · WiFi", timeout=3000)
        before = http(h.url, "/api/status")[1]["counters"]["restarts"]
        p.get_by_role("button", name="Restart stream").click()
        expect(p.locator("#actMsg")).to_have_text(re.compile(r"^The module has sent no beacon yet, so it may be Crossfire "
                                                             r"\(never sent joystick_end\): its stream restarts once it has "
                                                             r"streamed 20 s without a Crossfire beacon"), timeout=3000)
        if h.name == "real":
            expect(p.locator("#log")).to_contain_text("may be Crossfire (no beacon from it yet)", timeout=3000)
            wait_until(lambda: http(h.url, "/api/status")[1]["counters"]["restarts"] > before, 25, "the restart, 20 s on")
            expect(p.locator("#log")).to_contain_text("Restarting the stream of 127.0.0.2", timeout=3000)
        assert not pg.problems(), pg.problems()
        pg.close()
    finally:
        if h.name == "demo":
            h.cmd("kind elrs")
        else:
            h.identify()


def t_restart_keeps_unapplied(br, h):
    """the program restarts while a channel change waits for Apply: the field keeps the user's value"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    p.select_option("#fCh", "8")
    p.locator("#fPort").focus()                # (not the select being edited)
    h.restart()
    wait_streaming_page(pg, h)
    p.wait_for_timeout(1500)                   # the page sees the new instance and reads the settings again
    expect(p.locator("#fCh")).to_have_value("8")
    expect(p.locator("#revert")).to_be_visible()
    p.get_by_role("button", name="Undo changes").click()
    expect(p.locator("#fCh")).to_have_value("16")
    assert not pg.problems(allow=(r"ERR_CONNECTION_REFUSED", r"requestfailed", r"ERR_EMPTY_RESPONSE")), pg.problems()
    pg.close()


def t_log_restart(br, h):
    """the program restarted while the page was hidden, and has run longer and logged more since: the log shows
    the new run from its first line and nothing of the old one (demo only: it restarts at once)"""
    if h.name != "demo":
        return "skipped (demo only)"
    h.restart()                                # an uptime and a log both short
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    h.cmd("log old-run-marker")
    log = p.locator("#log")
    expect(log).to_contain_text("old-run-marker", timeout=3000)
    hide = """() => { Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => 'hidden' });
                      document.dispatchEvent(new Event('visibilitychange')); }"""
    show = hide.replace("'hidden'", "'visible'")
    p.evaluate(hide)
    p.wait_for_timeout(1200)                   # the last polls finish
    h.restart()
    h.cmd("log new-run-first")
    for i in range(20):
        h.cmd(f"log new-run {i}")
    p.wait_for_timeout(2500)                   # the new run is older than the old one was
    p.evaluate(show)
    expect(log).to_contain_text("new-run 19", timeout=3000)
    expect(log).to_contain_text("new-run-first")
    expect(log).not_to_contain_text("old-run-marker")
    expect(log.locator("div").first).to_contain_text("restarted")    # the new run's line 1
    assert not pg.problems(), pg.problems()
    pg.close()


def t_log(br, h):
    """the log: new lines come; it follows the end unless the reader scrolled up"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    log = p.locator("#log")
    expect(log.locator("div").first).to_be_visible(timeout=3000)
    expect(log.locator("div .t").first).to_have_text(re.compile(r"^\d\d:\d\d:\d\d  $"))
    tag = h.make_log_lines(30, "fill")
    expect(log).to_contain_text(tag, timeout=3000)
    p.wait_for_timeout(1300)
    at_end = "e => e.scrollTop + e.clientHeight >= e.scrollHeight - 8"
    assert log.evaluate("e => e.scrollHeight > e.clientHeight + 40"), "the log should overflow by now"
    assert log.evaluate(at_end), "the log should be at its end"
    n = log.locator("div").count()
    # scrolled up: new lines are added but the view stays
    log.evaluate("e => { e.scrollTop = 0; }")
    p.wait_for_timeout(200)
    tag2 = h.make_log_lines(3, "more")
    wait_until(lambda: log.locator("div").count() > n, 5, "new log lines")
    p.wait_for_timeout(300)
    assert log.evaluate("e => e.scrollTop") == 0
    expect(p.locator("#logHint")).to_have_text("scrolled up: new lines are added below")
    expect(log).to_contain_text(tag2)
    # back at the end: it follows again
    log.evaluate("e => { e.scrollTop = e.scrollHeight; }")
    expect(p.locator("#logHint")).to_have_text("")
    n = log.locator("div").count()
    h.make_log_lines(2, "last")
    wait_until(lambda: log.locator("div").count() > n, 5, "new log lines")
    p.wait_for_timeout(300)
    assert log.evaluate(at_end)
    # keyboard: the log can take focus (to scroll it with the keys)
    log.focus()
    expect(log).to_be_focused()
    assert not pg.problems(), pg.problems()
    pg.close()


def t_visibility(br, h):
    """it polls /api/status every ~100 ms while visible, and nothing while hidden"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    count = lambda t0, t1, path: sum(1 for t, m, u in pg.requests if t0 <= t < t1 and u.endswith(path))
    t0 = time.monotonic()
    p.wait_for_timeout(2000)
    t1 = time.monotonic()
    n = count(t0, t1, "/api/status")
    assert 12 <= n <= 22, f"{n} status polls in 2 s"
    nlog = sum(1 for t, m, u in pg.requests if t0 <= t < t1 and "/api/log" in u)
    assert 1 <= nlog <= 3, f"{nlog} log polls in 2 s"
    p.evaluate("""() => { Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => 'hidden' });
                          document.dispatchEvent(new Event('visibilitychange')); }""")
    p.wait_for_timeout(1200)                   # let the last ones finish
    t0 = time.monotonic()
    p.wait_for_timeout(1500)
    t1 = time.monotonic()
    assert count(t0, t1, "/api/") == 0, "polls while hidden"
    p.evaluate("""() => { Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => 'visible' });
                          document.dispatchEvent(new Event('visibilitychange')); }""")
    t0 = time.monotonic()
    p.wait_for_timeout(1000)
    t1 = time.monotonic()
    assert count(t0, t1, "/api/status") >= 5, "no polls after visible again"
    assert not pg.problems(), pg.problems()
    pg.close()


def t_phone(br, h):
    """a phone: no sideways scrolling, everything inside the screen, light and dark"""
    for scheme in ("light", "dark"):
        pg = Page(br, h.url, viewport={"width": 360, "height": 740}, is_mobile=True, has_touch=True,
                  device_scale_factor=2, color_scheme=scheme)
        p = pg.page
        wait_streaming_page(pg, h)
        p.locator("#details summary").click()
        p.locator("#adv summary").click()
        if h.name == "demo":
            h.cmd("log " + "x" * 300)                                 # one long word
            h.cmd("name " + "W" * 31)
            expect(p.locator("#log")).to_contain_text("x" * 300, timeout=3000)
        p.wait_for_timeout(500)
        sw, cw = p.evaluate("[document.documentElement.scrollWidth, document.documentElement.clientWidth]")
        assert sw <= cw, f"{scheme}: scrollWidth {sw} > {cw}"
        wide = p.evaluate("""() => [...document.querySelectorAll('body *')].filter(e => {
                                const r = e.getBoundingClientRect();
                                return r.width && (r.right > innerWidth + 0.5 || r.left < -0.5) && !e.closest('.sr');
                              }).map(e => e.tagName + '#' + e.id + '.' + e.className).slice(0, 5)""")
        assert not wide, f"{scheme}: outside the screen: {wide}"
        clipped = p.evaluate("() => [...document.querySelectorAll('.lab .nm, .lab .sub')].filter(e => e.scrollWidth > e.clientWidth + 1).map(e => e.textContent)")
        assert not clipped, f"{scheme}: labels cut: {clipped}"
        w = row(p, 0).locator(".track").bounding_box()["width"]
        assert w >= 120, f"bars too narrow: {w}"
        bg = p.evaluate("getComputedStyle(document.body).backgroundColor")
        lum = sum(int(x) for x in re.findall(r"\d+", bg)[:3])
        assert (lum < 150) if scheme == "dark" else (lum > 600), f"{scheme}: background {bg}"
        size = p.locator("#startStop").bounding_box()
        assert size["height"] >= 36, size                              # big enough to tap
        if h.name == "demo":
            h.cmd("name Unified TX")
        assert not pg.problems(), pg.problems()
        pg.close()


def t_keyboard(br, h):
    """every control can be reached with Tab, in order"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    seen = []
    for _ in range(14):
        p.keyboard.press("Tab")
        seen.append(p.evaluate("() => { const e = document.activeElement; return e.id || (e.tagName === 'SUMMARY' ? 'summary:' + e.parentElement.id : e.tagName); }"))
    want = ["startStop", "restart", "fIp", "fRate", "fCh", "fPort", "summary:adv", "apply", "summary:details", "log"]
    got = [x for x in seen if x in want]
    assert got[:len(want)] == want, seen
    # Space on Stop works like a click
    p.locator("#startStop").focus()
    p.keyboard.press("Space")
    expect(p.locator("#startStop")).to_have_text("Start", timeout=3000)
    expect(p.locator("#startStop")).to_be_focused()        # a pressed button keeps the focus
    p.keyboard.press("Space")
    expect(p.locator("#startStop")).to_have_text("Stop", timeout=3000)
    wait_streaming_page(pg, h)
    # the restart button and Apply keep it too
    p.locator("#restart").focus()
    p.keyboard.press("Enter")
    expect(p.locator("#actMsg")).to_have_text("Asked the module to restart its stream.", timeout=3000)
    expect(p.locator("#restart")).to_be_focused()
    p.locator("#apply").focus()
    p.keyboard.press("Enter")
    expect(p.locator("#cfgMsg")).to_have_text("Nothing to change.", timeout=3000)
    expect(p.locator("#apply")).to_be_focused()
    assert not pg.problems(), pg.problems()
    pg.close()


def t_no_shift(br, h):
    """no layout shift while the numbers change"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    p.wait_for_timeout(500)
    js = """() => [...document.querySelectorAll('.track, #rate, #jit, #pps, #startStop, #fIp, #log, .pct')]
                   .map(e => { const r = e.getBoundingClientRect(); return [Math.round(r.left * 10), Math.round(r.top * 10), Math.round(r.width * 10)]; })"""
    first = p.evaluate(js)
    for i in range(12):
        h.set_raw({1: (i * 7919) % 32768, 2: 32767 if i % 2 else 0, 9: 32767 if i % 2 else 0})
        p.wait_for_timeout(120)
        now = p.evaluate(js)
        assert now == first, f"layout moved at sample {i}"
    h.set_raw({1: 16393 if h.name == "real" else "auto", 2: 16393 if h.name == "real" else "auto", 9: 16393 if h.name == "real" else "auto"})
    assert not pg.problems(), pg.problems()
    pg.close()


def t_joystick(br, h):
    """the joystick panel: the device, monitor only, or the error with its fix"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    joy = p.locator("#joy")
    if h.name == "real":
        expect(joy).to_contain_text("Monitor only", timeout=3000)
    else:
        expect(joy).to_contain_text("/dev/input/event7")
        expect(joy).to_contain_text("ELRS Crossfire WiFi Joystick")
        h.cmd("joy err /dev/uinput: permission denied <b>x</b>\\nrun the install script")
        expect(joy).to_have_class("box bad", timeout=3000)
        assert joy.evaluate("e => e.textContent") == "No virtual joystick: /dev/uinput: permission denied <b>x</b>\nrun the install script"
        assert joy.evaluate("e => e.getBoundingClientRect().height") > 40      # the fix on a line of its own
        h.cmd("joy off")
        expect(joy).to_contain_text("Monitor only", timeout=3000)
        h.cmd("buttons off")
        expect(row(p, 8).locator(".nm")).to_have_text("CH9", timeout=3000)
        expect(row(p, 8).locator(".sub")).to_have_text("not a button")
        expect(p.locator("#gBtns")).to_have_text("Channels CH9–CH16")
        h.cmd("buttons on")
        h.cmd("joy ok")
        expect(row(p, 8).locator(".nm")).to_have_text("Button 1", timeout=3000)
        expect(joy).to_contain_text("/dev/input/event7", timeout=3000)
    assert not pg.problems(), pg.problems()
    pg.close()


def t_states(br, h):
    """banner colors and texts for every state (demo only: the fake engine can be told)"""
    if h.name != "demo":
        return "skipped (demo only)"
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    for st, text, title in (("searching", "connecting to 10.0.0.1", "Searching for module…"),
                            ("error", "UDP port 11000 in use", "Error"), ("streaming", "streaming", "Connected - streaming")):
        h.cmd(f"state {st} {text}")
        expect(p.locator("#bState")).to_have_text(title, timeout=3000)
        expect(p.locator("#banner")).to_have_class(f"banner st-{st}")
        if st != "streaming":
            expect(p.locator("#bDetail")).to_contain_text(text)
    h.cmd("present off")
    h.cmd("present on")
    h.cmd("state streaming streaming")
    # a module IP set and nothing found: restart still possible (it is sent to the module IP)
    h.cmd("present off")
    assert http(h.url, "/api/config", {"module_ip": "10.0.0.9"})[0] == 200
    h.cmd("state searching connecting to 10.0.0.9")
    expect(p.locator("#bDetail")).to_have_text("connecting to 10.0.0.9", timeout=3000)
    expect(p.locator("#restart")).to_be_enabled()
    # running but the UDP port cannot be bound: nothing could be sent (the program would only log "Not listening")
    h.cmd("state error UDP port 11000 in use")
    expect(p.locator("#restart")).to_be_disabled(timeout=3000)
    h.cmd("state searching connecting to 10.0.0.9")
    expect(p.locator("#restart")).to_be_enabled(timeout=3000)
    assert http(h.url, "/api/config", {"module_ip": ""})[0] == 200
    h.cmd("state searching listening")
    expect(p.locator("#restart")).to_be_disabled(timeout=3000)
    h.cmd("present on")
    h.cmd("state streaming streaming")
    wait_streaming_page(pg, h)
    assert not pg.problems(), pg.problems()
    pg.close()


def t_counters(br, h):
    """the details: each counter under its own label (frames without stick data vs. another module's)"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    p.locator("#details summary").click()
    s = http(h.url, "/api/status")[1]["counters"]
    value = lambda label: p.locator("#counters div").filter(has=p.locator("dt", has_text=label)).locator("dd")
    expect(value("Ignored (no stick data)")).to_have_text(str(s["dropped_filter"]), timeout=3000)
    expect(value("Ignored (other module)")).to_have_text(str(s["dropped_other"]), timeout=3000)
    assert not pg.problems(), pg.problems()
    pg.close()


def t_footer(br, h):
    """the footer: the version, the project, author and Ko-fi links (a new tab, no opener, no referrer, the
    page's focus ring); the page loads nothing but from the program"""
    pg = Page(br, h.url)
    p = pg.page
    # never the internet: a stub answers what is not the program (only a link that is followed may ask)
    outside = []

    def stub(route):
        outside.append((route.request.url, route.request.headers.get("referer")))
        route.fulfill(status=200, content_type="text/html", body="<!doctype html><title>stub</title>")
    pg.ctx.route(re.compile(r"^(?!http://127\.0\.0\.1:)"), stub)
    wait_streaming_page(pg, h)
    version = http(h.url, "/api/status")[1]["version"]
    expect(p.locator("#ver")).to_have_text(f"elrs-wifi-joystick {version} · {urllib.parse.urlsplit(h.url).netloc}")
    links = p.locator("#foot a")
    expect(links).to_have_count(len(FOOT_LINKS))
    got = links.evaluate_all("as => as.map(a => [a.textContent, a.getAttribute('href'), a.target, a.rel])")
    assert got == [[t, u, "_blank", "noopener noreferrer"] for t, u in FOOT_LINKS], got
    for t, _ in FOOT_LINKS:
        expect(p.get_by_role("link", name=t, exact=True)).to_have_accessible_description("opens in a new tab")
    # the keyboard: after the log, Tab reaches each one, with the outline of the rest of the page
    p.locator("#log").focus()
    for t, u in FOOT_LINKS:
        p.keyboard.press("Tab")
        assert p.evaluate("document.activeElement.href") == u, (t, p.evaluate("document.activeElement.outerHTML"))
        ring = p.evaluate("() => { const s = getComputedStyle(document.activeElement); return [s.outlineStyle, s.outlineWidth]; }")
        assert ring == ["solid", "2px"], (t, ring)
        # a link inside a sentence ("Made by juricabi"): the ring stays clear of the text before it
        clear = p.evaluate("""() => {
            const a = document.activeElement, t = a.previousSibling, s = getComputedStyle(a);
            const n = t && t.nodeType === Node.TEXT_NODE ? t.textContent.trimEnd().length : 0;
            if (!n) return null;
            const r = document.createRange();
            r.setStart(t, n - 1); r.setEnd(t, n);
            return a.getBoundingClientRect().left - parseFloat(s.outlineOffset) - parseFloat(s.outlineWidth)
                   - r.getBoundingClientRect().right; }""")
        assert clear is None or clear >= 1.5, (t, clear)
    # nothing came from anywhere else so far (no image, font or script of GitHub or Ko-fi)
    p.wait_for_timeout(500)
    assert not outside, outside
    other = [u for _, _, u in pg.requests if not u.startswith(h.url)]
    assert not other, other
    # a click opens a new tab, without this page as its opener nor its address as the referrer; this page stays
    with pg.ctx.expect_page() as new:
        p.get_by_role("link", name="Support on Ko-fi").click()
    tab = new.value
    tab.wait_for_load_state()
    assert tab.url == "https://ko-fi.com/juricabi", tab.url
    assert tab.evaluate("window.opener") is None
    assert outside == [("https://ko-fi.com/juricabi", None)], outside
    tab.close()
    assert p.url == h.url, p.url
    wait_streaming_page(pg, h)
    assert not [u for _, _, u in pg.requests if not u.startswith(h.url)]
    assert not pg.problems(), pg.problems()
    pg.close()


def t_offline(br, h):
    """the program goes away: the banner says so (last: it ends the program)"""
    pg = Page(br, h.url)
    p = pg.page
    wait_streaming_page(pg, h)
    h.kill()
    expect(p.locator("#bState")).to_have_text("No connection", timeout=5000)
    expect(p.locator("#banner")).to_have_class("banner st-offline")
    expect(p.locator("#startStop")).to_be_disabled()
    expect(pct(p, 0)).to_have_text("--")
    probs = pg.problems(allow=(r"ERR_CONNECTION_REFUSED", r"requestfailed"))
    assert not probs, probs
    pg.close()


TESTS = [t_load, t_bars, t_selects, t_apply, t_errors, t_crossfire, t_start_stop, t_stop_release, t_stop_log_in_flight,
         t_restart, t_log, t_visibility, t_phone, t_keyboard, t_no_shift, t_joystick, t_states, t_restart_deferred,
         t_restart_keeps_unapplied, t_log_restart, t_counters, t_footer, t_offline]


def chromium(pw):
    exe = os.environ.get("CHROMIUM")
    headless = os.environ.get("HEADED") != "1"
    if exe:
        return pw.chromium.launch(executable_path=exe, headless=headless)
    try:
        return pw.chromium.launch(headless=headless)
    except Exception:          # Playwright's own download is missing: any Chromium it can drive
        for pat in ("/opt/pw-browsers/chromium-*/chrome-linux/chrome", os.path.expanduser("~/.cache/ms-playwright/chromium-*/chrome-linux/chrome")):
            for c in sorted(glob.glob(pat), reverse=True):
                return pw.chromium.launch(executable_path=c, headless=headless)
        raise


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--demo", action="store_true", help="test against tests/web_demo.c")
    g.add_argument("--real", action="store_true", help="test against the program (BIN)")
    ap.add_argument("-k", default="", help="only tests whose name contains this")
    a = ap.parse_args()
    binary = os.environ.get("BIN", os.path.join(ROOT, "build", "native", "elrs-wifi-joystick"))
    use_real = a.real or (not a.demo and os.access(binary, os.X_OK))
    h = Real(binary) if use_real else Demo()
    print(f"web page tests against {'the program ' + binary if use_real else 'the demo (src/web.c + a fake engine)'}: {h.url}")
    if use_real:
        h.wait_streaming()
    failed = 0
    try:
        with sync_playwright() as pw:
            br = chromium(pw)
            print(f"Chromium {br.version}")
            for t in TESTS:
                if a.k not in t.__name__:
                    continue
                t0 = time.monotonic()
                try:
                    r = t(br, h)
                    print(f"PASS {t.__name__} ({time.monotonic() - t0:.1f} s){' ' + r if r else ''}: {t.__doc__}")
                except Exception as e:     # noqa: BLE001 - report and go on
                    failed += 1
                    print(f"FAIL {t.__name__}: {t.__doc__}\n  {type(e).__name__}: {e}")
                    traceback.print_exc(limit=3)
                    if t is not t_offline and h.p.poll() is not None:
                        print("  the server is gone - stopping")
                        break
            br.close()
    finally:
        h.close()
    # a program built with SAN=1: its sanitizer reports (they go to its output) are failures too
    out = "".join(getattr(h, "out", []))
    if re.search(r"Sanitizer|runtime error:", out):
        failed += 1
        print("FAIL the program's output has sanitizer reports:\n" + out[-4000:])
    print(f"{failed} failed" if failed else "all passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

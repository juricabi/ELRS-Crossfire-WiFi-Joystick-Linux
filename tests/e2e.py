#!/usr/bin/env python3
"""End-to-end: the real binary (--no-joystick) against fake modules (tests/fake_elrs.py) on 127.0.0.2
and 127.0.0.3, with free random ports, a private XDG_RUNTIME_DIR and settings directory.

Covers: --version, --list-rates, --help, bad options (exit 2), discovery, streaming, the status
command, rate / interval / channels changes (joystick_end + joystick_begin seen by the module),
Crossfire (never sent joystick_end), the module IP, stop/start, a port change, stdin line commands
(a UTF-8 BOM, unknown commands, EOF), the settings file (written, re-read at the next start, the
default XDG location, --no-config), a second instance (exit 3), SIGTERM / SIGINT / SIGHUP (clean
exit 0), --daemon, the interactive keys and prompts on a pseudo terminal (and the terminal restored),
and the web API when src/web.c serves one.

Standard library only. BIN = the binary (default build/native/elrs-wifi-joystick); RUN = a prefix to
run it with, like build.sh's (e.g. RUN=qemu-aarch64-static BIN=build/aarch64/elrs-wifi-joystick).
    python3 tests/e2e.py [-v] [case ...]
"""
import fcntl
import http.client
import json
import os
import pty
import re
import resource
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import termios
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import fake_elrs  # noqa: E402

BIN = os.environ.get("BIN", os.path.join(ROOT, "build", "native", "elrs-wifi-joystick"))
RUN = os.environ.get("RUN", "").split()
if RUN and os.sep not in RUN[0]:
    RUN[0] = shutil.which(RUN[0]) or RUN[0]
CMD = RUN + [BIN]
VERBOSE = False


def free_port():
    """A port free for UDP and TCP on every address (the program's UDP port, the fakes' web servers)."""
    for _ in range(100):
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.bind(("0.0.0.0", 0))
        port = u.getsockname()[1]
        t = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            t.bind(("0.0.0.0", port))
            return port
        except OSError:
            continue
        finally:
            t.close()
            u.close()
    raise RuntimeError("no free port")


def udp_free(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("0.0.0.0", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


class Fail(Exception):
    pass


def check(cond, what):
    if not cond:
        raise Fail(what)


class Prog:
    """The binary with stdin a pipe (line commands), stdout/stderr collected line by line."""

    def __init__(self, args, env, stdin=subprocess.PIPE, preexec_fn=None):
        self.args = args
        self.p = subprocess.Popen(CMD + args, stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  env=env, preexec_fn=preexec_fn)
        self.out, self.err = [], []
        self.cv = threading.Condition()
        self.threads = [threading.Thread(target=self._read, args=(self.p.stdout, self.out), daemon=True),
                        threading.Thread(target=self._read, args=(self.p.stderr, self.err), daemon=True)]
        for t in self.threads:
            t.start()

    def _read(self, f, into):
        for raw in iter(f.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip("\n")
            if VERBOSE:
                print("    | " + line, flush=True)
            with self.cv:
                into.append(line)
                self.cv.notify_all()
        with self.cv:
            self.cv.notify_all()

    def send(self, text, raw=False):
        data = text if raw else (text + "\n").encode()
        self.p.stdin.write(data)
        self.p.stdin.flush()

    def count(self, sub, start=0):
        with self.cv:
            return sum(1 for line in self.out[start:] if sub in line)

    def lines(self):
        with self.cv:
            return list(self.out)

    def mark(self):
        with self.cv:
            return len(self.out)

    def wait_for(self, sub, timeout=8.0, start=0, n=1):
        """until sub (a substring, or a compiled regex) is on n lines from start; returns the last one"""
        end = time.monotonic() + timeout
        with self.cv:
            while True:
                hits = [line for line in self.out[start:]
                        if (sub.search(line) if hasattr(sub, "search") else sub in line)]
                if len(hits) >= n:
                    return hits[n - 1]
                left = end - time.monotonic()
                if left <= 0 or (self.p.poll() is not None and not any(t.is_alive() for t in self.threads)):
                    raise Fail("timed out waiting for %r (x%d); output so far:\n%s" %
                               (getattr(sub, "pattern", sub), n, "\n".join(self.out[-40:])))
                self.cv.wait(min(left, 0.2))

    def finish(self, sig=signal.SIGTERM, timeout=8.0):
        if self.p.poll() is None:
            self.p.send_signal(sig)
        try:
            code = self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            raise Fail("did not exit within %.0f s of signal %d" % (timeout, sig))
        for t in self.threads:
            t.join(2)
        return code


class Env:
    """A private world for one case: runtime dir (the lock), config dir, ports, fake modules."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="e2e-elrs-")
        self.run = os.path.join(self.dir, "run")
        os.mkdir(self.run, 0o700)
        self.udp = free_port()
        self.http = free_port()
        self.config = os.path.join(self.dir, "config")
        self.env = dict(os.environ, XDG_RUNTIME_DIR=self.run, XDG_CONFIG_HOME=os.path.join(self.dir, "xdg"),
                        HOME=self.dir, NO_COLOR="1", TZ="UTC")
        self.modules = []

    def module(self, ip="127.0.0.2", kind="elrs", beacons=True, interval=0.5):
        for _ in range(20):
            m = fake_elrs.FakeModule(ip=ip, http_port=self.http, kind=kind, stream_port=self.udp,
                                     beacon_to=("127.0.0.1", self.udp) if beacons else None,
                                     beacon_interval=interval).start()
            # its random UDP port must not be ours: ip:udp taken would keep the program off *:udp
            if m._udp.getsockname()[1] != self.udp:
                break
            m.stop()
        self.modules.append(m)
        return m

    def prog(self, *extra, config=True, stdin=subprocess.PIPE, preexec_fn=None):
        args = ["--no-joystick", "-p", str(self.udp), "--http-port", str(self.http), "--no-web"]
        if config:
            args += ["--config", self.config]
        return Prog(args + list(extra), self.env, stdin=stdin, preexec_fn=preexec_fn)

    def close(self):
        for m in self.modules:
            m.stop()
        shutil.rmtree(self.dir, ignore_errors=True)


def bodies(m, action=None):
    with m.lock:
        return [p["body"] for p in m.posts if p["method"] == "POST" and (action is None or action in p["body"])]


def wait_until(fn, timeout=8.0, what="condition"):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if fn():
            return
        time.sleep(0.05)
    raise Fail("timed out waiting for " + what)


def run_simple(args, env=None, timeout=10):
    r = subprocess.run(CMD + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout,
                       env=env, stdin=subprocess.DEVNULL)
    return r.returncode, r.stdout.decode(), r.stderr.decode()


# ================================================================ cases
def case_version_help_rates():
    code, out, err = run_simple(["--version"])
    check(code == 0, "--version exit %d" % code)
    want = open(os.path.join(ROOT, "VERSION")).read().strip()
    # line 1 is what install.sh and release.yml read; then the project and Ko-fi
    links = "https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Linux - Ko-fi: https://ko-fi.com/juricabi"
    check(out == "elrs-wifi-joystick %s\n%s\n" % (want, links), "--version printed %r" % out)
    code, out, _ = run_simple(["-V"])
    check(code == 0 and out.splitlines()[0] == "elrs-wifi-joystick " + want, "-V")
    code, out, _ = run_simple(["--list-rates"])
    check(code == 0, "--list-rates exit")
    for hz, us in ((1000, 1000), (500, 2000), (333, 3000), (250, 4000), (200, 5000), (150, 6667), (100, 10000),
                   (50, 20000)):
        check(re.search(r"\b%d Hz +%d us" % (hz, us), out), "--list-rates lacks %d Hz" % hz)
    check("4 to 16" in out and "(default)" in out, "--list-rates channels/default")
    code, out, _ = run_simple(["--help"])
    check(code == 0 and out.startswith("Usage: elrs-wifi-joystick"), "--help")
    for opt in ("--module", "--rate", "--interval", "--channels", "--port", "--http-port", "--web", "--no-web",
                "--no-joystick", "--no-buttons", "--name", "--config", "--no-config", "--daemon", "--list-rates"):
        check(opt in out, "--help lacks " + opt)
    check(out.endswith("\n" + links + "\n"), "--help does not end with the links: %r" % out[-200:])


def case_bad_options():
    cases = [
        (["--channels", "3"], "--channels: 3 is not supported - use 4 to 16"),
        (["--rate", "7"], "--rate: 7 is not supported"),
        (["--interval", "500"], "--interval: 500 is not supported - use 1000 to 100000"),
        (["--module", "10.0.0"], "--module: \"10.0.0\" is not an IPv4 address"),
        (["--port", "0"], "--port: 0 is not supported - use 1 to 65535"),
        (["--web", "nowhere"], "--web: \"nowhere\" is not a web address"),
        (["--bogus"], "unknown option --bogus"),
        (["--rate"], "--rate needs a value"),
        (["11000"], "unexpected argument \"11000\""),
        (["--daemon=1"], "--daemon takes no value"),
    ]
    for args, want in cases:
        code, out, err = run_simple(args)
        check(code == 2, "%s: exit %d, expected 2" % (args, code))
        lines = err.strip().splitlines()
        check(len(lines) == 1 and want in lines[0], "%s: stderr %r" % (args, err))
        check(out == "", "%s printed on stdout: %r" % (args, out))


def case_stream_and_commands():
    e = Env()
    try:
        m = e.module()
        p = e.prog()
        p.wait_for("Listening for joystick data on UDP %d" % e.udp)
        p.wait_for("Discovery beacon from ELRS module 127.0.0.2 - activating")
        p.wait_for("Activation POST -> 127.0.0.2: 200 OK")
        p.wait_for("Joystick source locked to 127.0.0.2")
        check(bodies(m)[0] == "action=joystick_begin&interval=4000&channels=16", "first POST %r" % bodies(m))
        # status: the plain line, on demand and every 2 s
        mk = p.mark()
        p.send("status")
        line = p.wait_for("streaming from ELRS 127.0.0.2 (Unified TX)", start=mk)
        check("asked 250 Hz x 16 ch" in line and "CH1-8 %" in line, "status line %r" % line)
        p.wait_for(re.compile(r"\| 2\d\d\.\d Hz, jitter"), start=mk, timeout=6)
        # the request: a restart each time (joystick_end, then joystick_begin with it)
        p.send("rate 500")
        wait_until(lambda: len(bodies(m)) >= 3, what="the restart POSTs")
        b = bodies(m)
        check(b[1] == "action=joystick_end" and b[2] == "action=joystick_begin&interval=2000&channels=16",
              "rate 500 restart %r" % b)
        p.wait_for("Restarting the stream of 127.0.0.2: 500 Hz, 16 channels")
        p.wait_for("Stream end POST -> 127.0.0.2: 200")
        wait_until(lambda: m.state()["interval_us"] == 2000, what="the module at 2000 us")
        mk = p.mark()
        p.wait_for(re.compile(r"\| (4[5-9]\d|5[0-4]\d)\.\d Hz"), start=mk, timeout=8)
        p.send("channels 8")
        wait_until(lambda: "action=joystick_begin&interval=2000&channels=8" in bodies(m), what="channels 8")
        check(bodies(m)[-2] == "action=joystick_end", "channels 8: no joystick_end first %r" % bodies(m))
        p.send("interval 6667")
        wait_until(lambda: "action=joystick_begin&interval=6667&channels=8" in bodies(m), what="interval 6667")
        p.wait_for("Asking for 150 Hz (6667 us) and 8 channels")
        p.send("restart")
        wait_until(lambda: len(bodies(m, "joystick_end")) == 4, what="restart")
        # bad values: said, nothing changes
        n = len(bodies(m))
        mk = p.mark()
        p.send("channels 3")
        p.wait_for("channels: 3 is not supported - use 4 to 16", start=mk)
        p.send("rate fast")
        p.wait_for("rate: \"fast\" is not a number", start=mk)
        p.send("rate")
        p.wait_for("rate needs a value", start=mk)
        p.send("t 10.0.0")
        p.wait_for("module IP: \"10.0.0\" is not an IPv4 address", start=mk)
        p.send("frobnicate now")
        p.wait_for("Unknown command \"frobnicate now\" - try help", start=mk)
        p.send("HELP")
        p.wait_for("Commands: start, stop", start=mk)
        p.send("channels 8")
        p.wait_for("Asking for 8 channels already", start=mk)
        p.send("rate 150")
        p.wait_for("Asking for 150 Hz (6667 us) already", start=mk)
        p.send("t")
        p.wait_for("The module is found by its broadcast already", start=mk)
        p.send("w")
        p.wait_for("No web page: it is turned off (--no-web, or web = off in the settings file)", start=mk)
        time.sleep(0.3)
        check(len(bodies(m)) == n, "a bad value changed something")
        # stop: the port is free; start: back, without a POST (the module never stopped)
        p.send("stop")
        p.wait_for("Stopped. (The module keeps broadcasting until powered off.)")
        wait_until(lambda: udp_free(e.udp), what="the UDP port free after stop")
        mk = p.mark()
        p.send("s")
        p.wait_for("Listening for joystick data on UDP %d" % e.udp, start=mk)
        p.wait_for("Joystick source locked to 127.0.0.2", start=mk)
        p.send("s")
        p.wait_for("Stopped.", start=mk)
        p.send("start")
        p.wait_for("Listening for joystick data on UDP %d" % e.udp, start=mk, n=2)
        # the settings file has what changed (not the command line's port)
        text = open(e.config).read()
        check("interval_us = 6667\n" in text and "channels = 8\n" in text, "settings file:\n" + text)
        check(not re.search(r"^(port|http_port) *=", text, re.M), "command-line values were saved:\n" + text)
        p.send("quit")
        code = p.p.wait(8)
        check(code == 0, "quit: exit %d" % code)
        check(p.count("Stopped.") >= 3, "no Stopped at quit")
    finally:
        e.close()


def case_settings_reread_and_xdg():
    e = Env()
    try:
        with open(e.config, "w") as f:
            f.write("# mine\ninterval_us = 6667\nchannels = 8\ncolour = blue\njoystick = off\n")
        m = e.module()
        p = e.prog()
        p.wait_for("!! Settings: %s line 4: unknown key \"colour\" - ignored" % e.config)
        p.wait_for("No virtual joystick (--no-joystick, or joystick = off in the settings file): monitor only")
        p.wait_for("Joystick source locked to 127.0.0.2")
        check(bodies(m)[0] == "action=joystick_begin&interval=6667&channels=8", "settings not used: %r" % bodies(m))
        # the command line wins over the file
        p.send("q")
        check(p.p.wait(8) == 0, "quit")
        m.stop()
        e.modules.remove(m)
        m = e.module()
        p = e.prog("--rate", "1000")
        p.wait_for("Joystick source locked to 127.0.0.2")
        check(bodies(m)[0] == "action=joystick_begin&interval=1000&channels=8", "command line lost: %r" % bodies(m))
        p.send("channels 12")
        p.wait_for("Asking for 1000 Hz (1000 us) and 12 channels")
        p.send("q")
        check(p.p.wait(8) == 0, "quit")
        text = open(e.config).read()
        check(text.startswith("# mine\ninterval_us = 6667\nchannels = 12\n"), "rewritten:\n" + text)
        check("colour" not in text and "joystick = off" in text, "unknown key kept or known key lost:\n" + text)
        # the default place: $XDG_CONFIG_HOME/elrs-wifi-joystick/config
        p = e.prog(config=False)
        p.wait_for("Listening for joystick data")
        p.send("channels 5")
        xdg = os.path.join(e.dir, "xdg", "elrs-wifi-joystick", "config")
        wait_until(lambda: os.path.exists(xdg) and "channels = 5" in open(xdg).read(), what="the XDG settings file")
        p.send("q")
        check(p.p.wait(8) == 0, "quit")
        # --no-config: nothing read, nothing written
        os.remove(xdg)
        p = e.prog("--no-config", config=False)
        p.wait_for("Listening for joystick data")
        p.send("channels 6")
        p.wait_for("Asking for 250 Hz (4000 us) and 6 channels")
        time.sleep(0.3)
        p.send("q")
        check(p.p.wait(8) == 0, "quit")
        check(not os.path.exists(xdg), "--no-config wrote a file")
        # an unwritable settings file: a warning, the change applies
        ro = os.path.join(e.dir, "ro")
        os.mkdir(ro, 0o500)
        p = Prog(["--no-joystick", "-p", str(e.udp), "--no-web", "--config", os.path.join(ro, "sub", "config")], e.env)
        p.wait_for("Listening for joystick data")
        p.send("channels 7")
        if os.geteuid() != 0:
            p.wait_for("Settings not saved: cannot save the settings to")
        p.wait_for("Asking for 250 Hz (4000 us) and 7 channels")
        p.send("q")
        check(p.p.wait(8) == 0, "quit")
        os.chmod(ro, 0o700)
    finally:
        e.close()


def case_crossfire_and_module_ip():
    e = Env()
    try:
        x = e.module("127.0.0.3", "xf")
        p = e.prog()
        p.wait_for("Discovery beacon from Crossfire module 127.0.0.3 - activating")
        p.wait_for("Joystick source locked to 127.0.0.3")
        p.send("rate 1000")
        p.wait_for("Crossfire module 127.0.0.3 ignores the rate and channel count")
        p.send("channels 8")
        p.send("restart")
        p.wait_for("nothing to restart")
        mk = p.mark()
        p.send("status")
        line = p.wait_for("streaming from Crossfire 127.0.0.3", start=mk)
        check("asked 1000 Hz x 8 ch" in line, "status %r" % line)
        time.sleep(0.5)
        check(bodies(x, "joystick_end") == [], "Crossfire got joystick_end: %r" % bodies(x))
        check(len(bodies(x)) == 1, "Crossfire POSTs %r" % bodies(x))
        # an ELRS module next to it; the module IP picks it
        m = e.module("127.0.0.2", "elrs")
        time.sleep(0.7)
        check(bodies(m) == [], "a second module was activated while locked: %r" % bodies(m))
        p.send("t 127.0.0.2")
        p.wait_for("Target module set to 127.0.0.2 - connecting...")
        p.wait_for("Joystick source locked to 127.0.0.2")
        check(bodies(m)[0] == "action=joystick_begin&interval=1000&channels=8", "target POST %r" % bodies(m))
        mk = p.mark()
        p.send("status")
        p.wait_for("streaming from ELRS 127.0.0.2", start=mk)
        p.send("t")
        p.wait_for("Module IP cleared - finding the module by its broadcast")
        text = open(e.config).read()
        check("module_ip =\n" in text, "module_ip not saved as discovery:\n" + text)
        # a port change: rebind, the old one free
        u2 = free_port()
        p.send("port %d" % u2)
        p.wait_for("Listening for joystick data on UDP %d" % u2)
        wait_until(lambda: udp_free(e.udp), what="the old port free")
        check(not udp_free(u2), "the new port is not bound")
        p.send("q")
        check(p.p.wait(8) == 0, "quit")
        check(bodies(x, "joystick_end") == [], "Crossfire got joystick_end in the end: %r" % bodies(x))
    finally:
        e.close()


def case_stdin_details():
    e = Env()
    try:
        p = e.prog()
        p.wait_for("Listening for joystick data")
        # a UTF-8 BOM before the first command, CRLF line ends, case, blanks
        p.send(b"\xef\xbb\xbfSTATUS\r\n  \r\n", raw=True)
        p.wait_for("listening | asked 250 Hz x 16 ch | UDP %d" % e.udp)
        check(p.count("Unknown command") == 0, "the BOM made an unknown command")
        # EOF on stdin ends the commands, not the program
        p.p.stdin.close()
        time.sleep(1.0)
        check(p.p.poll() is None, "EOF on stdin ended the program")
        code = p.finish(signal.SIGTERM)
        check(code == 0, "SIGTERM exit %d" % code)
        p.wait_for("Stopped. (The module keeps broadcasting until powered off.)")
        # stdin /dev/null from the start: runs until a signal
        p = e.prog(stdin=subprocess.DEVNULL)
        p.wait_for("Listening for joystick data")
        time.sleep(0.5)
        check(p.p.poll() is None, "</dev/null ended the program")
        check(p.finish(signal.SIGINT) == 0, "SIGINT exit")
        p = e.prog()
        p.wait_for("Listening for joystick data")
        check(p.finish(signal.SIGHUP) == 0, "SIGHUP exit")
        check(udp_free(e.udp), "the port was not freed")
        # a long line and a last line without a newline
        p = e.prog()
        p.wait_for("Listening for joystick data")
        p.send("x" * 3000)
        p.wait_for("Command line too long - ignored")
        p.send(b"quit", raw=True)
        p.p.stdin.close()
        check(p.p.wait(8) == 0, "quit at EOF")
    finally:
        e.close()


def case_single_instance():
    e = Env()
    try:
        p = e.prog()
        p.wait_for("Listening for joystick data")
        code, out, err = run_simple(["--no-joystick", "--no-web", "--no-config", "-p", str(free_port())], env=e.env)
        check(code == 3, "second instance exit %d" % code)
        check("already running (process %d)" % p.p.pid in err, "second instance said %r" % err)
        # another runtime dir = another user's world: allowed (then the UDP port is what clashes)
        env2 = dict(e.env, XDG_RUNTIME_DIR=os.path.join(e.dir, "run2"))
        os.mkdir(env2["XDG_RUNTIME_DIR"], 0o700)
        q = Prog(["--no-joystick", "--no-web", "--no-config", "-p", str(e.udp)], env2)
        q.wait_for("ERROR: cannot listen on UDP port %d - it is already in use" % e.udp)
        check(q.finish() == 0, "the second one did not exit cleanly")
        check(p.finish() == 0, "the first one did not exit cleanly")
        # after the first one ended, a new one may run
        p = e.prog()
        p.wait_for("Listening for joystick data")
        check(p.finish() == 0, "exit")
    finally:
        e.close()


def case_daemon():
    e = Env()
    try:
        e.module()
        p = e.prog("--daemon")
        line = p.wait_for("Joystick source locked to 127.0.0.2")
        check(re.match(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}[+-]\d\d:\d\d Joystick source locked", line),
              "daemon line %r" % line)
        p.send("quit")                          # stdin is ignored in daemon mode
        time.sleep(0.7)
        check(p.p.poll() is None, "--daemon read stdin")
        check(p.count("Unknown command") == 0, "--daemon answered stdin")
        check(p.finish() == 0, "daemon SIGTERM exit")
        check(all(re.match(r"^\d{4}-\d\d-\d\dT", l) for l in p.lines() if l), "a line without a timestamp")
    finally:
        e.close()


def web_req(port, method, path, body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    h = {"Host": "127.0.0.1:%d" % port}
    if body is not None:
        h["Content-Type"] = "application/json"
    h.update(headers or {})
    c.request(method, path, body=json.dumps(body) if body is not None else None, headers=h)
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, data


def case_web_api():
    e = Env()
    try:
        m = e.module()
        wp = free_port()
        p = Prog(["--no-joystick", "-p", str(e.udp), "--http-port", str(e.http), "--web", "127.0.0.1:%d" % wp,
                  "--config", e.config], e.env)
        p.wait_for("Listening for joystick data")
        line = p.wait_for(re.compile(r"Web page: |No web page: "))
        if "No web page" in line:
            print("    (skipped: %s)" % line.split("No web page: ", 1)[1])
            p.finish()
            return
        check("http://127.0.0.1:%d/" % wp in line, "web line %r" % line)
        p.wait_for("Joystick source locked to 127.0.0.2")
        st, data = web_req(wp, "GET", "/api/status")
        check(st == 200, "status %d" % st)
        s = json.loads(data)
        wait_until(lambda: json.loads(web_req(wp, "GET", "/api/status")[1])["state"] == "streaming", what="streaming")
        s = json.loads(web_req(wp, "GET", "/api/status")[1])
        check(s["module"]["ip"] == "127.0.0.2" and s["module"]["kind"] == "ELRS", "module %r" % s.get("module"))
        check(s["request"] == {"interval_us": 4000, "rate_hz": 250, "channels": 16}, "request %r" % s["request"])
        check(len(s["raw"]) == 16 and s["nch"] == 16, "raw %r" % s["raw"])
        check(s["joystick"]["enabled"] is False, "joystick %r" % s["joystick"])
        st, data = web_req(wp, "POST", "/api/config", {"channels": 10})
        check(st == 200 and json.loads(data)["ok"], "POST config %d %r" % (st, data))
        wait_until(lambda: "action=joystick_begin&interval=4000&channels=10" in bodies(m), what="web restart")
        check(bodies(m)[-2] == "action=joystick_end", "web change without joystick_end")
        st, data = web_req(wp, "POST", "/api/config", {"channels": 3})
        check(st == 400 and "4 to 16" in json.loads(data)["error"], "bad channels %d %r" % (st, data))
        # the answer is the config in effect (the module IP in its plain form), not the body's text
        st, data = web_req(wp, "POST", "/api/config", {"module_ip": "\r\n127.0.0.2\n"})
        check(st == 200 and json.loads(data)["config"]["module_ip"] == "127.0.0.2", "POST config answer %r" % data)
        check(json.loads(web_req(wp, "GET", "/api/config")[1])["config"]["module_ip"] == "127.0.0.2", "config")
        st, data = web_req(wp, "POST", "/api/config", {"module_ip": " \n "})
        check(st == 200 and json.loads(data)["config"]["module_ip"] == "", "POST config answer %r" % data)
        # the page by a host name (--web 0.0.0.0 on a Pi, opened as raspberrypi.local): what to open instead
        st, data = web_req(wp, "GET", "/", headers={"Host": "raspberrypi.local:%d" % wp})
        check(st == 403 and b"Open this page by its IP address" in data and b"http://127.0.0.1:%d/" % wp in data,
              "page by a host name: %d %r" % (st, data[:200]))
        s1 = json.loads(web_req(wp, "GET", "/api/status")[1])
        lg = json.loads(web_req(wp, "GET", "/api/log?since=0")[1])
        check(s1["instance"] and s1["instance"] == lg["instance"], "instance %r / %r" % (s1.get("instance"), lg.get("instance")))
        st, data = web_req(wp, "POST", "/api/stop", {})
        check(st == 200, "stop %d" % st)
        p.wait_for("Stopped.")
        st, data = web_req(wp, "POST", "/api/start", {})
        p.wait_for("Listening for joystick data", n=2)
        st, data = web_req(wp, "GET", "/api/log?since=0")
        lg = json.loads(data)
        check(any("Joystick source locked" in l["text"] for l in lg["lines"]), "log %r" % lg)
        check("channels = 10" in open(e.config).read(), "the web change was not saved")
        check(p.finish() == 0, "exit")
    finally:
        e.close()


def case_interactive():
    """The keys on a pseudo terminal: raw mode, the status line, a prompt, q; the terminal restored."""
    e = Env()
    try:
        m = e.module()
        pid, fd = pty.fork()
        if pid == 0:
            env = dict(e.env, TERM="xterm", LANG="C.UTF-8")
            env.pop("NO_COLOR", None)
            os.execve(CMD[0], CMD + ["--no-joystick", "-p", str(e.udp), "--http-port", str(e.http), "--no-web",
                            "--config", e.config], env)
            os._exit(127)
        before = termios.tcgetattr(fd)
        buf = bytearray()

        def read_until(pat, timeout=8.0):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                if re.search(pat, bytes(buf)):
                    return
                r, _, _ = select.select([fd], [], [], 0.1)
                if r:
                    try:
                        d = os.read(fd, 65536)
                    except OSError:
                        break
                    if not d:
                        break
                    buf.extend(d)
                    if VERBOSE:
                        sys.stdout.write(d.decode("utf-8", "replace"))
            raise Fail("pty: no %r in:\n%s" % (pat, bytes(buf[-2000:]).decode("utf-8", "replace")))

        read_until(rb"Keys: \[s\] start/stop")
        read_until(rb"Joystick source locked to 127.0.0.2")
        # the status line: bars and state, redrawn in place
        read_until("\u25cf streaming".encode())
        attrs = termios.tcgetattr(fd)
        check(not (attrs[3] & termios.ICANON) and not (attrs[3] & termios.ECHO), "not in raw mode")
        # r: a prompt in cooked mode, then 500 Hz
        os.write(fd, b"r")
        read_until(rb"Rate in Hz")
        check(termios.tcgetattr(fd)[3] & termios.ICANON, "the prompt is not in cooked mode")
        os.write(fd, b"500\n")
        wait_until(lambda: "action=joystick_begin&interval=2000&channels=16" in bodies(m), what="the r prompt")
        # c, then Esc Enter: cancelled
        n = len(bodies(m))
        os.write(fd, b"c")
        read_until(rb"Channels 4-16")
        os.write(fd, b"\x1b\n")
        time.sleep(0.5)
        check(len(bodies(m)) == n, "Esc did not cancel")
        check(not (termios.tcgetattr(fd)[3] & termios.ICANON), "raw mode not back after the prompt")
        # s: stop, s: start
        os.write(fd, b"s")
        read_until(rb"Stopped\. \(The module")
        os.write(fd, b"s")
        read_until(rb"Listening for joystick data on UDP %d[\s\S]*Listening for joystick data" % e.udp)
        os.write(fd, b"h")
        read_until(rb"Keys: s start/stop")
        # a pasted answer longer than the prompt reads: the rest of the line never becomes keys (s, q)
        stops = bytes(buf).count(b"Stopped. (The module")
        os.write(fd, b"t")
        read_until(rb"Module IP \(now")
        os.write(fd, b"x" * 300 + b"sq\n")
        read_until(rb"module IP: \"x+\.\.\.\" is not an IPv4 address")
        time.sleep(0.6)
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            buf.extend(os.read(fd, 65536))
        check(os.waitpid(pid, os.WNOHANG) == (0, 0), "the rest of a long prompt line was run as keys (it quit)")
        check(bytes(buf).count(b"Stopped. (The module") == stops, "the rest of a long prompt line was run as keys (s)")
        check(not (termios.tcgetattr(fd)[3] & termios.ICANON), "raw mode not back after the long answer")
        os.write(fd, b"q")
        end = time.monotonic() + 8
        code = None
        while time.monotonic() < end:
            wpid, status = os.waitpid(pid, os.WNOHANG)
            if wpid:
                code = os.WEXITSTATUS(status) if os.WIFEXITED(status) else -1
                break
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    buf.extend(os.read(fd, 65536))
                except OSError:
                    pass
        check(code == 0, "q: exit %r" % code)
        after = termios.tcgetattr(fd)
        check(after[3] == before[3], "the terminal's lflag was not restored (%x, was %x)" % (after[3], before[3]))
        os.close(fd)
        text = open(e.config).read()
        check("interval_us = 2000" in text, "the prompt's rate was not saved")
    finally:
        e.close()


def case_scripted_commands():
    """stdin commands in one go see each other (like the Windows CLI's): s twice = stop, then start; status after
    stop says stopped; status after a port change shows the new port listening"""
    e = Env()
    try:
        p = e.prog()
        p.wait_for("Listening for joystick data on UDP %d" % e.udp)
        mk = p.mark()
        p.send("s\ns\nstatus")
        p.wait_for("Stopped.", start=mk)
        p.wait_for("Listening for joystick data on UDP %d" % e.udp, start=mk)
        line = p.wait_for(re.compile(r"\| asked 250 Hz x 16 ch \| UDP"), start=mk)
        check(line.split("  ", 1)[-1].startswith("listening |"), "status after s, s: %r" % line)
        mk = p.mark()
        p.send("stop\nstatus\nport %d" % e.udp)
        line = p.wait_for(re.compile(r"\| asked 250 Hz x 16 ch \| UDP"), start=mk)
        check(line.split("  ", 1)[-1].startswith("stopped |"), "status after stop: %r" % line)
        p.wait_for("The UDP port is %d already (used when listening starts)" % e.udp, start=mk)
        check(p.count("Listening on UDP port", start=mk) == 0, "said listening while stopped")
        u2 = free_port()
        mk = p.mark()
        p.send("start\nport %d\nstatus" % u2)
        line = p.wait_for(re.compile(r"\| asked 250 Hz x 16 ch \| UDP %d" % u2), start=mk)
        check(line.split("  ", 1)[-1].startswith("listening |"), "status after a port change: %r" % line)
        check(not udp_free(u2), "the new port is not bound at the status")
        mk = p.mark()
        p.send("port %d" % u2)
        p.wait_for("Listening on UDP port %d already" % u2, start=mk)
        check(p.finish() == 0, "exit")
    finally:
        e.close()


def case_inherited_signals():
    """nohup (SIGHUP ignored), a script's background job (SIGINT ignored): those stay ignored; SIGTERM ends it"""
    e = Env()
    try:
        for sig in (signal.SIGHUP, signal.SIGINT):
            p = e.prog(stdin=subprocess.DEVNULL, preexec_fn=lambda s=sig: signal.signal(s, signal.SIG_IGN))
            p.wait_for("Listening for joystick data")
            p.p.send_signal(sig)
            time.sleep(0.8)
            check(p.p.poll() is None, "signal %d ended the program although it was ignored at the start" % sig)
            check(p.count("Stopped.") == 0, "signal %d stopped it" % sig)
            check(p.finish(signal.SIGTERM) == 0, "SIGTERM exit")
        # not ignored: it ends the program, as before
        p = e.prog(stdin=subprocess.DEVNULL)
        p.wait_for("Listening for joystick data")
        check(p.finish(signal.SIGHUP) == 0, "SIGHUP exit")
    finally:
        e.close()


def case_closed_std_fds():
    """started with stdout or stdin closed: the lock file never becomes stdout or stdin"""
    e = Env()
    try:
        lock = os.path.join(e.run, "elrs-wifi-joystick.lock")
        args = CMD + ["--no-joystick", "--no-web", "--no-config", "-p", str(e.udp)]
        q = subprocess.Popen(["sh", "-c", 'exec "$0" "$@" >&-'] + args, stdin=subprocess.DEVNULL,
                             stderr=subprocess.PIPE, env=e.env)
        wait_until(lambda: not udp_free(e.udp), what="the program listening (stdout closed)")
        time.sleep(0.5)
        q.send_signal(signal.SIGTERM)
        check(q.wait(8) == 0, "exit with stdout closed")
        text = open(lock).read()
        check(re.fullmatch(r"\d+\n", text), "the lock file got the output: %r" % text[:200])
        q = subprocess.Popen(["sh", "-c", 'exec "$0" "$@" <&-'] + args, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, env=e.env)
        wait_until(lambda: not udp_free(e.udp), what="the program listening (stdin closed)")
        time.sleep(0.5)
        q.send_signal(signal.SIGTERM)
        out, _ = q.communicate(timeout=8)
        check(q.returncode == 0, "exit with stdin closed")
        check(b"Unknown command" not in out, "stdin closed: the lock file was read as commands: %r" % out[-300:])
    finally:
        e.close()


def case_stdout_reader_gone():
    """stdout a pipe whose reader goes away (| head -3): the program quits instead of running on unseen"""
    e = Env()
    try:
        q = subprocess.Popen(CMD + ["--no-joystick", "--no-web", "--no-config", "-p", str(e.udp)],
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=e.env)
        for _ in range(2):
            q.stdout.readline()
        q.stdout.close()
        try:
            code = q.wait(5)
        except subprocess.TimeoutExpired:
            q.kill()
            q.wait()
            raise Fail("still running 5 s after the reader of its stdout went away")
        check(code == 0, "exit %d" % code)
        check(udp_free(e.udp), "the port was not freed")
        # --daemon (a service): stdout is the journal's, never a reason to quit
        q = subprocess.Popen(CMD + ["--no-joystick", "--no-web", "--no-config", "-p", str(e.udp), "--daemon"],
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=e.env)
        q.stdout.readline()
        q.stdout.close()
        time.sleep(1.0)
        check(q.poll() is None, "--daemon quit when its stdout reader went away")
        q.send_signal(signal.SIGTERM)
        check(q.wait(8) == 0, "--daemon exit")
    finally:
        e.close()


def job_shell_main(slave, cr, aw, argv, env, background):
    """The shell side of JobShell, in an interpreter of its own (started by JobShell: forking the test's
    multi-threaded interpreter could deadlock the child). Already a session leader (start_new_session)."""
    try:
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        lflag0 = termios.tcgetattr(slave)[3]
        for sg in (signal.SIGTTOU, signal.SIGTTIN, signal.SIGTSTP):
            signal.signal(sg, signal.SIG_IGN)
        child = os.fork()
        if child == 0:                                 # the program
            os.setpgid(0, 0)
            if not background:
                os.tcsetpgrp(slave, os.getpgrp())
            for sg in (signal.SIGTTOU, signal.SIGTTIN, signal.SIGTSTP):
                signal.signal(sg, signal.SIG_DFL)
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))     # SIGQUIT: no core file
            for fd in (0, 1, 2):
                os.dup2(slave, fd)
            os.closerange(3, 256)
            os.execvpe(argv[0], argv, env)             # argv[0] may be RUN's emulator, found on PATH
        try:
            os.setpgid(child, child)
        except OSError:
            pass
        final = None

        def status(wait_s):
            nonlocal final
            end = time.monotonic() + wait_s
            while final is None:
                pid, st = os.waitpid(child, os.WUNTRACED | os.WNOHANG)
                if pid:
                    if os.WIFSTOPPED(st):
                        return "stopped %d" % os.WSTOPSIG(st)
                    final = ("exited %d" % os.WEXITSTATUS(st)) if os.WIFEXITED(st) else "signaled %d" % os.WTERMSIG(st)
                    break
                if time.monotonic() >= end:
                    return "running"
                time.sleep(0.02)
            return final

        for line in os.fdopen(cr):
            w = line.split()
            if w[0] == "wait":                         # until it stops or ends, at most N s
                ans = status(float(w[1]))
            elif w[0] == "take":
                os.tcsetpgrp(slave, os.getpgrp())
                ans = "ok"
            elif w[0] == "give":
                os.tcsetpgrp(slave, child)
                ans = "ok"
            elif w[0] == "signal":                     # to its process group (cont: fg/bg) or itself
                os.kill(-child if w[1] == "group" else child, int(w[2]))
                ans = "ok"
            elif w[0] == "lflag":
                ans = "%d %d" % (termios.tcgetattr(slave)[3], lflag0)
            else:
                if status(0) in ("running",) or status(0).startswith("stopped"):
                    os.kill(-child, signal.SIGKILL)
                    status(2)
                break
            os.write(aw, (ans + "\n").encode())
    finally:
        os._exit(0)


class JobShell:
    """A tiny job-control shell on a pseudo terminal: a session leader that runs the program in a process group
    of its own, in the foreground (or not: "cmd &"), and moves the terminal between the two like fg and bg."""

    def __init__(self, argv, env, background=False):
        self.master, slave = pty.openpty()
        cr, cw = os.pipe()
        ar, aw = os.pipe()
        self.shell = subprocess.Popen([sys.executable, os.path.abspath(__file__), "--job-shell",
                                       json.dumps([slave, cr, aw, list(argv), env, background])],
                                      stdin=subprocess.DEVNULL, pass_fds=(slave, cr, aw), start_new_session=True)
        os.close(slave)
        os.close(cr)
        os.close(aw)
        self.cmd_w = cw
        self.ans = os.fdopen(ar)
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        while True:
            try:
                d = os.read(self.master, 65536)
            except OSError:
                return
            if not d:
                return
            with self.lock:
                self.buf.extend(d)
            if VERBOSE:
                sys.stdout.write(d.decode("utf-8", "replace"))

    def ask(self, *words):
        os.write(self.cmd_w, (" ".join(str(x) for x in words) + "\n").encode())
        return self.ans.readline().strip()

    def keys(self, b):
        os.write(self.master, b)

    def mark(self):
        with self.lock:
            return len(self.buf)

    def text(self, start=0):
        with self.lock:
            return bytes(self.buf[start:])

    def wait_for(self, pat, start=0, timeout=8.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if re.search(pat, self.text(start)):
                return
            time.sleep(0.05)
        raise Fail("pty: no %r in:\n%s" % (pat, self.text(start)[-2000:].decode("utf-8", "replace")))

    def raw_mode(self):
        lflag, _ = (int(x) for x in self.ask("lflag").split())
        return not (lflag & termios.ICANON) and not (lflag & termios.ECHO)

    def lflag_restored(self):
        lflag, lflag0 = (int(x) for x in self.ask("lflag").split())
        return lflag == lflag0

    def close(self):
        try:
            self.ask("quit")
        except OSError:
            pass
        self.shell.wait()
        os.close(self.master)
        os.close(self.cmd_w)


def case_job_control():
    """Ctrl+Z (listening stops, the joystick centers; fg goes on), bg (runs on unseen: no reading or drawing
    on the shell's terminal, no stop on the shell's input), quitting from the background, Ctrl+backslash
    (the terminal restored), and a start with & then fg (its commands are read after fg)"""
    e = Env()
    try:
        env = dict(e.env, TERM="xterm", LANG="C.UTF-8")
        argv = CMD + ["--no-joystick", "--no-web", "--no-config", "-p", str(e.udp)]
        # ---- Ctrl+Z, fg
        j = JobShell(argv, env)
        try:
            j.wait_for(rb"Listening for joystick data on UDP %d" % e.udp)
            check(j.raw_mode(), "not in raw mode")
            j.keys(b"\x1a")
            check(j.ask("wait", 5) == "stopped %d" % signal.SIGSTOP, "Ctrl+Z did not suspend it")
            j.wait_for(rb"Stopped\. \(The module keeps broadcasting")
            j.wait_for(rb"Suspended: not listening \(the joystick is centered\)")
            check(udp_free(e.udp), "suspended but still listening (the joystick would hold the sticks)")
            check(j.lflag_restored(), "the terminal is not the shell's while suspended")
            mk = j.mark()
            j.ask("give")
            j.ask("signal", "group", signal.SIGCONT)            # fg
            j.wait_for(rb"Listening for joystick data on UDP %d" % e.udp, start=mk)
            check(j.ask("wait", 0.5) == "running", "not running after fg")
            check(j.raw_mode(), "raw mode not back after fg")
            # ---- Ctrl+Z, bg: runs (listening) in the background, silently; the shell's input is not read
            j.keys(b"\x1a")
            check(j.ask("wait", 5) == "stopped %d" % signal.SIGSTOP, "the second Ctrl+Z")
            j.ask("take")
            j.ask("signal", "group", signal.SIGCONT)            # bg
            wait_until(lambda: not udp_free(e.udp), what="listening again in the background")
            mk = j.mark()
            j.keys(b"ls\n")                                     # typed at the shell
            check(j.ask("wait", 1.5) == "running", "typing at the shell stopped the program in the background")
            drawn = j.text(mk)
            check(b"\x1b[K" not in drawn and b"asked" not in drawn, "drew over the shell's line: %r" % drawn[:200])
            # quit from the background: it ends (no stop on SIGTTOU), the terminal left to the shell
            j.ask("signal", "one", signal.SIGTERM)
            check(j.ask("wait", 5) == "exited 0", "SIGTERM in the background did not end it")
            check(udp_free(e.udp), "the port was not freed")
        finally:
            j.close()
        # ---- Ctrl+backslash: SIGQUIT ends it, the terminal restored first
        j = JobShell(argv, env)
        try:
            j.wait_for(rb"Listening for joystick data on UDP %d" % e.udp)
            check(j.raw_mode(), "not in raw mode")
            j.keys(b"\x1c")
            check(j.ask("wait", 5) == "signaled %d" % signal.SIGQUIT, "Ctrl+backslash")
            check(j.lflag_restored(), "SIGQUIT left the terminal in raw mode")
        finally:
            j.close()
        # ---- started with &, then fg: the line commands are read once it is in the foreground
        j = JobShell(argv, env, background=True)
        try:
            j.wait_for(rb"Commands on stdin")
            check(j.ask("wait", 1.0) == "running", "a background start stopped")
            j.ask("give")
            j.ask("signal", "group", signal.SIGCONT)            # fg
            mk = j.mark()
            j.keys(b"status\n")
            j.wait_for(rb"listening \| asked 250 Hz x 16 ch \| UDP %d" % e.udp, start=mk)
            j.keys(b"q\n")
            check(j.ask("wait", 5) == "exited 0", "q after fg")
        finally:
            j.close()
    finally:
        e.close()


CASES = [
    ("version_help_rates", case_version_help_rates),
    ("bad_options", case_bad_options),
    ("stream_and_commands", case_stream_and_commands),
    ("settings", case_settings_reread_and_xdg),
    ("crossfire_and_module_ip", case_crossfire_and_module_ip),
    ("stdin", case_stdin_details),
    ("single_instance", case_single_instance),
    ("daemon", case_daemon),
    ("web_api", case_web_api),
    ("interactive", case_interactive),
    ("scripted", case_scripted_commands),
    ("signals", case_inherited_signals),
    ("std_fds", case_closed_std_fds),
    ("stdout_gone", case_stdout_reader_gone),
    ("job_control", case_job_control),
]


def main(argv):
    global VERBOSE
    if argv[:1] == ["--job-shell"]:                   # JobShell's shell side
        job_shell_main(*json.loads(argv[1]))
    names = [a for a in argv if not a.startswith("-")]
    VERBOSE = "-v" in argv
    if not os.access(BIN, os.X_OK):
        print("no binary at %s (build it: ./build.sh native prog, or set BIN)" % BIN)
        return 2
    failed = []
    t0 = time.monotonic()
    for name, fn in CASES:
        if names and name not in names:
            continue
        t = time.monotonic()
        try:
            fn()
            print("PASS %s (%.1f s)" % (name, time.monotonic() - t), flush=True)
        except Exception as ex:          # noqa: BLE001 - every failure is reported, then the next case runs
            failed.append(name)
            print("FAIL %s: %s" % (name, ex), flush=True)
            if not isinstance(ex, Fail):
                traceback.print_exc()
    print("%d cases, %d failed (%.1f s)" % (len([c for c in CASES if not names or c[0] in names]), len(failed),
                                          time.monotonic() - t0))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

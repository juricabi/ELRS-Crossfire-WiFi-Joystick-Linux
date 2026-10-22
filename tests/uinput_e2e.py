#!/usr/bin/env python3
"""End to end through the kernel: the program makes its virtual joystick with /dev/uinput, and this
test reads it back from /dev/input/eventN the way a game does (evdev ioctls and events), while a fake
ELRS module (tests/fake_elrs.py) streams to the program.

Checks (DESIGN.md section 4): the device appears with the name given (EVIOCGNAME), BUS_VIRTUAL, vendor
and product 0, version 1 (EVIOCGID); exactly the 8 axes with range 0..32767, fuzz 0, flat 0, resolution
0, centered at 16384 (EVIOCGABS); exactly the buttons BTN_TRIGGER..BTN_BASE2, all released; udev and
SDL see a joystick, not an accelerometer. Then the sticks follow the module: the raw values exactly,
CH1..CH8 on X, Y, RX, RY, RZ, Z, THROTTLE, RUDDER (the Windows app's vJoy order), above 32767 = 32767;
CH9..CH16 press buttons 1..8 at 24576 and release them below 20480 (hysteresis, no chatter); every
frame is one packet (one SYN_REPORT); a pause under 3 s holds the sticks, more than 3 s of silence
centers them all at once, and they follow again when the stream comes back; "stop" centers them; the
program ends cleanly on SIGTERM and the device disappears. Then --no-buttons: still a joystick, and
CH9..CH16 never press a button.

Needs Linux, write access to /dev/uinput and read access to the new /dev/input/eventN (CI: modprobe
uinput, packaging/install.sh, the input group). Without them it prints SKIP and exits 0, unless --require
(CI), which makes that a failure. Standard library only (the Python that tests/fake_elrs.py needs).

    python3 tests/uinput_e2e.py [--require] [-v]
Environment: BIN = the program (default build/native/elrs-wifi-joystick).
"""
import argparse
import errno
import fcntl
import os
import platform
import random
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import fake_elrs  # noqa: E402

BIN = os.environ.get("BIN", os.path.join(ROOT, "build", "native", "elrs-wifi-joystick"))
VERBOSE = False

# ------------------------------------------------------------------ evdev (linux/input.h)
def _ioc_layout():
    """(size bits, NONE, READ, WRITE): asm-generic, or the older layout of these architectures"""
    if platform.machine().startswith(("ppc", "powerpc", "mips", "sparc", "alpha")):
        return 13, 1, 2, 4
    return 14, 0, 2, 1


_SIZEBITS, _IOC_NONE, _IOC_READ, _IOC_WRITE = _ioc_layout()


def _ioc(d, nr, size):
    return (d << (16 + _SIZEBITS)) | (size << 16) | (ord("E") << 8) | nr


EVIOCGID = _ioc(_IOC_READ, 0x02, 8)                       # struct input_id: 4 x u16


def EVIOCGNAME(n): return _ioc(_IOC_READ, 0x06, n)         # noqa: E302,E704
def EVIOCGPROP(n): return _ioc(_IOC_READ, 0x09, n)         # noqa: E302,E704
def EVIOCGKEY(n): return _ioc(_IOC_READ, 0x18, n)          # noqa: E302,E704
def EVIOCGBIT(ev, n): return _ioc(_IOC_READ, 0x20 + ev, n)  # noqa: E302,E704
def EVIOCGABS(code): return _ioc(_IOC_READ, 0x40 + code, 24)  # noqa: E302,E704  struct input_absinfo: 6 x s32


EV_SYN, EV_KEY, EV_REL, EV_ABS = 0, 1, 2, 3
SYN_REPORT, SYN_DROPPED = 0, 3
ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_THROTTLE, ABS_RUDDER = range(8)
ABS_PRESSURE = 0x18
ABS_NAMES = ["X", "Y", "Z", "RX", "RY", "RZ", "THROTTLE", "RUDDER"]
AXES = [ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_RZ, ABS_Z, ABS_THROTTLE, ABS_RUDDER]     # CH1..CH8
BTN_MOUSE, BTN_JOYSTICK, BTN_DIGI = 0x110, 0x120, 0x140
BTN_TOOL_PEN, BTN_TOOL_FINGER, BTN_TOUCH, BTN_STYLUS = 0x140, 0x145, 0x14a, 0x14b
BUTTONS = list(range(0x120, 0x128))                                                  # CH9..CH16
BUS_VIRTUAL = 0x06
CENTER, PRESS, RELEASE, MAXV = 16384, 24576, 20480, 32767
EV_FMT = "@llHHi"                 # struct input_event: struct timeval (2 longs), u16, u16, s32
EV_SIZE = struct.calcsize(EV_FMT)
KEY_BYTES, ABS_BYTES, EV_BYTES, PROP_BYTES = 0x2ff // 8 + 1, 0x3f // 8 + 1, 0x1f // 8 + 1, 0x1f // 8 + 1


def bits(buf):
    return {i * 8 + b for i, byte in enumerate(buf) for b in range(8) if byte >> b & 1}


def ioctl_bytes(fd, req, n):
    buf = bytearray(n)
    fcntl.ioctl(fd, req, buf, True)
    return bytes(buf)


class Fail(Exception):
    pass


class Skip(Exception):
    pass


class Gone(Exception):
    pass


class Checks:
    def __init__(self):
        self.n = 0
        self.failed = []

    def check(self, what, ok, detail=""):
        self.n += 1
        if ok:
            if VERBOSE:
                print("  ok   " + what, flush=True)
        else:
            self.failed.append(what + (": " + detail if detail else ""))
            print("  FAIL " + what + (": " + detail if detail else ""), flush=True)
        return ok


class Dev:
    """An evdev node, read like a game reads it: the state changes only at SYN_REPORT."""

    def __init__(self, path, fd):
        self.path, self.fd = path, fd
        self.abs = {c: self.absinfo(c)[0] for c in range(8)}
        self.btn = self.keys_down()
        self.pending = []
        self.packets = []          # (monotonic time, [(type, code, value), ...]) per SYN_REPORT
        self.allowed = {c: {self.abs[c], CENTER} for c in range(8)}   # every value an axis may take
        self.bad = []

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass

    # ---- ioctls
    def name(self):
        return ioctl_bytes(self.fd, EVIOCGNAME(256), 256).split(b"\0", 1)[0].decode("utf-8", "replace")

    def ident(self):
        return struct.unpack("HHHH", ioctl_bytes(self.fd, EVIOCGID, 8))

    def evbits(self):
        return bits(ioctl_bytes(self.fd, EVIOCGBIT(0, EV_BYTES), EV_BYTES))

    def absbits(self):
        return bits(ioctl_bytes(self.fd, EVIOCGBIT(EV_ABS, ABS_BYTES), ABS_BYTES))

    def keybits(self):
        return bits(ioctl_bytes(self.fd, EVIOCGBIT(EV_KEY, KEY_BYTES), KEY_BYTES))

    def props(self):
        return bits(ioctl_bytes(self.fd, EVIOCGPROP(PROP_BYTES), PROP_BYTES))

    def absinfo(self, code):
        """(value, minimum, maximum, fuzz, flat, resolution)"""
        return struct.unpack("6i", ioctl_bytes(self.fd, EVIOCGABS(code), 24))

    def keys_down(self):
        return bits(ioctl_bytes(self.fd, EVIOCGKEY(KEY_BYTES), KEY_BYTES))

    # ---- events
    def allow(self, code, value):
        self.allowed[code].add(value)

    def pump(self, timeout):
        """read what arrives within timeout (seconds); returns the packets completed"""
        done = []
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            r, _, _ = select.select([self.fd], [], [], max(0.0, left))
            if not r:
                return done
            try:
                data = os.read(self.fd, EV_SIZE * 64)
            except OSError as e:
                if e.errno == errno.EAGAIN:
                    continue
                if e.errno == errno.ENODEV:
                    raise Gone()
                raise
            now = time.monotonic()
            for off in range(0, len(data) - EV_SIZE + 1, EV_SIZE):
                _, _, typ, code, value = struct.unpack_from(EV_FMT, data, off)
                if typ == EV_SYN and code == SYN_DROPPED:
                    self.bad.append("SYN_DROPPED: the kernel dropped events (this test read too slowly)")
                    continue
                if typ == EV_SYN and code == SYN_REPORT:
                    pkt = self.pending
                    self.pending = []
                    for t, c, v in pkt:
                        if t == EV_ABS and c < 8:
                            if v not in self.allowed[c]:
                                self.bad.append("ABS_%s got %d, a value the module never sent" % (ABS_NAMES[c], v))
                            self.abs[c] = v
                        elif t == EV_KEY:
                            (self.btn.add if v else self.btn.discard)(c)
                    self.packets.append((now, pkt))
                    done.append((now, pkt))
                    if VERBOSE:
                        print("    packet " + " ".join("%d:%#x=%d" % e for e in pkt), flush=True)
                    continue
                self.pending.append((typ, code, value))
            if left <= 0:
                return done

    def wait(self, pred, what, timeout=4.0):
        end = time.monotonic() + timeout
        while not pred():
            left = end - time.monotonic()
            if left <= 0:
                raise Fail("timed out waiting for %s; axes %s, buttons %s" % (what, self.axes_str(), sorted(self.btn)))
            self.pump(min(left, 0.1))

    def axes_str(self):
        return ", ".join("%s=%d" % (ABS_NAMES[c], self.abs[c]) for c in range(8))


# ------------------------------------------------------------------ the program and the module
def free_port():
    """a port free for UDP and TCP on every address"""
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


class Prog:
    """the program, stdin a pipe (line commands), its output collected line by line"""

    def __init__(self, args, env):
        self.p = subprocess.Popen([BIN] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, env=env)
        self.out = []
        self.cv = threading.Condition()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for raw in iter(self.p.stdout.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip("\n")
            if VERBOSE:
                print("    | " + line, flush=True)
            with self.cv:
                self.out.append(line)
                self.cv.notify_all()
        with self.cv:
            self.cv.notify_all()

    def send(self, line):
        self.p.stdin.write((line + "\n").encode())
        self.p.stdin.flush()

    def mark(self):
        with self.cv:
            return len(self.out)

    def wait_for(self, pat, timeout=8.0, start=0):
        end = time.monotonic() + timeout
        with self.cv:
            while True:
                for line in self.out[start:]:
                    if (pat.search(line) if hasattr(pat, "search") else pat in line):
                        return line
                left = end - time.monotonic()
                if left <= 0 or (self.p.poll() is not None and not self.reader.is_alive()):
                    raise Fail("timed out waiting for %r; the program said:\n%s" %
                               (getattr(pat, "pattern", pat), "\n".join("    | " + x for x in self.out[-30:])))
                self.cv.wait(min(left, 0.2))

    def finish(self, sig=signal.SIGTERM, timeout=8.0):
        if self.p.poll() is None:
            self.p.send_signal(sig)
        try:
            code = self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            raise Fail("the program did not end within %.0f s of signal %d" % (timeout, sig))
        self.reader.join(2)
        return code


class World:
    """a private runtime dir (the single-instance lock), free ports, a fake ELRS module"""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="uinput-e2e-")
        run = os.path.join(self.dir, "run")
        os.mkdir(run, 0o700)
        self.udp, self.http = free_port(), free_port()
        self.env = dict(os.environ, XDG_RUNTIME_DIR=run, HOME=self.dir, NO_COLOR="1", TZ="UTC")
        self.env.pop("XDG_CONFIG_HOME", None)
        self.module = None
        self.prog = None

    def start_prog(self, *extra):
        self.prog = Prog(["-p", str(self.udp), "--http-port", str(self.http), "--no-web", "--no-config"] + list(extra),
                         self.env)
        return self.prog

    def start_module(self, values):
        """a fake ELRS module that streams these 16 raw values from its first frame"""
        self.module = fake_elrs.FakeModule(ip="127.0.0.2", http_port=self.http, kind="elrs", stream_port=self.udp,
                                           beacon_to=("127.0.0.1", self.udp), beacon_interval=0.5)
        self.module.values = list(values)
        return self.module.start()

    def close(self):
        if self.prog and self.prog.p.poll() is None:
            self.prog.p.kill()
            self.prog.p.wait()
        if self.module:
            self.module.stop()
        shutil.rmtree(self.dir, ignore_errors=True)


def event_nodes():
    try:
        return sorted(os.path.join("/dev/input", n) for n in os.listdir("/dev/input") if re.match(r"event\d+$", n))
    except OSError:
        return []


def sysfs_name(node):
    try:
        with open("/sys/class/input/%s/device/name" % os.path.basename(node)) as f:
            return f.read().rstrip("\n")
    except OSError:
        return None


def find_device(name, hint, timeout=5.0):
    """the event node named name: open it (udev may still be setting its permissions: retried)"""
    end = time.monotonic() + timeout
    denied = None
    while True:
        nodes = event_nodes()
        if hint in nodes:                       # the node the program reported first
            nodes.remove(hint)
            nodes.insert(0, hint)
        for node in nodes:
            try:
                fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            except OSError as e:
                if e.errno in (errno.EACCES, errno.EPERM) and sysfs_name(node) == name:
                    denied = node
                continue
            try:
                if ioctl_bytes(fd, EVIOCGNAME(256), 256).split(b"\0", 1)[0].decode("utf-8", "replace") == name:
                    return Dev(node, fd)
            except OSError:
                pass
            os.close(fd)
        if time.monotonic() >= end:
            if denied:
                raise Skip("the joystick %s exists, but this user cannot read it - add the user to the input group "
                           "(sudo usermod -aG input $USER, then log in again)" % denied)
            raise Fail("no /dev/input/event* named %r appeared" % name)
        time.sleep(0.05)


def udev_props(node):
    """udev's properties of the node, or None when udev does not run here"""
    adm = shutil.which("udevadm")
    if not adm or not os.path.exists("/run/udev/control"):
        return None
    for _ in range(50):                         # udev may not have processed the new device yet
        r = subprocess.run([adm, "info", "--query=property", "--name=" + node], stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL)
        props = dict(line.split("=", 1) for line in r.stdout.decode("utf-8", "replace").splitlines() if "=" in line)
        if r.returncode == 0 and props.get("ID_INPUT") == "1":
            return props
        time.sleep(0.1)
    return props


def check_is_joystick(t, dev, label):
    """what makes udev (input_id) and SDL call a device a joystick, not an accelerometer, mouse or tablet"""
    ev, ab, keys, props = dev.evbits(), dev.absbits(), dev.keybits(), dev.props()
    has_keys = EV_KEY in ev
    t.check(label + ": udev does not take it for an accelerometer (X, Y, Z axes need a button: EV_KEY)",
            has_keys or not {ABS_X, ABS_Y, ABS_Z} <= ab, "EV_KEY missing")
    t.check(label + ": SDL does not take it for an accelerometer (RX, RY, RZ axes need a button: EV_KEY)",
            has_keys or not {ABS_RX, ABS_RY, ABS_RZ} <= ab, "EV_KEY missing")
    t.check(label + ": no mouse, pen, touch or finger buttons",
            not keys & (set(range(BTN_MOUSE, BTN_JOYSTICK)) | {BTN_TOOL_PEN, BTN_TOOL_FINGER, BTN_TOUCH, BTN_STYLUS}),
            str(sorted(keys)))
    t.check(label + ": no input properties (pointer, direct, accelerometer...)", not props, str(sorted(props)))
    t.check(label + ": joystick buttons or joystick axes",
            bool(keys & set(range(BTN_JOYSTICK, BTN_DIGI))) or bool(ab & set(range(ABS_RX, ABS_PRESSURE))))
    up = udev_props(dev.path)
    if up is None:
        print("  (udev does not run here: its ID_INPUT_JOYSTICK is not checked)", flush=True)
        return
    t.check(label + ": udev says ID_INPUT_JOYSTICK=1", up.get("ID_INPUT_JOYSTICK") == "1", str(up))
    for k in ("ID_INPUT_ACCELEROMETER", "ID_INPUT_MOUSE", "ID_INPUT_TABLET", "ID_INPUT_TOUCHPAD",
              "ID_INPUT_TOUCHSCREEN", "ID_INPUT_KEYBOARD"):
        t.check(label + ": udev does not say " + k, k not in up, str(up))


def gone_from_kernel(name, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            with open("/proc/bus/input/devices") as f:
                text = f.read()
        except OSError:
            return True
        if 'N: Name="%s"' % name not in text:
            return True
        time.sleep(0.05)
    return False


def frame(axes, buttons=(0,) * 8):
    return list(axes) + list(buttons)


def set_and_wait(dev, m, axes, buttons, pressed, what):
    """stream these raw values, then wait for the device to show them (axes exactly, these buttons down)"""
    want = [min(v, MAXV) for v in axes]
    for i, v in enumerate(want):
        dev.allow(AXES[i], v)
    m.values = frame(axes, buttons)            # one assignment: every frame carries all 16 at once
    want_btn = {BUTTONS[i] for i in pressed}
    dev.wait(lambda: all(dev.abs[AXES[i]] == want[i] for i in range(8)) and dev.btn == want_btn, what)


def drain(dev, seconds=0.15):
    dev.pump(seconds)
    return len(dev.packets)


# ------------------------------------------------------------------ the cases
def case_joystick(t):
    w = World()
    name = "ELRS WiFi Joystick e2e %d" % os.getpid()
    try:
        p = w.start_prog("--name", name)
        line = p.wait_for(re.compile(r"[Vv]irtual joystick"))
        if "No virtual joystick" in line:
            raise Fail("the program made no virtual joystick: " + line)
        m_node = re.search(r"/dev/input/event\d+", line)
        dev = find_device(name, m_node.group(0) if m_node else None)
        try:
            run_joystick(t, w, p, dev, name, line, m_node)
        finally:
            dev.close()
    finally:
        w.close()


def run_joystick(t, w, p, dev, name, line, m_node):
    t.check("the program says which node it made", m_node is not None and m_node.group(0) == dev.path,
            "said %r, found %s" % (m_node and m_node.group(0), dev.path))
    t.check("the program says 8 axes, 8 buttons", "8 axes, 8 buttons" in line, line)
    t.check("name", dev.name() == name, dev.name())
    bus, vendor, product, version = dev.ident()
    t.check("BUS_VIRTUAL", bus == BUS_VIRTUAL, "bus %#x" % bus)
    t.check("vendor 0, product 0, version 1", (vendor, product, version) == (0, 0, 1), str((vendor, product, version)))
    t.check("event types: exactly SYN, KEY, ABS", dev.evbits() == {EV_SYN, EV_KEY, EV_ABS}, str(sorted(dev.evbits())))
    t.check("axes: exactly the 8", dev.absbits() == set(range(8)), str(sorted(dev.absbits())))
    for c in range(8):
        info = dev.absinfo(c)
        t.check("ABS_%s: centered at 16384, range 0..32767, fuzz 0, flat 0, resolution 0" % ABS_NAMES[c],
                info == (CENTER, 0, MAXV, 0, 0, 0), "(value, min, max, fuzz, flat, res) = %s" % (info,))
    t.check("buttons: exactly BTN_TRIGGER..BTN_BASE2", dev.keybits() == set(BUTTONS),
            str([hex(k) for k in sorted(dev.keybits())]))
    t.check("buttons: all released", not dev.keys_down(), str(sorted(dev.keys_down())))
    check_is_joystick(t, dev, "the joystick")
    t.check("no events before the module streams", drain(dev, 0.3) == 0, str(dev.packets))

    # the sticks: every axis its own value, in the vJoy order, from the first frame on
    vals = [1000 + 4000 * i for i in range(8)]
    for i, v in enumerate(vals):
        dev.allow(AXES[i], v)
    m = w.start_module(frame(vals))
    p.wait_for("Joystick source locked to 127.0.0.2")
    set_and_wait(dev, m, vals, (0,) * 8, (), "CH1..CH8 = 1000, 5000, ... 29000")
    for i in range(8):
        t.check("CH%d -> ABS_%s = %d (EVIOCGABS)" % (i + 1, ABS_NAMES[AXES[i]], vals[i]),
                dev.absinfo(AXES[i])[0] == vals[i], str(dev.absinfo(AXES[i])))
    for axes in ([0, MAXV, fake_elrs.ELRS_LOW, fake_elrs.ELRS_CENTER, 1, MAXV - 1, CENTER, 12345],
                 [MAXV] * 8, [0] * 8):
        set_and_wait(dev, m, axes, (0,) * 8, (), "the axes at %s" % axes)
    rnd = random.Random(7)
    for _ in range(20):
        axes = [rnd.randrange(0, MAXV + 1) for _ in range(8)]
        set_and_wait(dev, m, axes, (0,) * 8, (), "random axes %s" % axes)
    t.check("the axes follow exactly (20 random frames, extremes)", True)
    # a stick past a wide endpoint (above 32767, no placeholder): full deflection
    set_and_wait(dev, m, [0x816B, 100, 200, 300, 400, 500, 600, 700], (0,) * 8, (), "CH1 = 0x816B")
    t.check("CH1 above 32767 -> 32767", dev.absinfo(ABS_X)[0] == MAXV, str(dev.absinfo(ABS_X)))

    # the buttons: pressed at >= 24576, released below 20480, held in between
    marker = [2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000]
    steps = [
        ([24575, 24576, 32767, 20480, 20479, 0, 24576, 22000], {1, 2, 6}),
        ([24576, 20480, 20479, 24575, 32767, 24576, 22528, 20480], {0, 1, 4, 5, 6}),
        ([21000] * 8, {0, 1, 4, 5, 6}),
        ([20479] * 8, set()),
        ([32767] * 8, set(range(8))),
        ([20480] * 8, set(range(8))),
        ([0] * 8, set()),
    ]
    before = set()
    for k, (btns, pressed) in enumerate(steps):
        marker = [v + 1 for v in marker]
        n0 = len(dev.packets)
        set_and_wait(dev, m, marker, btns, pressed, "CH9..CH16 = %s" % btns)
        drain(dev)
        key_events = [(c, v) for _, pkt in dev.packets[n0:] for (typ, c, v) in pkt if typ == EV_KEY]
        want = sorted((BUTTONS[i], 1 if i in pressed else 0) for i in range(8) if (i in pressed) != (i in before))
        t.check("buttons step %d %s -> pressed %s, each change once" % (k + 1, btns, sorted(pressed)),
                sorted(key_events) == want and dev.keys_down() == {BUTTONS[i] for i in pressed},
                "events %s, want %s, kernel %s" % (key_events, want, sorted(dev.keys_down())))
        before = pressed
    t.check("every axis value was one the module sent; no events dropped", not dev.bad, "; ".join(dev.bad[:5]))
    t.check("every packet has at most 16 events (one frame = one write)",
            all(len(pkt) <= 16 for _, pkt in dev.packets))
    t.check("no empty packets (nothing written when nothing changed)", all(pkt for _, pkt in dev.packets))

    # a short pause holds everything
    set_and_wait(dev, m, [111, 222, 333, 444, 555, 666, 777, 888], (MAXV,) * 8, range(8), "before the pause")
    drain(dev)
    n0 = len(dev.packets)
    m.mode = "silent"
    dev.pump(1.2)
    t.check("a 1.2 s pause holds the sticks (no events)", len(dev.packets) == n0, str(dev.packets[n0:]))
    m.mode = "normal"
    set_and_wait(dev, m, [112, 222, 333, 444, 555, 666, 777, 888], (MAXV,) * 8, range(8), "after the pause")

    # more than 3 s of silence: centered, all at once
    drain(dev)
    n0 = len(dev.packets)
    m.mode = "silent"
    t0 = time.monotonic()
    dev.wait(lambda: all(dev.abs[c] == CENTER for c in range(8)) and not dev.btn, "centering after silence", 7.0)
    dt = dev.packets[-1][0] - t0
    t.check("silence > 3 s: centered after 3 s (%.2f s)" % dt, 2.9 <= dt <= 5.0, "%.2f s" % dt)
    t.check("silence: centered in one packet, nothing before it", len(dev.packets) == n0 + 1,
            str(dev.packets[n0:]))
    t.check("silence: the kernel agrees", all(dev.absinfo(c)[0] == CENTER for c in range(8)) and not dev.keys_down())
    m.mode = "normal"
    set_and_wait(dev, m, [9000] * 8, (MAXV,) * 4 + (0,) * 4, range(4), "the stream back after the silence")
    t.check("the sticks follow again after the silence", True)

    # stop centers; start follows again
    mk = p.mark()
    p.send("stop")
    p.wait_for("Stopped.", start=mk)
    dev.wait(lambda: all(dev.abs[c] == CENTER for c in range(8)) and not dev.btn, "centering at stop")
    t.check("stop: centered and released", all(dev.absinfo(c)[0] == CENTER for c in range(8)) and not dev.keys_down())
    p.send("start")
    set_and_wait(dev, m, [9001] * 8, (MAXV,) * 8, range(8), "the stream after start")
    t.check("start: the sticks follow again", True)

    # the end: exit 0, the device is gone
    code = p.finish(signal.SIGTERM)
    t.check("SIGTERM: exit 0", code == 0, "exit %s" % code)
    try:
        dev.pump(0.5)
        saw_center = all(dev.abs[c] == CENTER for c in range(8)) and not dev.btn
        if saw_center:
            t.check("at exit: centered before the device went away", True)
        else:
            print("  (the device went away before its last events could be read: centering at exit not seen)")
    except Gone:
        print("  (the device went away before its last events could be read: centering at exit not seen)")
    t.check("the device is gone from the kernel", gone_from_kernel(name))
    end = time.monotonic() + 3
    while os.path.exists(dev.path) and sysfs_name(dev.path) == name and time.monotonic() < end:
        time.sleep(0.05)
    t.check("its node is gone", not (os.path.exists(dev.path) and sysfs_name(dev.path) == name), dev.path)


def case_no_buttons(t):
    w = World()
    name = "ELRS WiFi Joystick e2e nb %d" % os.getpid()
    try:
        p = w.start_prog("--name", name, "--no-buttons")
        line = p.wait_for(re.compile(r"[Vv]irtual joystick"))
        if "No virtual joystick" in line:
            raise Fail("the program made no virtual joystick: " + line)
        m_node = re.search(r"/dev/input/event\d+", line)
        dev = find_device(name, m_node.group(0) if m_node else None)
        try:
            t.check("--no-buttons: the 8 axes", dev.absbits() == set(range(8)), str(sorted(dev.absbits())))
            check_is_joystick(t, dev, "--no-buttons")
            vals = [1000 * (i + 1) for i in range(8)]
            for i, v in enumerate(vals):
                dev.allow(AXES[i], v)
            m = w.start_module(frame(vals, (MAXV,) * 8))
            p.wait_for("Joystick source locked to 127.0.0.2")
            set_and_wait(dev, m, vals, (MAXV,) * 8, (), "the axes, CH9..CH16 at 32767")
            drain(dev)
            t.check("--no-buttons: CH9..CH16 at 32767 press nothing",
                    not dev.keys_down() and not any(typ == EV_KEY for _, pkt in dev.packets for (typ, _, _) in pkt),
                    str(sorted(dev.keys_down())))
            code = p.finish(signal.SIGTERM)
            t.check("--no-buttons: SIGTERM, exit 0", code == 0, "exit %s" % code)
            t.check("--no-buttons: the device is gone", gone_from_kernel(name))
        finally:
            dev.close()
    finally:
        w.close()


# ------------------------------------------------------------------ main
def main():
    global VERBOSE
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--require", action="store_true", help="a missing /dev/uinput or access to it is a failure (CI)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    VERBOSE = a.verbose

    def skip(why):
        if a.require:
            print("FAIL uinput_e2e: " + why + " (--require)", flush=True)
            return 1
        print("SKIP uinput_e2e: " + why, flush=True)
        return 0

    if not sys.platform.startswith("linux"):
        return skip("not Linux")
    if not os.path.exists("/dev/uinput"):
        return skip("/dev/uinput does not exist (load it: sudo modprobe uinput)")
    if not os.access("/dev/uinput", os.W_OK):
        return skip("no write access to /dev/uinput (run packaging/install.sh, or for now: "
                    "sudo setfacl -m u:$USER:rw /dev/uinput)")
    if not (os.path.isfile(BIN) and os.access(BIN, os.X_OK)):
        print("FAIL uinput_e2e: no program at %s (build it: ./build.sh native, or set BIN)" % BIN, flush=True)
        return 1
    t = Checks()
    for case in (case_joystick, case_no_buttons):
        print("== " + case.__name__, flush=True)
        try:
            case(t)
        except Skip as e:
            return skip(str(e))
        except (Fail, Gone, OSError) as e:
            # the case could not go on: one more failed check
            t.check("%s ran to the end" % case.__name__, False, str(e) or "the device went away")
    if t.failed:
        print("uinput_e2e: %d of %d checks FAILED" % (len(t.failed), t.n), flush=True)
        return 1
    print("uinput_e2e: %d checks passed" % t.n, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

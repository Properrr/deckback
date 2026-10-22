"""Synthetic evdev devices on the Deck, driven over SSH by a generated program on /dev/uinput.

Use CDP for "does Leanback do X"; use uinput for "does the launcher's evdev path notice Y". Steam
Input may double-deliver a new pad's presses, so assert that focus moved, not how many times.
"""

from __future__ import annotations

import struct

# ---- kernel constants (linux/input-event-codes.h, linux/uinput.h) --------------------------------

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0

BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST = 0x130, 0x131, 0x133, 0x134
BTN_TL, BTN_TR = 0x136, 0x137
BTN_SELECT, BTN_START = 0x13A, 0x13B
BTN_THUMBL, BTN_THUMBR = 0x13D, 0x13E
BTN_TOUCH = 0x14A

ABS_X, ABS_Y, ABS_Z = 0x00, 0x01, 0x02
ABS_RX, ABS_RY, ABS_RZ = 0x03, 0x04, 0x05
ABS_HAT0X, ABS_HAT0Y = 0x10, 0x11
ABS_MT_SLOT = 0x2F
ABS_MT_POSITION_X, ABS_MT_POSITION_Y = 0x35, 0x36
ABS_MT_TRACKING_ID = 0x39

ABS_CNT = 64

# ioctl request numbers: _IO('U', n) and _IOW('U', n, int) expanded.
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502
UI_SET_EVBIT = 0x40045564  # _IOW('U', 100, int)
UI_SET_KEYBIT = 0x40045565  # _IOW('U', 101, int)
UI_SET_ABSBIT = 0x40045567  # _IOW('U', 103, int)
UI_SET_PROPBIT = 0x4004556E  # _IOW('U', 110, int)

INPUT_PROP_DIRECT = 0x01  # touchscreen (as opposed to a touchpad); consumed by libinput

EV_FF = 0x15
FF_RUMBLE = 0x50

# `struct input_event` on 64-bit Linux: struct timeval {long, long}, __u16, __u16, __s32.
# `q`, not `l`: with the `=` prefix `l` is 4 bytes, but time_t is 8.
_INPUT_EVENT = struct.Struct("=qqHHi")
INPUT_EVENT_SIZE = 24

# ---- ioctl encoding -----------------------------------------------------------------------------
# The kernel's _IOC macro; the UI_* constants above are checked against it.

_IOC_NRBITS, _IOC_TYPEBITS, _IOC_SIZEBITS = 8, 8, 14
_IOC_NRSHIFT = 0
_IOC_TYPESHIFT = _IOC_NRSHIFT + _IOC_NRBITS
_IOC_SIZESHIFT = _IOC_TYPESHIFT + _IOC_TYPEBITS
_IOC_DIRSHIFT = _IOC_SIZESHIFT + _IOC_SIZEBITS

IOC_NONE, IOC_WRITE, IOC_READ = 0, 1, 2


def ioc(direction, typ, nr, size):
    return (
        (direction << _IOC_DIRSHIFT)
        | (typ << _IOC_TYPESHIFT)
        | (nr << _IOC_NRSHIFT)
        | (size << _IOC_SIZESHIFT)
    )


def eviocgname(length):
    return ioc(IOC_READ, ord("E"), 0x06, length)


def eviocgbit(ev_type, length):
    """EVIOCGBIT(ev, len) — the capability bitmap for one event type (ev=0 asks which types exist)."""
    return ioc(IOC_READ, ord("E"), 0x20 + ev_type, length)

# `struct uinput_user_dev`: char name[80]; struct input_id {u16 x4}; u32 ff_effects_max;
# s32 absmax[64], absmin[64], absfuzz[64], absflat[64].  "=" means no alignment padding.
_UINPUT_USER_DEV = struct.Struct("=80s4HI" + "64i" * 4)
UINPUT_USER_DEV_SIZE = 1116


class DeviceSpec:
    """A uinput device to create. `abs_axes` maps code -> (min, max, fuzz, flat).

    `props` lists the INPUT_PROP_* it advertises; libinput treats multitouch without
    INPUT_PROP_DIRECT as a touchpad, with it as a touchscreen.
    """

    def __init__(self, name, keys=(), abs_axes=None, props=(), vendor=0x1234, product=0x5678,
                 version=1):
        self.name = name
        self.keys = tuple(keys)
        self.abs_axes = dict(abs_axes or {})
        self.props = tuple(props)
        self.vendor = vendor
        self.product = product
        self.version = version

    def ev_bits(self):
        """The EV_* types to enable: only EV_KEY/EV_ABS, never EV_SYN (UI_DEV_CREATE fails with it)."""
        bits = []
        if self.keys:
            bits.append(EV_KEY)
        if self.abs_axes:
            bits.append(EV_ABS)
        return bits

    def validate(self):
        """Raise on the mistakes that produce a device the kernel accepts but nobody can use."""
        for code, info in self.abs_axes.items():
            if len(info) != 4:
                raise ValueError(f"ABS axis 0x{code:02x} needs (min, max, fuzz, flat), got {info!r}")
            lo, hi = info[0], info[1]
            if lo >= hi:
                raise ValueError(f"ABS axis 0x{code:02x} has min {lo} >= max {hi}")
            if code >= ABS_CNT:
                raise ValueError(f"ABS axis 0x{code:02x} is beyond ABS_CNT={ABS_CNT}")
        if not self.name or len(self.name.encode()) > 79:
            raise ValueError("device name must be 1..79 bytes (uinput_user_dev.name is char[80])")
        return self


def pack_input_event(ev_type, code, value):
    """One `struct input_event`. Timestamps are zero: the kernel stamps them on the way in."""
    return _INPUT_EVENT.pack(0, 0, ev_type, code, value)


def pack_user_dev(spec):
    """`struct uinput_user_dev` for the legacy setup path (write it, then UI_DEV_CREATE)."""
    spec.validate()
    absmax = [0] * ABS_CNT
    absmin = [0] * ABS_CNT
    absfuzz = [0] * ABS_CNT
    absflat = [0] * ABS_CNT
    for code, (lo, hi, fuzz, flat) in spec.abs_axes.items():
        absmin[code], absmax[code], absfuzz[code], absflat[code] = lo, hi, fuzz, flat
    return _UINPUT_USER_DEV.pack(
        spec.name.encode(),
        0x03,  # BUS_USB
        spec.vendor,
        spec.product,
        spec.version,
        0,  # ff_effects_max
        *absmax,
        *absmin,
        *absfuzz,
        *absflat,
    )


def gamepad_spec(name="deckback-test-pad"):
    """An Xbox-shaped pad: exactly the controls `launcher/src/input.cpp` reads.

    Axis ranges mirror Steam Input's virtual pad (sticks ±32767, triggers 0..255, hats ±1).
    """
    return DeviceSpec(
        name,
        keys=(
            BTN_SOUTH,
            BTN_EAST,
            BTN_NORTH,
            BTN_WEST,
            BTN_TL,
            BTN_TR,
            BTN_SELECT,
            BTN_START,
            BTN_THUMBL,
            BTN_THUMBR,
        ),
        abs_axes={
            ABS_X: (-32768, 32767, 0, 0),
            ABS_Y: (-32768, 32767, 0, 0),
            ABS_RX: (-32768, 32767, 0, 0),
            ABS_RY: (-32768, 32767, 0, 0),
            ABS_Z: (0, 255, 0, 0),
            ABS_RZ: (0, 255, 0, 0),
            ABS_HAT0X: (-1, 1, 0, 0),
            ABS_HAT0Y: (-1, 1, 0, 0),
        },
    )


def touchscreen_spec(name="deckback-test-touch", width=1280, height=800):
    """A multitouch panel shaped like the FTS3528 by capability, not by its USB id."""
    return DeviceSpec(
        name,
        keys=(BTN_TOUCH,),
        abs_axes={
            ABS_X: (0, width - 1, 0, 0),
            ABS_Y: (0, height - 1, 0, 0),
            ABS_MT_SLOT: (0, 9, 0, 0),
            ABS_MT_POSITION_X: (0, width - 1, 0, 0),
            ABS_MT_POSITION_Y: (0, height - 1, 0, 0),
            ABS_MT_TRACKING_ID: (0, 65535, 0, 0),
        },
        # Without this, libinput sees a touchpad, not a touchscreen.
        props=(INPUT_PROP_DIRECT,),
    )


def syn():
    return [(EV_SYN, SYN_REPORT, 0)]


def press(code):
    """Press and release one button, each with its own SYN_REPORT (one report = zero-length press)."""
    return [(EV_KEY, code, 1)] + syn() + [(EV_KEY, code, 0)] + syn()


def hold(code):
    return [(EV_KEY, code, 1)] + syn()


def release(code):
    return [(EV_KEY, code, 0)] + syn()


def dpad(dx, dy):
    """Push the hat, then centre it."""
    return [(EV_ABS, ABS_HAT0X, dx), (EV_ABS, ABS_HAT0Y, dy)] + syn() + [
        (EV_ABS, ABS_HAT0X, 0),
        (EV_ABS, ABS_HAT0Y, 0),
    ] + syn()


def stick(code, value):
    return [(EV_ABS, code, value)] + syn()


def tap(x, y, slot=0, tracking_id=1):
    """A single-finger multitouch tap: down at (x, y), then up.

    Type-B protocol: the tracking id opens and closes the contact. BTN_TOUCH accompanies it because
    libinput/gamescope's pointer emulation keys off it.
    """
    return (
        [
            (EV_ABS, ABS_MT_SLOT, slot),
            (EV_ABS, ABS_MT_TRACKING_ID, tracking_id),
            (EV_ABS, ABS_MT_POSITION_X, x),
            (EV_ABS, ABS_MT_POSITION_Y, y),
            (EV_KEY, BTN_TOUCH, 1),
            (EV_ABS, ABS_X, x),
            (EV_ABS, ABS_Y, y),
        ]
        + syn()
        + [
            (EV_ABS, ABS_MT_SLOT, slot),
            (EV_ABS, ABS_MT_TRACKING_ID, -1),
            (EV_KEY, BTN_TOUCH, 0),
        ]
        + syn()
    )


def encode_events(events):
    """A flat byte string of `struct input_event`s, ready to write(2) to the uinput fd."""
    return b"".join(pack_input_event(t, c, v) for t, c, v in events)


# ---- the program that actually runs on the Deck --------------------------------------------------

_REMOTE = r'''
import ctypes, fcntl, os, struct, sys, time, base64

UI_DEV_CREATE, UI_DEV_DESTROY = {ui_create}, {ui_destroy}
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = {ui_evbit}, {ui_keybit}, {ui_absbit}, {ui_propbit}
EV_KEY, EV_ABS = {ev_key}, {ev_abs}

user_dev = base64.b64decode("{user_dev}")
events   = base64.b64decode("{events}")
ev_bits  = {ev_bits!r}
keys     = {keys!r}
axes     = {axes!r}
props    = {props!r}
settle   = {settle}

try:
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
except OSError as e:
    # Exit 3 = ENVIRONMENT, per the harness exit-code taxonomy (.internal/HARNESS.md). A missing
    # udev rule is not a product defect and must never be reported as one.
    sys.stderr.write("uinput: cannot open /dev/uinput: %s\n" % e)
    sys.stderr.write("uinput: needs KERNEL==\"uinput\", MODE=\"0660\", GROUP=\"input\"\n")
    sys.exit(3)

try:
    for b in ev_bits:
        fcntl.ioctl(fd, UI_SET_EVBIT, b)
    for k in keys:
        fcntl.ioctl(fd, UI_SET_KEYBIT, k)
    for a in axes:
        fcntl.ioctl(fd, UI_SET_ABSBIT, a)
    for p in props:
        fcntl.ioctl(fd, UI_SET_PROPBIT, p)
    os.write(fd, user_dev)
    fcntl.ioctl(fd, UI_DEV_CREATE)
except OSError as e:
    sys.stderr.write("uinput: device setup failed: %s\n" % e)
    os.close(fd)
    sys.exit(3)

# The consumer (our launcher, and gamescope) must notice the new /dev/input/eventN and open it
# before any event we send is observable. The launcher rescans on a 2 s hotplug tick.
time.sleep(settle)

if events:
    os.write(fd, events)
    time.sleep(0.15)

fcntl.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)
print("uinput: ok")
'''


def remote_program(spec, events=(), settle=2.5):
    """The Python source to run on the Deck. Deterministic: same inputs, same bytes."""
    import base64

    spec.validate()
    return _REMOTE.format(
        ui_create=UI_DEV_CREATE,
        ui_destroy=UI_DEV_DESTROY,
        ui_evbit=UI_SET_EVBIT,
        ui_keybit=UI_SET_KEYBIT,
        ui_absbit=UI_SET_ABSBIT,
        ev_key=EV_KEY,
        ev_abs=EV_ABS,
        user_dev=base64.b64encode(pack_user_dev(spec)).decode(),
        events=base64.b64encode(encode_events(events)).decode(),
        ui_propbit=UI_SET_PROPBIT,
        ev_bits=spec.ev_bits(),
        keys=list(spec.keys),
        axes=sorted(spec.abs_axes),
        props=list(spec.props),
        settle=settle,
    )


# ---- reading what the real devices can do --------------------------------------------------------

_CAPS = r'''
import fcntl, glob, json, os, struct, sys

EVIOCGNAME = {gname}
EVIOCGBIT0 = {gbit0}
EVIOCGBIT_ABS = {gbit_abs}
EVIOCGBIT_FF = {gbit_ff}
EVIOCGID = {gid}

def bits(fd, req, nbytes=64):
    buf = bytearray(nbytes)
    try:
        fcntl.ioctl(fd, req, buf)
    except OSError:
        return set()
    out = set()
    for i, byte in enumerate(buf):
        for b in range(8):
            if byte & (1 << b):
                out.add(i * 8 + b)
    return out

devices = []
for path in sorted(glob.glob("/dev/input/event*")):
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    except OSError:
        continue
    try:
        name = bytearray(256)
        try:
            fcntl.ioctl(fd, EVIOCGNAME, name)
            nm = name.split(b"\0", 1)[0].decode(errors="replace")
        except OSError:
            nm = "?"
        try:
            idbuf = bytearray(8)
            fcntl.ioctl(fd, EVIOCGID, idbuf)
            bus, vendor, product, version = struct.unpack("=4H", bytes(idbuf))
        except OSError:
            bus = vendor = product = version = 0
        devices.append({{
            "path": path,
            "name": nm,
            "vendor": "%04x" % vendor,
            "product": "%04x" % product,
            "ev": sorted(bits(fd, EVIOCGBIT0, 8)),
            "abs": sorted(bits(fd, EVIOCGBIT_ABS, 8)),
            "ff": sorted(bits(fd, EVIOCGBIT_FF, 16)),
        }})
    finally:
        os.close(fd)

print(json.dumps(devices))
'''


def capabilities_program():
    """A program that prints every /dev/input node's name, ids, EV_* types, ABS axes and FF effects
    as JSON. Read-only, so it needs no udev rule.
    """
    return _CAPS.format(
        gname=eviocgname(256),
        gbit0=eviocgbit(0, 8),
        gbit_abs=eviocgbit(EV_ABS, 8),
        gbit_ff=eviocgbit(EV_FF, 16),
        gid=ioc(IOC_READ, ord("E"), 0x02, 8),
    )


def find_device(devices, *, name_contains=None, needs_abs=(), needs_ev=()):
    """Pick devices out of `capabilities_program()`'s JSON by name/capability (eventN is unstable)."""
    out = []
    for d in devices:
        if name_contains and name_contains.lower() not in d["name"].lower():
            continue
        if any(a not in d["abs"] for a in needs_abs):
            continue
        if any(e not in d["ev"] for e in needs_ev):
            continue
        out.append(d)
    return out

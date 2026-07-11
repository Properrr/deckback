"""The touch lock: does `EVIOCGRAB` actually starve gamescope of touch?

**This is the single most important test in the suite**, and it has never run. The entire touch-lock
feature (input-ux §4) rests on one untested assumption: that gamescope/libinput read the touchscreen
*without* grabbing it, so our exclusive grab takes it away from them system-wide. If gamescope grabs
first, the lock is a silent no-op — and since the feedback work landed the launcher now *announces*
the lock with a toast and a rumble, so it would be loudly announcing a state that never happened.
That is worse than the silence it replaced.

Assert on the grab, never on the toast. A human reporting "I saw the lock message" is not evidence
that touch went dead (TEST-PLAN §2C).

**Method.** We cannot inject into the real FTS3528 while the launcher may hold it — a grabbed node
delivers only to the grabbing client, so a write there proves nothing about gamescope. Instead we
create a synthetic multitouch panel with uinput, which gamescope's pointer emulation should treat
like any other touchscreen, and observe whether a tap becomes a click **in the page**. The observable
is Leanback's own state, never our bookkeeping (TEST-PLAN §0).

The three-way outcome is the point, and collapsing it would hide a lie:

    tap lands while grabbed    -> EVIOCGRAB does nothing. The feature is a no-op.
    tap lands only after release -> the grab works. The feature is real.
    tap never lands at all      -> the panel never reached the compositor. INCONCLUSIVE, not a pass.

The last row is why `test_synthetic_tap_reaches_the_page_when_unlocked` runs first as a positive
control. Without it, "no clicks observed" would read as a triumphant PASS from a harness that was
never connected to anything.
"""

from __future__ import annotations

import threading
import time

import pytest

from lib import uinput as u

# A page-side click recorder. Installed before the tap, read after: we assert the *page* saw a click,
# not that we wrote bytes to a device node. `capture=true` so nothing in Leanback can swallow it.
INSTALL_RECORDER = (
    "(function(){window.__deckbackClicks=0;"
    "if(window.__deckbackClickListener)"
    "document.removeEventListener('click',window.__deckbackClickListener,true);"
    "window.__deckbackClickListener=function(){window.__deckbackClicks++;};"
    "document.addEventListener('click',window.__deckbackClickListener,true);"
    "return true;})()"
)
READ_RECORDER = "window.__deckbackClicks|0"

STAGE1 = "/tmp/deckback_grab_stage1"
GO = "/tmp/deckback_grab_go"
STAGE2 = "/tmp/deckback_grab_stage2"
PANEL_NAME = "deckback-grab-test"

CLICK_SETTLE_S = 1.0  # gamescope -> Xwayland -> Blink


def _tap_via_uinput(deck, x=640, y=400):
    panel = u.touchscreen_spec()
    rc, _, err = deck.python(u.remote_program(panel, u.tap(x, y), settle=2.5), check=False)
    if rc == 3:
        pytest.skip(f"/dev/uinput unusable (environment, not a defect): {err.strip()}")
    assert rc == 0, f"uinput touch panel failed (rc={rc}): {err.strip()}"
    time.sleep(CLICK_SETTLE_S)


@pytest.mark.gate
@pytest.mark.uinput
def test_synthetic_tap_reaches_the_page_when_unlocked(deck, leanback):
    """The positive control. Without this, "no click" below means nothing at all."""
    leanback.evaluate(INSTALL_RECORDER)
    assert leanback.evaluate(READ_RECORDER) == 0

    _tap_via_uinput(deck)

    assert leanback.evaluate(READ_RECORDER) > 0, (
        "a synthetic multitouch tap produced no click in the page. gamescope's touch->pointer "
        "emulation may not apply to uinput-created panels, in which case this harness cannot "
        "measure the touch lock at all and the grab test is unsupported."
    )


# The grab/tap/release tail, spliced into the generated uinput program so the created fd stays open
# across the whole sequence. Synchronised with the runner through files rather than sleeps: a timing
# assumption here would turn a real result into a coin flip.
_GRAB_TAIL = r'''
import base64 as _b64, glob as _glob
EVIOCGRAB = {eviocgrab}
EVIOCGNAME_256 = {eviocgname}
tap_events = _b64.b64decode("{tap}")

mine = None
for p in sorted(_glob.glob("/dev/input/event*")):
    try:
        rfd = os.open(p, os.O_RDONLY | os.O_NONBLOCK)
    except OSError:
        continue
    buf = bytearray(256)
    try:
        fcntl.ioctl(rfd, EVIOCGNAME_256, buf)
        if buf.split(b"\0", 1)[0].decode(errors="replace") == "{panel}":
            mine = p
    except OSError:
        pass
    os.close(rfd)
    if mine:
        break

if mine is None:
    sys.stderr.write("grab: could not find the panel we just created\n")
    fcntl.ioctl(fd, UI_DEV_DESTROY); os.close(fd); sys.exit(3)

gfd = os.open(mine, os.O_RDONLY)
try:
    fcntl.ioctl(gfd, EVIOCGRAB, 1)
except OSError as e:
    # The grab was refused outright -- e.g. something else already holds it. That is a RESULT, and
    # exit 3 would misfile it as an environment problem. Say so and let the runner adjudicate.
    sys.stderr.write("grab: EVIOCGRAB refused: %s\n" % e)
    os.close(gfd); fcntl.ioctl(fd, UI_DEV_DESTROY); os.close(fd); sys.exit(2)

os.write(fd, tap_events)          # tap #1: while grabbed. Must NOT reach the compositor.
time.sleep({settle})
open("{stage1}", "w").close()

deadline = time.time() + 30
while not os.path.exists("{go}"):
    if time.time() > deadline:
        sys.stderr.write("grab: runner never sampled; giving up\n")
        fcntl.ioctl(gfd, EVIOCGRAB, 0); os.close(gfd)
        fcntl.ioctl(fd, UI_DEV_DESTROY); os.close(fd); sys.exit(3)
    time.sleep(0.05)

fcntl.ioctl(gfd, EVIOCGRAB, 0)
os.close(gfd)
time.sleep(0.3)
os.write(fd, tap_events)          # tap #2: after release. MUST reach the compositor.
time.sleep({settle})
open("{stage2}", "w").close()

fcntl.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)
print("uinput: ok")
'''


def _wait_for_file(deck, path, timeout=45):
    deadline = time.time() + timeout
    while time.time() < deadline:
        rc, _, _ = deck.run(f"test -f {path}", check=False)
        if rc == 0:
            return True
        time.sleep(0.2)
    return False


@pytest.mark.probe
@pytest.mark.uinput
def test_grab_starves_the_compositor_of_touch(deck, leanback):
    """THE test. Grab the panel from a helper process; assert the page stops seeing taps.

    The grab is taken by a standalone helper rather than through the app's chord, so a failure here
    indicts `EVIOCGRAB` itself rather than the chord, the toast, the config, or the launcher.
    """
    import base64

    deck.run(f"rm -f {STAGE1} {GO} {STAGE2}", check=False)
    leanback.evaluate(INSTALL_RECORDER)
    assert leanback.evaluate(READ_RECORDER) == 0

    panel = u.touchscreen_spec(PANEL_NAME)
    tail = _GRAB_TAIL.format(
        eviocgrab=u.ioc(u.IOC_WRITE, ord("E"), 0x90, 4),
        eviocgname=u.eviocgname(256),
        tap=base64.b64encode(u.encode_events(u.tap(640, 400))).decode(),
        panel=PANEL_NAME,
        settle=CLICK_SETTLE_S,
        stage1=STAGE1,
        go=GO,
        stage2=STAGE2,
    )
    program = u.remote_program(panel, events=(), settle=2.0).replace(
        'fcntl.ioctl(fd, UI_DEV_DESTROY)\nos.close(fd)\nprint("uinput: ok")', tail
    )
    assert tail in program, "the grab tail did not splice into the generated program"

    result = {}

    def run():
        result["rc"], result["out"], result["err"] = deck.python(program, check=False, timeout=120)

    worker = threading.Thread(target=run, daemon=True)
    worker.start()

    if not _wait_for_file(deck, STAGE1):
        worker.join(10)
        rc = result.get("rc")
        if rc == 3:
            pytest.skip(f"environment: {result.get('err', '').strip()}")
        pytest.fail(f"helper never reached stage 1 (rc={rc}): {result.get('err', '').strip()}")

    grabbed_clicks = leanback.evaluate(READ_RECORDER)
    deck.run(f"touch {GO}", check=False)

    assert _wait_for_file(deck, STAGE2), "helper never released the grab"
    worker.join(30)
    deck.run(f"rm -f {STAGE1} {GO} {STAGE2}", check=False)

    if result.get("rc") == 2:
        pytest.fail(f"EVIOCGRAB was refused outright: {result.get('err', '').strip()}")
    if result.get("rc") == 3:
        pytest.skip(f"environment: {result.get('err', '').strip()}")
    assert result.get("rc") == 0, f"grab helper failed: {result.get('err', '').strip()}"

    total = leanback.evaluate(READ_RECORDER)
    released_clicks = total - grabbed_clicks

    # Inconclusive, and it must never read as a pass: zero clicks throughout means the panel never
    # reached the compositor, so the grab was never tested.
    assert released_clicks > 0, (
        f"after releasing EVIOCGRAB the tap still did not reach the page "
        f"(grabbed={grabbed_clicks}, after_release={released_clicks}). The synthetic panel never "
        "delivered to the compositor, so this run says NOTHING about whether the grab works. "
        "Fix the positive control before trusting any result here."
    )
    assert grabbed_clicks == 0, (
        f"EVIOCGRAB did NOT starve the compositor: {grabbed_clicks} click(s) reached the page while "
        "the node was grabbed. The touch lock is a no-op, and the toast + rumble the launcher shows "
        "on lock announce a state that never happened. Register the finding in "
        ".internal/findings/durable/input-ux.md §4 and ship touch_lock_enabled=false."
    )


# ======================================================================================
# disable_touch — the SHIPPED behaviour (findings durable/touch-lock.md). The EVIOCGRAB lock above is
# dead on this platform; instead we make touch inert two independent ways. These tests assert the two
# halves actually engage on the deployed app.
# ======================================================================================

# Self-validating probe for Option A. A DETACHED node's own listener still fires (proving the event
# machinery works, so a 0 below means 'swallowed', not 'dispatch is broken'); an IN-TREE dispatch must
# be eaten by the launcher's window-capture swallow (no_pointer.js) before any page listener sees it.
_DISABLE_TOUCH_PROBE = r"""(function(){
  var d = document.createElement('div'); var control = 0;
  d.addEventListener('click', function(){ control++; });
  d.dispatchEvent(new MouseEvent('click'));
  var seen = 0; var probe = function(){ seen++; };
  window.addEventListener('click', probe, true);
  document.addEventListener('pointerdown', probe, true);
  document.body.dispatchEvent(new MouseEvent('click', {bubbles:true, cancelable:true}));
  document.body.dispatchEvent(new PointerEvent('pointerdown', {bubbles:true, cancelable:true}));
  window.removeEventListener('click', probe, true);
  document.removeEventListener('pointerdown', probe, true);
  return JSON.stringify({control: control, seen: seen});
})()"""


@pytest.mark.gate
def test_disable_touch_swallows_pointer_events(leanback):
    """Option A: no_pointer.js swallows pointer/mouse/touch in the page, so a finger — delivered as
    synthetic MOUSE events under gamescope — cannot navigate. Self-validating via the detached-node
    control, so `seen == 0` means swallowed, not that dispatch is broken."""
    import json

    raw = leanback.evaluate(_DISABLE_TOUCH_PROBE)
    assert raw, "probe returned nothing (no <body>? engine unreachable?)"
    r = json.loads(raw)
    assert r["control"] == 1, (
        f"event machinery is broken (control={r['control']}); the swallow result below would be "
        "meaningless, so this run is inconclusive, not a pass"
    )
    assert r["seen"] == 0, (
        f"{r['seen']} in-tree pointer event(s) reached the page — no_pointer.js is NOT swallowing "
        "them. Is disable_touch off, or did the document-start injection fail?"
    )


@pytest.mark.probe
def test_gamescope_touch_mode_is_hover_while_focused(deck_host):
    """Option B: TouchModeGuard holds gamescope's STEAM_TOUCH_CLICK_MODE at 0 (hover) while OUR window
    is focused. Skips — never fails — when our app is not the focused window, because the guard
    deliberately leaves the mode alone then.

    gamescope runs several Xwayland servers (--xwayland-count 2): Steam on :0, a Steam-launched game
    on :1. Our app — and the touch mode the guard manages — live on whichever display our
    content_shell is FOCUSED on, which is why a naive :0 check always sees 'steam' and skips."""
    from lib import ssh as sshlib

    sh = sshlib.Ssh(deck_host, sshlib.deck_port())
    target = None
    for disp in (":0", ":1", ":2"):
        _, cls, _ = sh.run(
            f"DISPLAY={disp} xdotool getwindowfocus getwindowclassname 2>/dev/null", check=False
        )
        if "content_shell" in (cls or "").lower():
            target = disp
            break
    if not target:
        pytest.skip(
            "our content_shell is not the focused window on any Xwayland display (app backgrounded or "
            "launched over SSH onto the wrong display); the guard only asserts hover while focused"
        )
    _, mode, _ = sh.run(f"DISPLAY={target} xprop -root STEAM_TOUCH_CLICK_MODE", check=False)
    assert "= 0" in (mode or ""), (
        f"our window is focused on {target} but gamescope touch mode is not hover: {mode.strip()!r}. "
        "TouchModeGuard should be holding STEAM_TOUCH_CLICK_MODE at 0."
    )

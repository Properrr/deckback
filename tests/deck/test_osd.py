"""The in-app OSD Settings menu (osd-menu-plan.md), on hardware.

Two layers, mirroring test_input.py's positive-control idea:

  * The `osd.js` component driven directly over CDP — focus/tab/scroll verdicts and the "capture <=>
    paint" DOM survival invariant. A pass tells you about **the component** and its CSP-safe render.
  * The full launcher path via /dev/uinput — Menu opens the OSD, B closes it, and input reaches
    Leanback again afterwards. A pass tells you about **the launcher wiring**.

The two prior bugs this guards (osd-menu-plan.md §2): (a) an action firing off its context, and (b) a
modal overlay trapping input while nothing is on screen. Test 5 is (b) end-to-end; test 3 is (b)'s DOM
invariant; the launcher never opening a dead-end menu covers the reported "Menu with no update blocks
input, expected A/B/Y".
"""

from __future__ import annotations

import json
import time
from pathlib import Path

import pytest

from lib import probes
from lib import uinput as u

_REPO = Path(__file__).resolve().parent.parent.parent
_OSD_JS = _REPO / "config" / "scripts" / "osd.js"

SETTLE_S = 0.6
# The launcher rescans /dev/input on a 2 s hotplug tick (input.cpp kHotplugScanMs), and the OSD's
# per-tick CDP work can stretch that interval. A fresh synthetic pad's event must not land inside
# that window or the launcher never opens the node in time to see it — so settle well past 2 s.
UINPUT_SETTLE_S = 4.0
OSD_PRESENT = "!!document.getElementById('__deckback_osd')"
OSD_ABSENT = "!document.getElementById('__deckback_osd')"


def _osd(cdp, **params):
    """Invoke the real osd.js body with `params` and return its string verdict."""
    return cdp.evaluate(_OSD_JS.read_text() + "(" + json.dumps(params) + ")")


def _cmd(cdp, cmd):
    return _osd(cdp, op="cmd", cmd=cmd)


def _open(cdp, *, tab="settings", has_update=False, about=False, about_features=None):
    buttons = (
        [["update.confirm", "Update now"], ["update.ignore", "Ignore this version"]]
        if has_update
        else []
    )
    about_kw = dict(
        about_name="Deckback",
        about_summary="Living-room video client for Steam Deck",
        about_desc="An unofficial native TV-interface client.",
        about_author="properrr",
        about_version="0.0.4",
        about_features=about_features
        or ["Controller-native", "Hardware VP9 decode", "Sleep & resume"],
        about_links=[["Project", "https://github.com/properrr/deckback"]],
    ) if about else {}
    return _osd(
        cdp,
        op="open",
        tab=tab,
        keys=[["A", "Select"], ["B", "Back"], ["Menu (☰)", "Settings"]],
        upd_has=has_update,
        upd_status=("v0.0.5 is available. You have v0.0.4." if has_update
                    else "Update status is not available."),
        upd_notes=("• A change\n• Another change" if has_update else ""),
        upd_buttons=buttons,
        **about_kw,
    )


@pytest.fixture(autouse=True)
def _cleanup(cdp):
    """Remove any OSD node a prior (possibly failed) test left, before and after."""
    yield
    try:
        cdp.evaluate(
            "(function(){var n=document.getElementById('__deckback_osd');if(n)n.remove();"
            "return true;})()"
        )
    except Exception:  # noqa: BLE001 - cleanup must never mask the real failure
        pass


# ---- the osd.js component over CDP ---------------------------------------------------------------


@pytest.mark.probe
def test_osd_renders_over_leanback(leanback):
    """The render claim: a CSP-safe overlay actually paints over youtube.com/tv (self-update.md).

    The launcher builds no HTML here — it injects this exact file — so we inject it too and check it
    lands and is styled (position:fixed, not an unstyled in-flow block, which is how the old pill/card
    silently failed on-Deck).
    """
    assert _open(leanback) == "ok"
    time.sleep(0.3)
    assert leanback.evaluate(OSD_PRESENT), "the OSD <div> did not survive on the page"
    assert leanback.evaluate(
        "getComputedStyle(document.getElementById('__deckback_osd')).position"
    ) == "fixed", "the OSD did not get its fixed-position style (CSP dropped it — self-update.md)"
    assert leanback.evaluate(
        "document.querySelectorAll('#__deckback_osd td.k').length"
    ) >= 1, "the Keys sub-tab rendered no rows"


@pytest.mark.gate
def test_osd_focus_and_verdicts(leanback):
    """Every command's verdict from the states that matter — the focus model with no L0 JS harness."""
    _open(leanback, tab="updates", has_update=True)

    # Updates has focusable buttons, so focus lands on the first one.
    assert leanback.evaluate("!!document.querySelector('#__deckback_osd .btn.dbf')"), (
        "opening Updates did not focus the first action button"
    )
    assert _cmd(leanback, "select") == "action:update.confirm"

    # Down moves focus; a second select fires the other action.
    assert _cmd(leanback, "down") == "consumed"
    assert _cmd(leanback, "select") == "action:update.ignore"

    # Contextual Y ignores only on the Updates tab.
    assert _cmd(leanback, "ignore") == "action:update.ignore"

    # Switch to Settings (Keys). No focusable widget there, so up/down scroll and Y does nothing.
    assert _cmd(leanback, "tab_prev") == "consumed"
    assert leanback.evaluate(
        "document.querySelector('#__deckback_osd .panel[data-tab=settings]').classList.contains('on')"
    ), "tab_prev did not activate the Settings panel"
    assert _cmd(leanback, "down") == "consumed"
    assert _cmd(leanback, "ignore") == "consumed", "Y ignored a version from the Keys tab"

    # Back at the top level closes.
    assert _cmd(leanback, "back") == "close"


@pytest.mark.gate
def test_osd_survives_leanback_body_swap(leanback):
    """Bug-b DOM invariant: a Leanback body swap must not strand the modal menu (self-update.md).

    input.cpp treats the menu as modal while open; if a swap detaches the node and it does not come
    back, the launcher swallows keys behind an absent menu ("keys don't work"). The shared keep-alive
    MutationObserver must re-append it.
    """
    assert _open(leanback) == "ok"
    assert leanback.evaluate(OSD_PRESENT)
    leanback.evaluate(
        "(function(){var o=document.getElementById('__deckback_osd');"
        "if(o&&o.parentNode)o.parentNode.removeChild(o);return true;})()"
    )
    reappeared = leanback.wait_for(
        "(function(){var n=document.getElementById('__deckback_osd');"
        "return !!n && n.isConnected;})()",
        timeout=5,
    )
    assert reappeared, (
        "keep-alive did not re-append the OSD after a body swap — input.cpp would keep swallowing "
        "keys behind an absent menu (the bug-b input trap)."
    )


@pytest.mark.gate
def test_cmd_during_a_detach_window_keeps_capture(leanback):
    """A command dispatched while the node is momentarily detached must keep capture, not release it.

    While the OSD is open the launcher captures input; a body swap can detach the node for a tick
    before the keep-alive observer re-appends it. A keypress landing in that window must return
    'consumed' (keep capture), not 'gone' — else the node re-appears painted with no owner and input
    passes through to Leanback behind a visible menu (the capture<=>paint invariant, self-update.md).
    """
    assert _open(leanback) == "ok"
    # Detach the node and dispatch a cmd in ONE synchronous eval, before the MutationObserver
    # microtask can re-append it, so osd.js genuinely sees an off-DOM but keep-alive-owned node.
    verdict = leanback.evaluate(
        "(function(){document.getElementById('__deckback_osd').remove();"
        "return (" + _OSD_JS.read_text() + ")({op:'cmd',cmd:'tab_next'});})()"
    )
    assert verdict == "consumed", (
        f"a cmd during a detach window returned {verdict!r} — must keep capture as 'consumed'"
    )
    # The observer re-appends it; the menu reports a live state again, proving it was never lost.
    assert leanback.wait_for(OSD_PRESENT, timeout=5), "keep-alive did not re-append after the cmd"
    assert _osd(leanback, op="state").startswith("tab="), "the re-appended menu is not driveable"


@pytest.mark.gate
def test_osd_tab_cycle_and_scroll(leanback):
    """L1/R1 cycle every tab in order, and a long-content tab's overflow actually scrolls.

    The About tab uses the same transform-based scroll region the changelog / future long-content
    tabs will depend on, so a padded feature list exercises the scroll-region contract on hardware.
    """
    long_feats = [f"Feature line number {i} that fills the About tab" for i in range(30)]
    assert _open(leanback, about=True, about_features=long_feats) == "ok"
    for key in ("settings", "updates", "about"):
        assert leanback.evaluate(
            f"!!document.querySelector('#__deckback_osd .tab[data-tab={key}]')"
        ), f"the {key} tab did not render"

    # tab_next cycles settings -> updates -> about and wraps back to settings.
    assert _cmd(leanback, "tab_next") == "consumed"
    assert _cmd(leanback, "tab_next") == "consumed"
    assert leanback.evaluate(
        "document.querySelector('#__deckback_osd .panel[data-tab=about]').classList.contains('on')"
    ), "two tab_next from Settings did not reach the About panel"

    region = "document.querySelector('#__deckback_osd .panel[data-tab=about] .scroll')"
    assert leanback.evaluate(f"{region}.__inner.scrollHeight > {region}.clientHeight"), (
        "the padded About content did not overflow its region, so the scrollbar can't be exercised"
    )
    assert _cmd(leanback, "scroll_down") == "consumed"
    # youtube.com/tv resets native scrollTop on any injected element (verified on-Deck), so the OSD
    # translates an inner wrapper instead. The sleep proves our offset PERSISTS — it doesn't get
    # reset the way scrollTop did.
    time.sleep(SETTLE_S)
    assert leanback.evaluate(f"{region}.__off") > 0, "scroll_down did not advance the scroll offset"
    assert leanback.evaluate(f"getComputedStyle({region}.__inner).transform") != "none", (
        "the About inner wrapper was not translated — the scroll offset is not being applied"
    )

    assert _cmd(leanback, "tab_next") == "consumed"
    assert leanback.evaluate(
        "document.querySelector('#__deckback_osd .panel[data-tab=settings]').classList.contains('on')"
    ), "tab_next did not wrap from About back to Settings"


# ---- the full launcher path via /dev/uinput ------------------------------------------------------


def _press(deck, code):
    """Play one button press on a fresh synthetic pad; skip on a missing udev rule (env, not defect)."""
    rc, _, err = deck.python(u.remote_program(u.gamepad_spec(), u.press(code), settle=UINPUT_SETTLE_S),
                             check=False)
    if rc == 3:
        pytest.skip(f"/dev/uinput unusable (environment): {err.strip()}")
    assert rc == 0, f"synthetic pad failed (rc={rc}): {err.strip()}"


@pytest.mark.gate
def test_about_tab_renders_its_content(leanback):
    """The About tab paints its metainfo-sourced fields — title, summary, features, version/author.

    The launcher single-sources this from the AppStream metainfo; here we drive the component with a
    known model and assert the shape renders (the on-device launcher path is checked separately).
    """
    assert _open(leanback, about=True) == "ok"
    assert leanback.evaluate("!!document.querySelector('#__deckback_osd .tab[data-tab=about]')"), (
        "the About tab did not render when about content was provided"
    )
    assert _cmd(leanback, "tab_next") == "consumed"  # settings -> updates
    assert _cmd(leanback, "tab_next") == "consumed"  # updates -> about
    about = "document.querySelector('#__deckback_osd .panel[data-tab=about]')"
    assert leanback.evaluate(f"{about}.classList.contains('on')"), "two tab_next did not reach About"
    assert leanback.evaluate(f"{about}.querySelector('.a-title').textContent") == "Deckback"
    assert leanback.evaluate(f"{about}.querySelectorAll('.a-feats li').length") == 3, (
        "the About feature list did not render"
    )
    txt = leanback.evaluate(f"{about}.querySelector('.a-meta').textContent")
    assert "Version 0.0.4" in txt and "properrr" in txt, f"About meta missing version/author: {txt!r}"


@pytest.mark.gate
@pytest.mark.uinput
def test_menu_opens_and_b_closes_the_osd(deck, leanback):
    """Physical Menu (BTN_START) opens the OSD off playback; B closes it.

    This is also the "Menu with nothing to offer" case: with no update available the menu still opens
    to a populated Settings/Keys view, so there is no dead-end (the reported bug-b variant).
    """
    _press(deck, u.BTN_START)
    assert leanback.wait_for(OSD_PRESENT, timeout=5), "Menu (BTN_START) did not open the OSD"
    assert leanback.evaluate(
        "document.querySelectorAll('#__deckback_osd td.k').length"
    ) >= 1, "the OSD opened but the Keys sub-tab is empty"

    _press(deck, u.BTN_EAST)  # B
    assert leanback.wait_for(OSD_ABSENT, timeout=5), "B did not close the OSD"


@pytest.mark.gate
@pytest.mark.uinput
def test_input_reaches_leanback_after_osd_closes(deck, leanback):
    """Bug b, end-to-end: after the menu closes, the D-pad must move Leanback focus again."""
    _press(deck, u.BTN_START)
    assert leanback.wait_for(OSD_PRESENT, timeout=5), "Menu did not open the OSD"
    _press(deck, u.BTN_EAST)  # B closes
    assert leanback.wait_for(OSD_ABSENT, timeout=5), "B did not close the OSD"

    before = leanback.evaluate(probes.ACTIVE_ELEMENT_EXPR)
    rc, _, err = deck.python(u.remote_program(u.gamepad_spec(), u.dpad(0, 1), settle=UINPUT_SETTLE_S),
                             check=False)
    if rc == 3:
        pytest.skip(f"/dev/uinput unusable: {err.strip()}")
    assert rc == 0, err
    time.sleep(SETTLE_S)
    assert leanback.evaluate(probes.ACTIVE_ELEMENT_EXPR) != before, (
        "a D-pad press after the OSD closed did not move Leanback focus — input is trapped, the "
        "exact bug-b failure the capture<=>paint invariant exists to prevent."
    )


@pytest.mark.gate
@pytest.mark.uinput
def test_capture_released_when_a_reload_wipes_the_open_menu(deck, leanback):
    """Regression (suspend/resume passthrough): press Menu, then the page reloads out from under the
    open menu — a full reload wipes the OSD's JS context, and keep-alive cannot bring it back. The
    launcher must not keep capturing input behind a menu that is no longer painted, or every key
    passes through to Leanback while a (stale or absent) menu sits on screen. capture <=> paint.

    A same-URL location.reload() is the tractable stand-in for a Game-Mode suspend/resume: both leave
    the navigator's not-app->app transition unfired, so only the input-tick reconciler can catch it.
    """
    _press(deck, u.BTN_START)
    assert leanback.wait_for(OSD_PRESENT, timeout=5), "Menu did not open the OSD"

    leanback.evaluate("location.reload()")
    # The reloaded page has no OSD node (context wiped); wait for Leanback itself to come back.
    app_ready = ("location.href.indexOf('youtube.com/tv') >= 0 && "
                 "document.querySelectorAll('[class*=zylon]').length > 3")
    assert leanback.wait_for(OSD_ABSENT, timeout=20), "page did not reload"
    assert leanback.wait_for(app_ready, timeout=30), "Leanback did not finish reloading"
    time.sleep(3)  # let the reconciler's throttled tick (>=750 ms) release capture

    before = leanback.evaluate(probes.ACTIVE_ELEMENT_EXPR)
    rc, _, err = deck.python(u.remote_program(u.gamepad_spec(), u.dpad(0, 1), settle=UINPUT_SETTLE_S),
                             check=False)
    if rc == 3:
        pytest.skip(f"/dev/uinput unusable: {err.strip()}")
    assert rc == 0, err
    time.sleep(SETTLE_S)
    assert leanback.evaluate(probes.ACTIVE_ELEMENT_EXPR) != before, (
        "after the reload wiped the open menu, a D-pad press did not reach Leanback — input is "
        "trapped behind a menu the launcher still thinks it owns (the suspend/resume passthrough bug)."
    )

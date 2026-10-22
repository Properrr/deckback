"""Injected overlays (controls card, toast) and the error page render and respond on hardware.

These injections fail silently by design, so the only check is to look at the DOM.
"""

from __future__ import annotations

import time

import pytest

from lib import probes

ERROR_PAGE_PRESENT = "!!document.getElementById('__deckback_error')"
ERROR_RETRY_FOCUSED = (
    "(function(){var b=document.getElementById('__deckback_retry');"
    "return !!b && document.activeElement === b;})()"
)
HELP_CARD_PRESENT = "!!document.getElementById('__deckback_help')"
HELP_ROW_COUNT = (
    "(function(){var c=document.getElementById('__deckback_help');"
    "return c?c.querySelectorAll('tr').length:0;})()"
)
TOAST_PRESENT = "!!document.getElementById('__deckback_toast')"


@pytest.mark.probe
def test_controls_card_can_render_over_leanback(leanback, deck):
    """A CDP-injected controls-card <div> survives Leanback's DOM management."""
    # Mirrors the launcher's C++-built JS: same id, appended to documentElement, innerHTML through
    # the same Trusted Types policy (a bare innerHTML throws under youtube.com/tv's CSP).
    html = probes.trusted_html_js("'<table><tr><td>A</td><td>Select</td></tr></table>'")
    leanback.evaluate(
        "(function(){var d=document.createElement('div');d.id='__deckback_help';"
        "d.innerHTML=" + html + ";"
        "document.documentElement.appendChild(d);return true;})()"
    )
    time.sleep(0.3)
    assert leanback.evaluate(HELP_CARD_PRESENT), (
        "a <div> appended to document.documentElement did not survive. Both the controls card "
        "(input-ux §17) and the touch-lock toast (§14) are built this way and are silent no-ops "
        "if it fails."
    )
    assert leanback.evaluate(HELP_ROW_COUNT) >= 1
    leanback.evaluate("(function(){var n=document.getElementById('__deckback_help');"
                      "if(n)n.remove();return true;})()")


@pytest.mark.probe
def test_toast_can_render_over_leanback(leanback):
    """A CDP-injected toast <div> survives Leanback's DOM management."""
    leanback.evaluate(
        "(function(){var d=document.createElement('div');d.id='__deckback_toast';"
        "d.textContent='probe';document.documentElement.appendChild(d);return true;})()"
    )
    time.sleep(0.3)
    assert leanback.evaluate(TOAST_PRESENT), "the toast <div> did not survive on the page"
    leanback.evaluate("(function(){var n=document.getElementById('__deckback_toast');"
                      "if(n)n.remove();return true;})()")


@pytest.mark.probe
def test_error_page_renders_and_focuses_retry(cdp, deck):
    """A failed navigation shows our error page with Retry focused, then recovers to Leanback."""
    cdp.call("Page.navigate", {"url": "https://deckback-invalid.invalid/"})

    # The launcher polls every devtools_poll_ms (default 1000).
    appeared = cdp.wait_for(ERROR_PAGE_PRESENT, timeout=20)
    assert appeared, (
        "the launcher's error page never appeared after a failed navigation. The user is looking at "
        "Chromium's desktop interstitial, whose Reload button no controller can focus."
    )
    assert cdp.evaluate(ERROR_RETRY_FOCUSED), (
        "the Retry button exists but is not focused. On a controller there is no cursor: an "
        "unfocused button is an unreachable one, which is the failure this page replaces."
    )

    # The launcher's own backoff returns to the TV app.
    back = cdp.wait_for(probes.ON_LEANBACK_EXPR, timeout=60)
    assert back, "the launcher never recovered from the error page back to the TV app"


@pytest.mark.probe
def test_injected_enter_activates_our_retry_button(cdp):
    """An injected Enter reaches a focused button on our own about:blank document."""
    cdp.call("Page.navigate", {"url": "about:blank"})
    cdp.wait_for("document.readyState === 'complete'", timeout=10)
    cdp.evaluate(
        "(function(){document.documentElement.innerHTML="
        "'<body><button id=b>x</button></body>';"
        "window.__hit=0;var b=document.getElementById('b');b.focus();"
        "document.addEventListener('keydown',function(e){if(e.key==='Enter')window.__hit++;});"
        "return true;})()"
    )
    cdp.dispatch_key("Enter")
    time.sleep(0.3)
    assert cdp.evaluate("window.__hit|0") > 0, (
        "an injected Enter did not reach a button on our own about:blank document. The error page's "
        "Retry button cannot be activated, and the kiosk failure state is still a dead end."
    )

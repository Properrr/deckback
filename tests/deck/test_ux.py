"""The two surfaces a user hits when things go wrong or when they arrive for the first time.

Both were built without hardware, and both fail *silently* by design: a CDP injection that produces a
syntax error renders nothing and reports nothing (a failed toast must never break the touch lock it
announces). So there is exactly one way to know whether they render: look.
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
    """input-ux §17. Does a CDP-injected <div> survive Leanback's own DOM management?

    Forced by deleting the first-run marker and restarting is too heavy for a probe; instead we
    inject the same JS the launcher injects and check it lands. That tests the *rendering* claim,
    which is the unverified one. Whether the launcher calls it at the right moment is covered at L0.
    """
    # The launcher builds this JS in C++, so we cannot import it. Re-create the essential shape: an
    # element with the same id, appended to documentElement, its innerHTML assigned through the SAME
    # Trusted Types policy the launcher uses (probes.trusted_html_js mirrors js_trusted_html). A bare
    # innerHTML here throws under youtube.com/tv's CSP -- which is what this test failed on once the
    # launcher was fixed and this inline copy was not (2026-07-10). The escaping is covered at L0
    # (overlay_test, onboarding_test); what is unproven on hardware is whether the node survives.
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
    """input-ux §14. Same claim, different id — and the toast is what the touch lock relies on."""
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
    """input-ux §16. Sever DNS, assert OUR page appears and no Chromium interstitial is reachable.

    Destructive: it edits the app's view of the network. We restore by navigating back, and the
    launcher's own retry loop recovers on its own if we die mid-test.
    """
    # Navigate somewhere that cannot resolve. This exercises the real path: Page.navigate reports
    # failure via errorText, and the launcher must notice and draw its own page.
    cdp.call("Page.navigate", {"url": "https://deckback-invalid.invalid/"})

    # The launcher polls; give it a couple of ticks (devtools_poll_ms defaults to 1000).
    appeared = cdp.wait_for(ERROR_PAGE_PRESENT, timeout=20)
    assert appeared, (
        "the launcher's error page never appeared after a failed navigation. The user is looking at "
        "Chromium's desktop interstitial, whose Reload button no controller can focus."
    )
    assert cdp.evaluate(ERROR_RETRY_FOCUSED), (
        "the Retry button exists but is not focused. On a controller there is no cursor: an "
        "unfocused button is an unreachable one, which is the failure this page replaces."
    )

    # And it recovers: the launcher's own backoff should take us home without our help.
    back = cdp.wait_for(probes.ON_LEANBACK_EXPR, timeout=60)
    assert back, "the launcher never recovered from the error page back to the TV app"


@pytest.mark.probe
def test_injected_enter_activates_our_retry_button(cdp):
    """The retry path only works if our own injected Enter reaches our own page.

    Distinct from `test_cdp_arrow_moves_focus`: that proves keys reach *Leanback*. This proves they
    reach a document we created on about:blank, which is a different security origin.
    """
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

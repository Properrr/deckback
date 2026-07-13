#!/usr/bin/env bash
# Headless boot + DevTools-protocol assertion + screenshot. Same script CI runs (doc §14.1).
# Also runnable nightly against production youtube.com/tv as an R1/R2 Leanback-drift canary.
#
# What it gates (T6):
#   1. the TV UA lands on Leanback: `ytlr-app` present, no `app=desktop` redirect, no cast screen;
#   2. the UA override SURVIVED that navigation (--assert-ua) — it is sticky in DevToolsClient, and
#      when it silently is not, every later probe measures the desktop site instead;
#   3. AV1 steering works through all three APIs YouTube probes, and does not over-reach onto
#      VP9/H.264 (--assert-av1-steering).
#
# (3) injects config/av1_steering.js — the same file CMake compiles into the launcher — so this
# tests the script that ships rather than a copy of it. It also runs a BASELINE first: before the
# script is injected, MediaSource.isTypeSupported(av01...) must be true. An engine built without any
# AV1 decoder would otherwise satisfy every steering assertion while proving nothing at all, which is
# `just power` reporting a perfect 0.00 W from a battery with no telemetry, in a different costume.
#
# Exit 2 means a check failed and the product is wrong. See scripts/cdp.py:main().
#
# NOTE (S0.2): content_shell IGNORES --user-agent (Cobalt hardcodes a Chrome/999 UA), so the TV UA
# is injected over CDP (Network.setUserAgentOverride) via scripts/cdp.py, not a launch flag.
# --no-sandbox is for THIS headless harness only (no user-ns/SUID sandbox in the build container);
# the shipped Flatpak sandboxes via zypak — never --no-sandbox (Flathub rejects it).
. "$(dirname "$0")/lib.sh"
require_cobalt_checkout
preset="${1:-dev}"
port=9222

info "Headless Leanback smoke: out/${preset}/${COBALT_TARGET} + CDP assertion ..."
in_container bash -lc "
  set -u
  cd $CTR_TREE
  # 'You never built this preset' is ENV (3), not TRANSPORT (4). Without this guard the engine simply
  # never starts, cdp.py waits 20s for a DevTools endpoint that nothing is serving, and reports a
  # transport failure — which tells an automated runner to RETRY. It would retry a build that does
  # not exist, forever. cert.sh has always had this check; smoke.sh did not, and the default preset
  # here is 'dev' while the tree usually has 'deck'.
  [ -x out/${preset}/${COBALT_TARGET} ] || { echo 'no engine at out/${preset}/${COBALT_TARGET} — run: just build ${preset}' >&2; exit 3; }
  UA=\$(python3 -c \"import json;print(json.load(open('/work/config/app.json'))['user_agent'])\")
  URL=\$(python3 -c \"import json;print(json.load(open('/work/config/app.json'))['url'])\")
  Xvfb :99 -screen 0 1280x720x24 >/tmp/xvfb.log 2>&1 &
  export DISPLAY=:99
  sleep 2
  out/${preset}/${COBALT_TARGET} --remote-debugging-port=${port} --no-first-run --no-sandbox \
    --window-size=1280,720 --data-path=/tmp/deckback-smoke about:blank >/tmp/cobalt-smoke.log 2>&1 &
  cobalt_pid=\$! ;
  python3 /work/scripts/cdp.py --port ${port} --ua \"\$UA\" --navigate \"\$URL\" \
    --add-script /work/config/scripts/av1_steering.js \
    --expect \"!!document.querySelector('ytlr-app') && !location.href.includes('app=desktop') && !/ready to cast/i.test(document.body.innerText)\" \
    --assert-ua --assert-av1-steering \
    --ready-timeout 60 --out out/${preset}/smoke.png ;
  rc=\$? ;
  kill \$cobalt_pid 2>/dev/null ;
  exit \$rc
"
info "Smoke passed: Leanback loaded under the TV UA, and AV1 steering held. Screenshot -> ${COBALT_TREE}/out/${preset}/smoke.png"

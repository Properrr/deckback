#!/usr/bin/env bash
# L3 conformance (TEST-PLAN §6): run the self-hosted js_mse_eme suites against our engine.
#
#   scripts/cert.sh [suite] [preset] [-- extra cert.py args]   # workstation, Xvfb + content_shell
#   scripts/cert.sh --deck [suite]                             # real hardware, over SSH
#
# Exit codes: 0 no regressions · 2 a passing test stopped passing, or the suite never finished ·
#             3 no baseline / no harness / vector hash moved / insecure origin · 4 transport · 5 usage.
#
# **Passing js_mse_eme is NOT YouTube certification** and must never be described as such
# (docs/legal.md). It is a regression net built from the harness Google used to certify devices.
#
# Most of the conformance value needs no Deck: MSE and codec correctness are engine behaviour, and
# they run headless in the build image next to `just smoke`. That is what makes this gate cheap
# enough to survive a busy week — a gate that needs hardware for every PR gets skipped for every PR.
#
# The harness is served on 127.0.0.1 only. That is load-bearing twice over: EME requires a secure
# context (`http://localhost` qualifies, a LAN IP does not), and the harness POSTs its results to
# qual-e.appspot.com when the page URL contains appspot.com/googleapis.com. cert.py asserts both
# before running a single test.
#
# CI must never fetch Google's Widevine CDM, so the automated run is **ClearKey only**. Widevine key
# systems stay `expected-fail` in the baseline until the P7 AddContentDecryptionModules patch lands.
. "$(dirname "$0")/lib.sh"

port="${DECKBACK_CERT_PORT:-8000}"
cdp_port=9222

# --- the Deck tier: the app is already running there; we drive it from here -------------------------
if [ "${1:-}" = "--deck" ]; then
  shift
  suite="${1:-conformance-test}"
  shift || true
  require_deck
  info "cert-deck: ${suite} against ${DECK_HOST} (the app must already be running there)"
  exec python3 "$(dirname "$0")/cert.py" --suite "$suite" --deck --port "$port" \
    --cdp-port "$cdp_port" "$@"
fi

# --- the workstation tier: Xvfb + content_shell, inside the build container ------------------------
require_cobalt_checkout
suite="${1:-conformance-test}"
preset="${2:-dev}"
shift 2 2>/dev/null || shift $# # remaining args pass through to cert.py

info "cert: ${suite} on out/${preset}/${COBALT_TARGET} (headless) ..."

# --autoplay-policy: the harness plays its own media with no user gesture. Without it every media
# test times out waiting for a play() the browser silently refused, and the suite reads as a decoder
# failure. --no-sandbox is for THIS headless harness only; the shipped Flatpak sandboxes via zypak.
#
# `set -u`, deliberately NOT `set -e`: `-l` makes this a login shell, and under `set -e` bash runs
# ~/.bash_logout on the way out, whose last command fails in our image and replaces the exit status
# with 1 — flattening even a deliberate `exit 3`. See finding F8.
in_container bash -lc "
  set -u
  cd $CTR_TREE
  [ -x out/${preset}/${COBALT_TARGET} ] || { echo 'no engine: just build ${preset}' >&2; exit 3; }
  Xvfb :99 -screen 0 1280x720x24 >/tmp/xvfb-cert.log 2>&1 &
  export DISPLAY=:99
  sleep 2
  out/${preset}/${COBALT_TARGET} --remote-debugging-port=${cdp_port} --no-first-run --no-sandbox \
    --autoplay-policy=no-user-gesture-required --window-size=1280,720 \
    --data-path=/tmp/deckback-cert about:blank >/tmp/cobalt-cert.log 2>&1 &
  shell_pid=\$! ;
  python3 /work/scripts/cert.py --suite '${suite}' --port ${port} --cdp-port ${cdp_port} $* ;
  rc=\$? ;
  kill \$shell_pid 2>/dev/null ;
  exit \$rc
"

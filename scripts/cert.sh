#!/usr/bin/env bash
# L3 conformance: run the self-hosted js_mse_eme suites against our engine.
#
#   scripts/cert.sh [suite] [preset] [-- extra cert.py args]   # workstation, Xvfb + content_shell
#   scripts/cert.sh --deck [suite]                             # real hardware, over SSH
#
# Exit codes: 0 no regressions · 2 a passing test stopped passing, or the suite never finished ·
#             3 no baseline / no harness / vector hash moved / insecure origin · 4 transport · 5 usage.
#
# Passing js_mse_eme is NOT YouTube certification (docs/legal.md). The harness is served on 127.0.0.1
# only: EME needs a secure context, and the harness POSTs results to qual-e.appspot.com when the page
# URL contains appspot.com/googleapis.com. ClearKey only: CI must never fetch Google's Widevine CDM.
# Env: DECKBACK_CERT_PORT (default 8000).
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

# --autoplay-policy: the harness plays media with no user gesture. --no-sandbox is for this headless
# harness only. `set -u`, NOT `set -e`: `-l` makes this a login shell, and under `set -e` the failing
# last command of ~/.bash_logout replaces the exit status (even a deliberate `exit 3`).
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

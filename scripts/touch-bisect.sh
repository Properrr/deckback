#!/usr/bin/env bash
# Does the gesture router still receive touch events? Answered OFF-DECK, in the build container.
#
# For each engine preset in $1 (default "deck release") x {router, router+hide_cursor}: boot the
# engine headless under Xvfb, inject the page scripts, dispatch a real touch sequence through CDP
# `Input.dispatchTouchEvent`, and report what the router counted. This covers Blink's touch dispatch
# and above, NOT gamescope -> Xwayland -> XI2: a failure is conclusive, a pass narrows it to below.
#
# Exit codes: 0 ran and reported · 2 a cell regressed (see the table) · 3 environment (no engine,
# no container) · 5 usage.
. "$(dirname "$0")/lib.sh"

port=9223
engines="${1:-deck release}"

require_cobalt_checkout

for preset in $engines; do
  [ -x "$COBALT_TREE/out/$preset/$COBALT_TARGET" ] ||
    die_env "no engine at out/$preset/$COBALT_TARGET — build it, or pass only the presets you have"
done

info "Touch bisect: engines [$engines] x {router, router+hide_cursor}"

in_container bash -lc "
  set -u
  cd $CTR_TREE
  Xvfb :99 -screen 0 1280x800x24 >/tmp/xvfb-bisect.log 2>&1 &
  export DISPLAY=:99
  sleep 2
  rc=0
  for preset in $engines; do
    for mode in router router+cursor; do
      out/\$preset/$COBALT_TARGET --remote-debugging-port=$port --no-first-run --no-sandbox \
        --window-size=1280,800 --data-path=/tmp/deckback-bisect-\$preset-\$mode about:blank \
        >/tmp/cobalt-bisect.log 2>&1 &
      pid=\$!
      python3 /work/scripts/touch_bisect_probe.py --port $port --preset \$preset --mode \$mode || rc=2
      kill \$pid 2>/dev/null
      wait \$pid 2>/dev/null
    done
  done
  exit \$rc
"

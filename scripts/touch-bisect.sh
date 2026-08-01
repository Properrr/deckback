#!/usr/bin/env bash
# Does the gesture router still receive touch events? Answered OFF-DECK, in the build container.
#
# durable/touch-gestures.md §7.0: on the v0.0.9 build a real finger produced ZERO events of any
# family, and TWO things had changed at once — the cursor-hiding script, and the whole engine binary
# (`gold`+ThinLTO from out/release, where every hand-tested build was `deck`/qa from out/deck).
# Arguing about which is cheap and unconvincing; this runs the 2x2.
#
# For each engine, with and without hide_cursor.js, it boots the engine headless under Xvfb, injects
# the real page scripts, dispatches a REAL touch sequence through CDP `Input.dispatchTouchEvent`
# (trusted, engine-level — not a synthetic DOM event), and reports what the router counted.
#
# WHAT THIS CAN AND CANNOT SHOW. CDP injects touch inside the browser, so this exercises Blink's
# touch dispatch and everything above it — which is exactly where a page script could interfere. It
# does NOT exercise gamescope -> Xwayland -> XI2, so a compositor-level cause would pass here.
# A FAILURE here is therefore conclusive; a pass narrows the question to the layer below.
#
# Exit codes (.internal/HARNESS.md §1): 0 ran and reported · 2 a cell regressed (see the table) ·
# 3 environment (no engine, no container) · 5 usage.
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

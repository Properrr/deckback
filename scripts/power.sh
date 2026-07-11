#!/usr/bin/env bash
# P4 acceptance: poll Deck battery draw during playback -> CSV. Target <= ~9 W for 1080p VP9.
#
# Exit codes: 0 pass · 2 gate failed (mean above the budget, or the wrong decoder) ·
#             3 no usable battery telemetry / nothing playing · 4 Deck unreachable · 5 usage.
#
# HISTORY (do not regress). Two ways this script has reported a perfect score from nothing:
#
#   1. It printed a confident `mean 0.00 W over 300 samples` and exited 0 when the battery sysfs node
#      was absent, because `awk` reads an empty string as numeric 0. The P4 gate is "<= 9 W", so *no
#      data at all* silently PASSED it. Every read is now validated and a missing node is a hard error.
#
#   2. It did not know whether a video was playing. A 300-second average taken over a **paused**
#      video passes 9 W effortlessly and describes nothing. The same bug one layer up: a measurement
#      of the absence of the thing being measured. It now asserts over CDP that `currentTime` is
#      advancing — before sampling AND after — and refuses the run otherwise.
#
# And a third that would have been just as quiet: a draw measured under `Dav1dVideoDecoder` is a
# number about software AV1 decode, a configuration we do not ship. `--require-vaapi` makes that a
# hard failure rather than a mysteriously bad score.
#
#   4. It could not run at all on the only hardware this project owns. It required
#      `BAT*/power_now`; the OLED (Galileo) gauge is charge-based and has no such node anywhere in
#      /sys, so the P4 gate exited 3 forever and nobody noticed, because "environment" reads like
#      somebody else's problem. Watts are now computed as V x I when power_now is absent.
#
#   5. Two ways to measure the wrong machine, both of which yield a believable number under budget:
#      sampling ON AC (where current_now is the *charging* current, so the reading describes energy
#      entering the battery) and sampling with the PANEL OFF (which omits watts of display draw).
#      Both are now hard preconditions.
. "$(dirname "$0")/lib.sh"
require_deck

secs="${1:-300}"
case "$secs" in
'' | *[!0-9]*) die_usage "usage: power.sh [seconds] (got '$secs')" ;;
esac
out="power-$(git rev-parse --short HEAD 2>/dev/null || echo local).csv"

# deckctl.py already answers in this taxonomy (2 assert / 3 env / 4 transport), so propagate its code
# verbatim instead of flattening every precondition into one generic failure.
deckctl() {
  python3 "$(dirname "$0")/deckctl.py" "$@" || exit $?
}

# Machine state first, app state second. Both must hold, but the machine's is cheaper to check and
# more fundamental: there is no point asking someone to start a video on a Deck that is plugged in,
# only to refuse the run they just set up.
# --- Refuse to measure the wrong machine ----------------------------------------------------------
# On AC, `current_now` is the CHARGE current: P = V x I then describes energy flowing INTO the
# battery (~4.6 W on this Deck) — a plausible number, under the gate, about nothing.
ac="$(deck_ssh "$DECK_HOST" 'cat /sys/class/power_supply/A[CD]*/online 2>/dev/null || true')"
if ac_is_online "$ac"; then
  die_env "${DECK_HOST} is on AC. Unplug it: on the charger the battery current is the charging
   current, so a P = V x I sample measures energy going into the battery, not the draw of the app.
   It would read a plausible few watts and PASS the <=${DECKBACK_MAX_WATTS:-9} W gate while
   measuring the opposite of what the gate is for."
fi

# A dark panel understates a real session by watts on a device whose entire gate is "<= ~9 W".
panel="$(deck_ssh "$DECK_HOST" '
  d=$(cat /sys/class/drm/card*-eDP-1/dpms 2>/dev/null | head -1)
  b=$(cat /sys/class/backlight/*/brightness 2>/dev/null | head -1)
  printf "%s %s" "${d:-unknown}" "${b:-unknown}"')"
if panel_is_dark "${panel% *}" "${panel##* }"; then
  if [ "${DECKBACK_PANEL_OFF_OK:-0}" = 1 ]; then
    info "panel is off (dpms=${panel% *} brightness=${panel##* }) — DECKBACK_PANEL_OFF_OK=1, sampling anyway."
    info "The number below EXCLUDES panel draw. It is not comparable to the P4 budget."
  else
    die_env "${DECK_HOST}'s panel is off (dpms=${panel% *} brightness=${panel##* }).
   A draw sampled with the screen dark is watts below a real session and cannot be compared to the
   P4 budget. Turn the display back on, or set DECKBACK_PANEL_OFF_OK=1 to sample anyway and accept
   that the result describes a machine nobody uses."
  fi
fi

# --- Resolve how this Deck reports power ----------------------------------------------------------
# `power_now` (uW) is the easy case. The OLED (Galileo) does not have it: its gauge is charge-based
# and exposes only voltage_now (uV) and current_now (uA), so watts must be computed. Probing for the
# method up front means a Deck with no usable telemetry fails here, loudly, instead of averaging
# empty strings to a perfect 0.00 W (F1).
info "Locating battery telemetry on ${DECK_HOST} ..."
probe="$(deck_ssh "$DECK_HOST" '
  for f in /sys/class/power_supply/BAT*/power_now; do
    [ -r "$f" ] || continue
    v=$(cat "$f" 2>/dev/null) || continue
    case "$v" in "" | *[!0-9]*) continue ;; esac
    [ "$v" -gt 0 ] || continue
    printf "power_now %s %s" "$f" ""; exit 0
  done
  for d in /sys/class/power_supply/BAT*; do
    [ -r "$d/voltage_now" ] && [ -r "$d/current_now" ] || continue
    uv=$(cat "$d/voltage_now" 2>/dev/null); ua=$(cat "$d/current_now" 2>/dev/null)
    case "$uv$ua" in "" | *[!0-9]*) continue ;; esac
    [ "$uv" -gt 0 ] && [ "$ua" -gt 0 ] || continue
    printf "vi %s %s" "$d/voltage_now" "$d/current_now"; exit 0
  done
  exit 3')" || die_env \
  "no usable battery telemetry on ${DECK_HOST}: no BAT*/power_now, and no BAT* exposing both
   voltage_now and current_now with non-zero values.
   Refusing to sample: empty reads average to 0.00 W and would silently PASS the <=9 W gate."

method="${probe%% *}"
rest="${probe#* }"
node1="${rest%% *}"
node2="${rest#* }"

# --- Now the app state ----------------------------------------------------------------------------
info "Checking the app is actually playing something on ${DECK_HOST} ..."
deckctl playing --window 4

info "Checking the engine is using the VA-API decoder ..."
deckctl decoder --require-vaapi >/dev/null

case "$method" in
power_now) info "Battery reports power_now directly (${node1}). Sampling ${secs}s -> ${out}" ;;
vi) info "No power_now on this Deck; computing P = V x I from ${node1} and ${node2}. Sampling ${secs}s -> ${out}" ;;
esac

echo "sample,watts" >"$out"
# Sample RAW microunits remotely; convert host-side. The multiply that decides what the gate measures
# belongs somewhere a test can execute it (scripts/lib/power_watts.awk, tests/harness/).
# An unreadable sample arrives as an empty field and voids the run rather than averaging in as zero.
deck_ssh "$DECK_HOST" "
  for i in \$(seq 1 ${secs}); do
    if [ '${method}' = power_now ]; then
      printf '%s,%s,\n' \"\$i\" \"\$(cat '${node1}' 2>/dev/null || true)\"
    else
      printf '%s,%s,%s\n' \"\$i\" \"\$(cat '${node1}' 2>/dev/null || true)\" \"\$(cat '${node2}' 2>/dev/null || true)\"
    fi
    sleep 1
  done
" | awk -F, -v method="$method" -f scripts/lib/power_watts.awk >>"$out" ||
  die_transport "sampling loop died (SSH dropped?) — partial CSV at $out"

# Playback must still be running. If the video ended, buffered out, or the app died at second 40, the
# remaining 260 samples are an idle-menu measurement wearing a playback measurement's name.
info "Confirming playback survived the whole sampling window ..."
deckctl playing --window 4

# Adjudicate locally. Any invalid sample voids the run: a mean over a silently-truncated subset
# understates draw, which is the same failure mode in a new costume. The rules live in a separate awk
# program so they can be tested without a Deck (tests/harness/).
awk -F, -v max="${DECKBACK_MAX_WATTS:-9}" -f scripts/lib/power_adjudicate.awk "$out"

#!/usr/bin/env bash
# P4 acceptance: poll Deck battery draw during playback -> CSV. Target <= ~9 W for 1080p VP9.
# Refuses to sample on AC, with the panel off, without battery telemetry, or unless a video is playing
# on the VA-API decoder before and after the window.
# Env: DECKBACK_MAX_WATTS (gate, default 9) · DECKBACK_PANEL_OFF_OK=1 (sample with the panel dark).
#
# Exit codes: 0 pass · 2 gate failed (mean above the budget, or the wrong decoder) ·
#             3 no usable battery telemetry / nothing playing · 4 Deck unreachable · 5 usage.
#
. "$(dirname "$0")/lib.sh"
require_deck

secs="${1:-300}"
case "$secs" in
'' | *[!0-9]*) die_usage "usage: power.sh [seconds] (got '$secs')" ;;
esac
out="power-$(git rev-parse --short HEAD 2>/dev/null || echo local).csv"

# deckctl.py exits in the same taxonomy; propagate its code verbatim.
deckctl() {
  python3 "$(dirname "$0")/deckctl.py" "$@" || exit $?
}

# --- Refuse to measure the wrong machine ----------------------------------------------------------
ac="$(deck_ssh "$DECK_HOST" 'cat /sys/class/power_supply/A[CD]*/online 2>/dev/null || true')"
if ac_is_online "$ac"; then
  die_env "${DECK_HOST} is on AC. Unplug it: on the charger the battery current is the charging
   current, so a P = V x I sample measures energy going into the battery, not the draw of the app.
   It would read a plausible few watts and PASS the <=${DECKBACK_MAX_WATTS:-9} W gate while
   measuring the opposite of what the gate is for."
fi

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
# `power_now` (uW) when present; the OLED gauge exposes only voltage_now (uV) and current_now (uA),
# so watts are V x I.
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
# Sample raw microunits remotely; scripts/lib/power_watts.awk converts host-side. An unreadable sample
# arrives as an empty field and voids the run.
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

info "Confirming playback survived the whole sampling window ..."
deckctl playing --window 4

# Any invalid sample voids the run (rules in scripts/lib/power_adjudicate.awk).
awk -F, -v max="${DECKBACK_MAX_WATTS:-9}" -f scripts/lib/power_adjudicate.awk "$out"

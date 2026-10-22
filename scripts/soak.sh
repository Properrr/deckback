#!/usr/bin/env bash
# P6 acceptance: n x suspend/resume mid-playback. Each cycle requires the app alive and `currentTime`
# advanced over CDP (a rewind is reported separately from a stall). EPP change after resume
# (steamos#2383) is reported, never gated: restoring it needs root. Not checked: audio restored,
# screen did not dim.
#
#   scripts/soak.sh [cycles]   (default 25; needs passwordless `sudo rtcwake` on the Deck)
#
# Exit codes: 0 ok · 2 the app died, stalled, or restarted the video across a resume (real defect) ·
#             3 rtcwake unusable / nothing playing · 4 Deck unreachable · 5 usage.
. "$(dirname "$0")/lib.sh"
require_deck

n="${1:-25}"
case "$n" in
'' | *[!0-9]*) die_usage "usage: soak.sh [cycles] (got '$n')" ;;
esac

deckctl() { python3 "$(dirname "$0")/deckctl.py" "$@"; }

# rtcwake needs root to arm the RTC alarm; check passwordless sudo once, before any setup.
if ! deck_ssh "$DECK_HOST" 'sudo -n rtcwake --help >/dev/null 2>&1'; then
  die_env "passwordless 'sudo rtcwake' is not configured on ${DECK_HOST}, so the suspend/resume loop
   cannot arm an RTC wake alarm (that needs root). Nothing non-root can wake a suspended Deck on a
   timer, so this is a real precondition, not something to work around. To grant just this:

     sudo steamos-readonly disable
     echo 'deck ALL=(root) NOPASSWD: /usr/bin/rtcwake' | sudo tee /etc/sudoers.d/zz-deckback-soak
     sudo chmod 0440 /etc/sudoers.d/zz-deckback-soak
     sudo steamos-readonly enable

   The 'zz-' prefix is load-bearing: SteamOS ships 'deck ALL=(ALL) ALL' (needs a password) in a
   sudoers.d file, and sudo takes the LAST matching rule, so our NOPASSWD only wins if our file
   sorts after it. A file named just 'deckback-soak' is silently overridden. The path must match
   what sudo resolves for the bare 'rtcwake' the loop calls — /usr/bin/rtcwake on holo
   ('command -v rtcwake' to confirm on your unit)."
fi

# Preconditions: the app is running and a video is playing, or every cycle fails for an unrelated reason.
deck_ssh "$DECK_HOST" 'pgrep -f deckback-launcher >/dev/null' ||
  die_env "deckback-launcher is not running on ${DECK_HOST} — start it (just run / just remote-run) first"

info "Checking a video is playing before we start suspending ..."
deckctl playing --window 4 || exit $?

epp_before="$(deckctl epp)" || die_env "cannot read EPP on ${DECK_HOST}"

info "Running ${n} suspend/resume cycles on ${DECK_HOST} ..."
epp_changed_cycles=0
for i in $(seq 1 "$n"); do
  t_before="$(deckctl current-time)" || exit $?
  info "cycle $i/$n: currentTime=${t_before}s, rtcwake -m mem -s 45"

  # An rtcwake that cannot run at all is an environment problem, not a resume defect.
  if ! deck_ssh "$DECK_HOST" 'sudo -n rtcwake -m mem -s 45' 2>/dev/null; then
    die_env "cycle $i: rtcwake failed to run (passwordless 'sudo rtcwake' on the Deck?)"
  fi
  sleep 5
  if ! deck_ssh -o ConnectTimeout=15 "$DECK_HOST" true 2>/dev/null; then
    die_transport "cycle $i: Deck did not come back on the network after resume"
  fi
  if ! deck_ssh "$DECK_HOST" 'pgrep -f deckback-launcher >/dev/null'; then
    die_assert "cycle $i: launcher not alive after resume"
  fi

  # `playing` samples currentTime twice: paused = ENV, stalled or rewound = ASSERT. The sleep lets
  # the player re-acquire the decoder first.
  sleep 3
  # `$?` after `if ! cmd` is the negated status, not deckctl's: capture it before branching.
  set +e
  deckctl playing --window 4 --min-fraction 0.4
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    echo "error: cycle $i: playback did not survive the resume" >&2
    echo "       (currentTime was ${t_before}s before suspend; see the reason above)" >&2
    exit "$rc"
  fi

  epp_now="$(deckctl epp 2>/dev/null || true)"
  if [ -n "$epp_now" ] && [ "$epp_now" != "$epp_before" ]; then
    epp_changed_cycles=$((epp_changed_cycles + 1))
  fi
  info "cycle $i: alive, playing, position advanced"
done

info "Soak: ${n} cycles. The app stayed alive and the video kept playing across every resume."
if [ "$epp_changed_cycles" -gt 0 ]; then
  info "EPP: changed after resume on ${epp_changed_cycles}/${n} cycles (steamos#2383)."
  info "     Root-only sysfs — we can measure it and cannot fix it from the Flatpak. Not a gate."
  deckctl epp | sed 's/^/     cpu/' || true
else
  info "EPP: unchanged across every resume on this SteamOS build."
fi
info "NOT CHECKED (P6 gate, remaining clauses): audio actually restored, screen did not dim."

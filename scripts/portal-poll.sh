#!/usr/bin/env bash
# Override the Deck's Flatpak update-portal poll interval, so a self-update round-trip that normally
# waits ~30 min for the portal's periodic check fires in seconds instead. A debugging aid for the
# notify/auto self-update path (findings/durable/self-update.md). Fully reversible.
#
#   scripts/portal-poll.sh set [seconds]   install a user drop-in --poll-timeout=<seconds> (default 60)
#   scripts/portal-poll.sh restore         remove the drop-in, restore the built-in ~30-min default
#   scripts/portal-poll.sh status          report the effective poll interval + whose drop-in it is
#
# GOTCHA (self-update.md): restarting the portal ORPHANS an already-created UpdateMonitor — the app
# subscribes once at launch and never recreates it, and the orphaned monitor goes silent (no error).
# So run `set` FIRST, THEN (re)launch Deckback, or no detection fires that session.
#
# Runs from the workstation and drives the Deck over deck_ssh. Changes ONLY the user's flatpak-portal
# service — no root, no password, touches nothing else the user installed. Exit codes (harness §1):
# 0 ok · 3 environment (override did not take) · 4 transport (Deck/SSH) · 5 usage.
. "$(dirname "$0")/lib.sh"
# shellcheck source=scripts/lib/portal_poll.sh
. "$(dirname "$0")/lib/portal_poll.sh"

svc="$DECKBACK_PORTAL_SERVICE"
rel_dir="$DECKBACK_PORTAL_DROPIN_DIR"
rel_path="$rel_dir/$DECKBACK_PORTAL_DROPIN_NAME"

# `systemctl --user show -p ExecStart` for the effective (post-drop-in) command line.
portal_execstart() {
  deck_ssh "$DECK_HOST" "systemctl --user show $svc -p ExecStart --value 2>/dev/null" || true
}

# The vendor ExecStart, read from the unit fragment so an existing drop-in of ours can never feed its
# own --poll-timeout back into a fresh one. Falls back to the well-known binary path if unreadable.
portal_base_cmd() {
  local frag base=""
  frag="$(deck_ssh "$DECK_HOST" "systemctl --user show $svc -p FragmentPath --value 2>/dev/null" || true)"
  if [ -n "$frag" ]; then
    base="$(deck_ssh "$DECK_HOST" "grep -m1 '^ExecStart=' '$frag' 2>/dev/null | sed 's/^ExecStart=//'" || true)"
  fi
  if [ -n "$base" ]; then echo "$base"; else echo "$DECKBACK_PORTAL_DEFAULT_BIN"; fi
}

restart_portal() {
  deck_ssh "$DECK_HOST" "systemctl --user daemon-reload && systemctl --user restart $svc" ||
    die_transport "could not reload/restart $svc on $DECK_HOST"
}

our_dropin_present() { deck_ssh "$DECK_HOST" "test -f \"\$HOME/$rel_path\""; }

show_status() {
  local es cur
  es="$(portal_execstart)"
  cur="$(poll_seconds_from_execstart "$es")"
  if our_dropin_present; then
    if [ "$cur" = default ]; then
      info "portal: our drop-in is present but no --poll-timeout is active yet (restart pending?)"
    else
      info "portal: fast-poll ACTIVE — --poll-timeout=${cur}s (Deckback drop-in ~/$rel_path)"
    fi
  elif [ "$cur" = default ]; then
    info "portal: DEFAULT poll ($DECKBACK_PORTAL_DEFAULT_POLL_HINT); no Deckback drop-in installed"
  else
    info "portal: --poll-timeout=${cur}s is set by something other than Deckback (no drop-in of ours)"
  fi
  info "portal ExecStart: ${es:-<unreadable>}"
}

do_set() {
  local seconds="${1:-60}"
  valid_poll_seconds "$seconds" ||
    die_usage "seconds must be a positive integer in [1,86400], got '$seconds'"

  local base content
  base="$(portal_base_cmd)"
  content="$(portal_dropin_content "$base" "$seconds")"

  if ! deck_ssh "$DECK_HOST" "mkdir -p \"\$HOME/$rel_dir\" && cat > \"\$HOME/$rel_path\"" <<EOF
$content
EOF
  then
    die_transport "could not write the drop-in on $DECK_HOST"
  fi
  restart_portal

  local cur
  cur="$(poll_seconds_from_execstart "$(portal_execstart)")"
  [ "$cur" = "$seconds" ] ||
    die_env "wrote --poll-timeout=${seconds} but the portal reports '${cur}' — the override did not take"
  info "portal fast-poll set: --poll-timeout=${seconds}s on $DECK_HOST"
  info "NOW (re)launch Deckback — a portal restart orphans a running app's update monitor (self-update.md)."
  info "Undo with: just portal-poll restore"
}

do_restore() {
  deck_ssh "$DECK_HOST" "rm -f \"\$HOME/$rel_path\"; rmdir \"\$HOME/$rel_dir\" 2>/dev/null || true" ||
    die_transport "could not remove the drop-in on $DECK_HOST"
  restart_portal

  local cur
  cur="$(poll_seconds_from_execstart "$(portal_execstart)")"
  [ "$cur" = default ] ||
    info "note: portal still reports --poll-timeout=${cur} — set outside Deckback, left as-is"
  info "portal restored to default ($DECKBACK_PORTAL_DEFAULT_POLL_HINT) on $DECK_HOST"
}

main() {
  local cmd="${1:-status}"
  [ "$#" -gt 0 ] && shift
  require_deck
  case "$cmd" in
  set) do_set "$@" ;;
  restore) do_restore ;;
  status) show_status ;;
  -h | --help | help) sed -n '2,18p' "$0" ;;
  *) die_usage "unknown command '$cmd' (use: set [seconds] | restore | status)" ;;
  esac
}

main "$@"

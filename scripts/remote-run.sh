#!/usr/bin/env bash
# Launch the installed Flatpak through Steam's non-Steam shortcut URI over SSH (headless Game Mode).
#
# Exit codes: 0 the app is up and its CDP endpoint answers · 2 the launch did not produce a running
#             app · 3 no Steam shortcut / no Steam session · 4 Deck unreachable.
#
# `steam://rungameid/...` is fire-and-forget: Steam returns 0 whether or not it launched anything. So
# this script used to print "launch request sent" and exit 0 with the app dead — a stale shortcut id,
# a dead D-Bus session or the wrong DISPLAY all looked like success. We now WAIT for the process and
# for the DevTools endpoint, because every automated on-Deck test starts by trusting this exit code.
. "$(dirname "$0")/lib.sh"

require_deck
display="${DECK_DISPLAY:-:0}"
app="io.github.properrr.deckback"
cdp_port="${DECKBACK_CDP_PORT:-9222}"
boot_timeout="${DECKBACK_BOOT_TIMEOUT:-40}"

# The Deck's login uid: hardcoding 1000 breaks on any Deck whose primary user is not `deck`.
deck_uid="$(deck_ssh "$DECK_HOST" 'id -u')" || die_transport "cannot read the Deck's uid"
steam_bin="$(deck_ssh "$DECK_HOST" 'ls "$HOME/.local/share/Steam/ubuntu12_32/steam" 2>/dev/null || true')"
[ -n "$steam_bin" ] || die_env "Steam client binary not found on ${DECK_HOST} (is Steam installed?)"

# Steam stores a 32-bit shortcut id in shortcuts.vdf, but rungameid needs the full URI id:
# (shortcut_id << 32) | 0x02000000. Pick the newest Deckback entry after an app update.
shortcut_id="$(deck_ssh "$DECK_HOST" "python3 -c 'import glob,re; p=glob.glob(\"\$HOME/.local/share/Steam/userdata/*/config/shortcuts.vdf\")[0]; b=open(p,\"rb\").read(); m=list(re.finditer(b\"\\x02appid\\x00(.{4})\\x01appname\\x00Deckback\\x00\",b)); print(int.from_bytes(m[-1].group(1),\"little\") if m else \"\")'" 2>/dev/null || true)"
[ -n "$shortcut_id" ] || die_env "Deckback Steam shortcut not found on ${DECK_HOST} (run 'just install' on the Deck first)"
steam_uri_id="$(python3 -c "print(($shortcut_id << 32) | 0x02000000)")"

info "Stopping any existing Deckback instance on ${DECK_HOST} ..."
deck_ssh "$DECK_HOST" "flatpak kill ${app}" 2>/dev/null || true

info "Launching Deckback through Steam in Game Mode (shortcut=${shortcut_id}) ..."
deck_ssh "$DECK_HOST" env DISPLAY="$display" "XDG_RUNTIME_DIR=/run/user/${deck_uid}" \
  "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/${deck_uid}/bus" "$steam_bin" \
  "steam://rungameid/${steam_uri_id}" >/dev/null 2>&1 || true # Steam's own exit code is meaningless here

# Verification, in two stages. The process appearing proves Steam honored the URI; the CDP endpoint
# answering proves the engine actually came up rather than crashing on startup.
info "Waiting up to ${boot_timeout}s for the launcher process ..."
for _ in $(seq 1 "$boot_timeout"); do
  if deck_ssh "$DECK_HOST" 'pgrep -f deckback-launcher >/dev/null 2>&1'; then
    proc_up=1
    break
  fi
  sleep 1
done
[ "${proc_up:-0}" = 1 ] || die_assert \
  "Steam accepted steam://rungameid/${steam_uri_id} but no deckback-launcher process appeared within ${boot_timeout}s.
   Check: is Game Mode running on DISPLAY=${display}? Is the shortcut id stale (re-run 'just install')?"

info "Waiting for the DevTools endpoint on :${cdp_port} ..."
for _ in $(seq 1 "$boot_timeout"); do
  # Chrome >= 111 requires Host: localhost on /json — query it from the Deck itself, never over the LAN.
  if deck_ssh "$DECK_HOST" "curl -sf -m 2 http://localhost:${cdp_port}/json/version >/dev/null 2>&1"; then
    cdp_up=1
    break
  fi
  sleep 1
done
[ "${cdp_up:-0}" = 1 ] || die_assert \
  "deckback-launcher is running but its DevTools endpoint never answered on :${cdp_port} within ${boot_timeout}s.
   The engine likely failed to start (check 'just logs')."

info "Deckback is up on ${DECK_HOST} (pid alive, CDP :${cdp_port} answering)."

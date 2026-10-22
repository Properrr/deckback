#!/usr/bin/env bash
# Launch the rsynced dev build (~/cobalt-yt) on the Deck over SSH and claim the Game Mode panel.
# Env: DECK_DISPLAY (gamescope Xwayland to render on, default :1) · DECKBACK_DEV_APPID ·
# STEAM_CTRL_DISPLAY. --no-sandbox goes via DECKBACK_EXTRA_ARGS so it never lands in config/app.json.
. "$(dirname "$0")/lib.sh"
require_deck
DECK_DISPLAY="${DECK_DISPLAY:-:1}"

# gamescope presents only the window steamcompmgr focuses: one whose STEAM_GAME appid is listed in
# GAMESCOPECTRL_BASELAYER_APPID. A process started over SSH has neither, so tag the window and PREPEND
# our appid; Steam's appids stay listed as the fallback when the app exits. The list is rebuilt as
# `ours + previous-minus-ours` so repeated runs do not grow it.
DECKBACK_DEV_APPID="${DECKBACK_DEV_APPID:-1234567890}"
# Steam's control atoms live on gamescope's first Xwayland regardless of where the app renders.
STEAM_CTRL_DISPLAY="${STEAM_CTRL_DISPLAY:-:0}"
FOCUS_TIMEOUT_S=30
# WM_CLASS, not the title (about:blank at boot); content_shell's helper windows carry no class.
WINDOW_CLASS=chromium-content_shell

info "Launching Deckback on ${DECK_HOST} (DISPLAY=${DECK_DISPLAY}; Ctrl-C to detach; app keeps running) ..."

remote=$(
  cat <<EOF
set -u
cd ~/cobalt-yt || exit 3

# Claim the panel once the window maps. Backgrounded so it cannot delay or fail the launch: if xprop
# is missing or gamescope is not running (Desktop Mode), this warns and the app still runs.
(
  if ! command -v xprop >/dev/null 2>&1 || ! command -v xwininfo >/dev/null 2>&1; then
    echo "focus: xprop/xwininfo not found — not claiming the screen (Game Mode will keep showing Steam)" >&2
    exit 0
  fi
  for _ in \$(seq 1 ${FOCUS_TIMEOUT_S}); do
    win=\$(DISPLAY=${DECK_DISPLAY} xwininfo -root -children 2>/dev/null | awk '/${WINDOW_CLASS}/ {print \$1; exit}')
    if [ -n "\$win" ]; then
      DISPLAY=${DECK_DISPLAY} xprop -id "\$win" -f STEAM_GAME 32c -set STEAM_GAME ${DECKBACK_DEV_APPID} 2>/dev/null || exit 0
      prev_line=\$(DISPLAY=${STEAM_CTRL_DISPLAY} xprop -root GAMESCOPECTRL_BASELAYER_APPID 2>/dev/null || true)
      case "\$prev_line" in *"= "*) prev=\${prev_line#*= } ;; *) prev="" ;; esac
      rest=\$(printf '%s' "\$prev" | tr -d ' ' | tr ',' '\n' | grep -vx "${DECKBACK_DEV_APPID}" | grep -v '^\$' | paste -sd, -)
      DISPLAY=${STEAM_CTRL_DISPLAY} xprop -root -f GAMESCOPECTRL_BASELAYER_APPID 32c \
        -set GAMESCOPECTRL_BASELAYER_APPID "${DECKBACK_DEV_APPID}\${rest:+, \$rest}" 2>/dev/null || exit 0
      echo "focus: claimed the panel for \$win (appid ${DECKBACK_DEV_APPID}); Steam stays as fallback" >&2
      exit 0
    fi
    sleep 1
  done
  echo "focus: no ${WINDOW_CLASS} window within ${FOCUS_TIMEOUT_S}s — screen not claimed" >&2
) &

XDG_RUNTIME_DIR=/run/user/\$(id -u) DISPLAY=${DECK_DISPLAY} \
DECKBACK_COBALT_BIN=./content_shell \
DECKBACK_EXTRA_ARGS='--no-sandbox' \
./deckback-launcher --config ./config/app.json 2>&1 | tee -a ~/cobalt-yt/deckback.log
EOF
)

deck_ssh -t "$DECK_HOST" "$remote"

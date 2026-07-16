#!/usr/bin/env bash
. "$(dirname "$0")/lib.sh"
require_deck
# Game Mode render target: gamescope exposes Xwayland :0/:1 under XDG_RUNTIME_DIR (findings m114).
# --no-sandbox is bring-up only (no zypak until Phase 8) — passed via DECKBACK_EXTRA_ARGS so it never
# gets baked into the shipped config/app.json. The launcher boots content_shell on about:blank and
# injects the TV UA + navigates over CDP (content_shell ignores --user-agent).
DECK_DISPLAY="${DECK_DISPLAY:-:1}"

# ---- why this script claims focus, and why it used to show nothing --------------------------------
#
# Measured 2026-07-16 on the OLED unit, Game Mode: this script launched the app CORRECTLY — window
# `0x800004 "YouTube on TV" ("chromium-content_shell") 1280x800+0+0` present on :1, Leanback loaded,
# CDP up — and the screen showed Steam anyway. Nothing was broken and nothing was visible.
#
# gamescope runs with `--xwayland-count 2` (:0 and :1) and presents ONE window: the one Steam's
# steamcompmgr focuses, published as GAMESCOPE_FOCUSED_WINDOW on :0's root. Focus is not "newest
# window" or "the only window on this display" — it is whatever carries a STEAM_GAME appid that also
# appears in GAMESCOPECTRL_BASELAYER_APPID on the control display. A process started over SSH carries
# neither, so steamcompmgr never considers it and Big Picture keeps the panel. Observed before:
#
#     GAMESCOPE_FOCUSED_WINDOW = 37748789        (0x2400035 "Steam Big Picture Mode", on :0)
#     GAMESCOPECTRL_BASELAYER_APPID = 413091, 769
#
# `just remote-run` avoids all of this by going through `steam://rungameid/...` and letting Steam set
# the atoms — but it can only launch the INSTALLED Flatpak, which is the wrong binary for a dev loop
# that just rsynced a build to ~/cobalt-yt. So we set the same two atoms Steam would.
#
# Verified 2026-07-16: tagging the window and prepending the appid moved GAMESCOPE_FOCUSED_WINDOW to
# 8388612 (= 0x800004, our window) within ~2s, and the app appeared on the panel.
#
# WHY STEAM'S APPIDS STAY IN THE LIST. On exit gamescope falls back to the next appid that still has a
# live window — measured: the launcher died and focus returned to 37748789 (Steam) on its own. So we
# PREPEND rather than replace, and never "restore" on detach (that would blank a still-running app,
# and this script is documented to keep the app alive after Ctrl-C). A stale appid left in the list is
# inert: nothing claims it, so gamescope skips it.
#
# The list is rebuilt as `ours + previous-minus-ours` so repeated runs cannot grow it without bound.
DECKBACK_DEV_APPID="${DECKBACK_DEV_APPID:-1234567890}"
# Steam's control atoms live on gamescope's FIRST Xwayland regardless of where the app renders, so
# this is not DECK_DISPLAY: we read/write the baselayer on :0 while tagging a window on :1.
STEAM_CTRL_DISPLAY="${STEAM_CTRL_DISPLAY:-:0}"
FOCUS_TIMEOUT_S=30
# WM_CLASS, not the window title: at boot the page is about:blank and the title is not yet
# "YouTube on TV". The class is stable from map time, and content_shell's other windows (a 1x1 helper,
# a 10x10 "Chromium clipboard") carry no class at all, so this matches exactly one window.
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

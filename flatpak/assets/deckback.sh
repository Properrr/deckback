#!/bin/sh
# Deckback entrypoint inside the Flatpak. The launcher owns the watchdog, CDP UA-injection/navigation
# and Phase-6 power hooks; it forks the engine named by DECKBACK_COBALT_BIN (=/app/bin/cobalt-zypak,
# set via the manifest), which zypak-wraps content_shell. So content_shell is sandboxed by zypak while
# the tiny launcher stays outside it. Extra args are forwarded (e.g. an app:// deep-link from Steam).
exec /app/bin/deckback-launcher --config /app/share/deckback/app.json "$@"

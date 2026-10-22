#!/bin/sh
# Flatpak entrypoint. The launcher stays outside zypak and forks the engine named by
# DECKBACK_COBALT_BIN (/app/bin/cobalt-zypak). Extra args (e.g. an app:// deep link) are forwarded.
exec /app/bin/deckback-launcher --config /app/share/deckback/app.json "$@"

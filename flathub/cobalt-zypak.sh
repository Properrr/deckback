#!/bin/sh
# Flathub variant of the engine shim. Identical to flatpak/assets/cobalt-zypak.sh EXCEPT the engine
# lives at /app/extra/ here (unpacked from extra-data by apply_extra at install time) instead of
# /app/cobalt/ (baked in by the local-bundle build). zypak-wrapper must wrap the Chromium binary
# DIRECTLY so its zygote/sandbox broker intercepts correctly.
exec zypak-wrapper.sh /app/extra/content_shell "$@"

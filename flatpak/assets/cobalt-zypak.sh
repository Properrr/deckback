#!/bin/sh
# Engine shim the launcher forks: zypak-wrapper must wrap the Chromium binary directly for its
# zygote/sandbox broker to work. All launcher flags pass through.
exec zypak-wrapper.sh /app/cobalt/content_shell "$@"

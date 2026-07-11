#!/bin/sh
# Engine shim: zypak-wrapper must wrap the Chromium binary DIRECTLY for its zygote/sandbox broker to
# intercept correctly, so the launcher forks this (not the launcher itself under zypak). All flags the
# launcher composes (about:blank, --data-path, --remote-debugging-port, cobalt_flags) pass through.
exec zypak-wrapper.sh /app/cobalt/content_shell "$@"

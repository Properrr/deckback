#!/usr/bin/env bash
# In-container half of the test sim (invoked by scripts/sim/run.sh; do not run directly on the host).
# Source is read-only at /src; everything writable goes under /tmp.
set -uo pipefail
suite="${1:-all}"
SRC=/src
fail=0

note() { echo "== $* =="; }

# The sim's whole value is that it is the DEVICE's userspace. Trusting the image tag for that would be
# the same unchecked assumption the base swap exists to remove, so prove it inside the container.
if [ "${DECKBACK_SIM_BASE:-}" != "steamos" ]; then
  echo "sim: image is not the SteamOS base (DECKBACK_SIM_BASE='${DECKBACK_SIM_BASE:-unset}')." >&2
  echo "     Refusing: a green here would claim SteamOS fidelity the image does not have." >&2
  exit 3
fi
ok() { echo "  ok  - $1"; }
bad() {
  echo "  FAIL - $1"
  fail=$((fail + 1))
}

# The launcher is the out-of-tree C++23 shim; building it on the SteamOS userspace (not the repo's
# Debian toolchain) proves it isn't distro-locked AND that it compiles against the versions the Deck
# actually carries. Out-of-source build so /src can stay read-only.
#
# The image is a real SteamOS 3.8.1x rootfs pinned to the device's own gcc/glibc, not archlinux:latest
# -- which drifts ahead of SteamOS and had this suite compiling with GCC 16 against glibc 2.43 while
# the Deck runs GCC 15.1.1 / glibc 2.41 (durable/test-sim.md ★ CORRECTION).
run_launcher() {
  note "suite: launcher — build the out-of-tree launcher on the SteamOS userspace + run its L0 tests"
  if ! cmake -S "$SRC/launcher" -B /tmp/lbuild -G Ninja -DCMAKE_BUILD_TYPE=Release >/tmp/cmake.log 2>&1; then
    tail -20 /tmp/cmake.log
    bad "cmake configure"
    return
  fi
  if ! cmake --build /tmp/lbuild >/tmp/build.log 2>&1; then
    tail -30 /tmp/build.log
    bad "launcher build"
    return
  fi
  if ctest --test-dir /tmp/lbuild --output-on-failure >/tmp/ctest.log 2>&1; then
    grep -E "tests passed" /tmp/ctest.log | tail -1
    ok "launcher builds + L0 green on SteamOS ${DECKBACK_SIM_BRANCH:-?} (gcc $(gcc -dumpfullversion), glibc $(ldd --version | head -1 | awk "{print \$NF}"))"
  else
    tail -20 /tmp/ctest.log
    bad "launcher ctest"
  fi
}

# The installer's Steam-tile writing: exactly what the one-line installer does on a new Deck, minus the
# GPU. Faithful — shortcuts.vdf + the grid/ art dir are plain files Steam reads on next start.
run_shortcut() {
  note "suite: shortcut — installer Steam tile + artwork (steam_shortcuts.py add/art)"
  local uid=1000000001
  local cfg="/tmp/steamhome/.local/share/Steam/userdata/$uid/config"
  mkdir -p "$cfg"
  if ! python3 "$SRC/scripts/steam_shortcuts.py" add --vdf "$cfg/shortcuts.vdf" --force >/tmp/add.log 2>&1; then
    cat /tmp/add.log
    bad "add shortcut"
    return
  fi
  if ! python3 "$SRC/scripts/steam_shortcuts.py" art --vdf "$cfg/shortcuts.vdf" --appname Deckback \
    --assets "$SRC/flatpak/assets/steam" >/tmp/art.log 2>&1; then
    cat /tmp/art.log
    bad "install art"
    return
  fi
  if python3 - "$cfg" <<'PY'; then
import sys, os, importlib.util
cfg = sys.argv[1]
spec = importlib.util.spec_from_file_location("ss", "/src/scripts/steam_shortcuts.py")
ss = importlib.util.module_from_spec(spec); spec.loader.exec_module(ss)
_root, items = ss.load_shortcuts(os.path.join(cfg, "shortcuts.vdf"))
ms = [e for (_t, _k, e) in items if ss._matches(e, "Deckback")]
assert len(ms) == 1, f"expected 1 Deckback entry, got {len(ms)}"
e = ms[0]
gid = ss.art_id(e, ss._entry_field(e, "Exe") or "", ss._entry_field(e, "AppName") or "")
grid = os.path.join(cfg, "grid")
missing = [suf for suf in ("", "p", "_hero", "_logo", "_icon")
           if not os.path.isfile(os.path.join(grid, f"{gid}{suf}.png"))]
assert not missing, f"missing art suffixes {missing} for appid {gid}"
print(f"  Steam tile + 5 art files written for appid {gid} — installer shortcut logic proven")
PY
    ok "installer shortcut + artwork land correctly (no Deck, no GPU)"
  else
    bad "assert shortcut+art layout"
  fi
}

# The Flatpak update portal is what the self-updater + D-Bus reconnect talk to. Proving it activates
# and enforces its caller contract here is the foundation the reconnect suite (Phase 2) builds on.
run_portal() {
  note "suite: portal — Flatpak update portal answers in-container (self-update/reconnect foundation)"
  local out
  out="$(dbus-run-session -- gdbus call --session --dest org.freedesktop.portal.Flatpak \
    --object-path /org/freedesktop/portal/Flatpak \
    --method org.freedesktop.portal.Flatpak.CreateUpdateMonitor '{}' 2>&1 || true)"
  if printf '%s' "$out" | grep -q "Updates only supported by flatpak apps"; then
    ok "flatpak-portal + PermissionStore activate + enforce their contract"
  else
    printf '%s\n' "$out"
    bad "portal did not activate/answer as expected"
  fi
}

# Wait up to $3 seconds for the literal substring $2 to appear in file $1. 0 if seen, 1 on timeout.
wait_log() {
  local f="$1" pat="$2" secs="$3" i
  for ((i = 0; i < secs * 5; i++)); do
    grep -qF -- "$pat" "$f" 2>/dev/null && return 0
    sleep 0.2
  done
  return 1
}

# Start a session dbus-daemon at a FIXED socket path (so the reconnect drive can drop and restore the
# bus at the same address, which is what case A needs). Sets DBUS_PID. 0 once the socket is listening.
start_bus() {
  dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" --nofork --nopidfile >/tmp/dbus.log 2>&1 &
  DBUS_PID=$!
  local i
  for i in $(seq 1 50); do
    [ -S "${XDG_RUNTIME_DIR}/bus" ] && return 0
    sleep 0.1
  done
  return 1
}

# Run the real updater loop as a flatpak instance (bwrap with a synthetic /.flatpak-info, so the portal
# treats us as a flatpak app and CreateUpdateMonitor succeeds) for up to $1 seconds, logging to /tmp/
# watch.log. Sets WATCH_PID. No --unshare-pid: the portal reads /proc/<pid>/root/.flatpak-info, so our
# pid must stay visible in the container's pid namespace.
start_watch() {
  : >/tmp/watch.log
  bwrap --unshare-user --uid 0 --gid 0 \
    --ro-bind /usr /usr --ro-bind /etc /etc \
    --symlink usr/lib /lib --symlink usr/lib /lib64 --symlink usr/bin /bin --symlink usr/bin /sbin \
    --proc /proc --dev /dev --tmpfs /tmp \
    --ro-bind /tmp/lbuild /opt/lbuild \
    --ro-bind /tmp/fpi/flatpak-info /.flatpak-info \
    --bind "$XDG_RUNTIME_DIR" "$XDG_RUNTIME_DIR" \
    --setenv XDG_RUNTIME_DIR "$XDG_RUNTIME_DIR" \
    --setenv DBUS_SESSION_BUS_ADDRESS "$DBUS_SESSION_BUS_ADDRESS" \
    --setenv HOME /tmp \
    -- /opt/lbuild/deckback-launcher --selftest-watch "$1" >/tmp/watch.log 2>&1 &
  WATCH_PID=$!
}

cleanup_reconnect() {
  [ -n "${WATCH_PID:-}" ] && kill "$WATCH_PID" 2>/dev/null
  [ -n "${DBUS_PID:-}" ] && kill "$DBUS_PID" 2>/dev/null
  pkill -f flatpak-portal 2>/dev/null || true
}

# The heart of Phase 2: drive the launcher's D-Bus reconnect logic (durable/dbus-reconnect.md) off
# hardware. Case B = flatpak-portal restarts (our bus survives, the orphaned monitor is rebuilt via
# NameOwnerChanged); case A = the session bus itself drops and returns. This is the real PortalUpdater
# loop, not a stub — it retires the on-Deck reconnect drive that a sleeping Deck kept blocking.
run_reconnect() {
  note "suite: reconnect — drive the updater's D-Bus reconnect (case A bus drop, case B portal restart)"
  if [ ! -x /tmp/lbuild/deckback-launcher ]; then
    if ! cmake -S "$SRC/launcher" -B /tmp/lbuild -G Ninja -DCMAKE_BUILD_TYPE=Release >/tmp/cmake.log 2>&1 ||
      ! cmake --build /tmp/lbuild >/tmp/build.log 2>&1; then
      tail -20 /tmp/build.log 2>/dev/null
      bad "launcher build (reconnect prerequisite)"
      return
    fi
  fi
  # A libsystemd-less build compiles the StubUpdater, whose watch does nothing — that would fake a pass.
  if /tmp/lbuild/deckback-launcher --selftest-watch 1 2>&1 | grep -q "no libsystemd backend compiled in"; then
    bad "launcher built WITHOUT sd-bus — the reconnect path is not compiled (need systemd-libs headers)"
    return
  fi

  export XDG_RUNTIME_DIR=/tmp/xrd
  mkdir -p "$XDG_RUNTIME_DIR"
  chmod 700 "$XDG_RUNTIME_DIR"
  export DBUS_SESSION_BUS_ADDRESS="unix:path=${XDG_RUNTIME_DIR}/bus"
  if ! start_bus; then
    bad "session bus did not come up"
    return
  fi

  mkdir -p /tmp/fpi
  printf '%s\n' '[Application]' 'name=io.github.properrr.deckback' '' \
    '[Instance]' 'instance-id=deckback-sim' 'flatpak-version=1.15.0' >/tmp/fpi/flatpak-info

  start_watch 60

  # Positive control: without an accepted flatpak-instance caller the monitor never comes up and the
  # whole drive proves nothing (a NON-flatpak caller is what the `portal` suite already rejects).
  if wait_log /tmp/watch.log "watching for updates to this app" 15; then
    ok "positive control: portal accepted the flatpak-instance caller, monitor up"
  else
    sed 's/^/    /' /tmp/watch.log
    bad "monitor never came up (bwrap/flatpak-info positive control)"
    cleanup_reconnect
    return
  fi

  # ---- Case B: flatpak-portal restarts. The session bus survives; the orphaned monitor is rebuilt. --
  pkill -f flatpak-portal 2>/dev/null
  sleep 0.5
  # Reactivate the portal from ANY caller so NameOwnerChanged(new owner) broadcasts to our watcher.
  gdbus call --session --dest org.freedesktop.portal.Flatpak \
    --object-path /org/freedesktop/portal/Flatpak \
    --method org.freedesktop.portal.Flatpak.CreateUpdateMonitor '{}' >/dev/null 2>&1 || true
  if wait_log /tmp/watch.log "flatpak-portal restarted" 15; then
    ok "case B: portal restart detected — monitor re-created (NameOwnerChanged path)"
  else
    sed 's/^/    /' /tmp/watch.log
    bad "case B: portal restart did not trigger a monitor rebuild"
  fi

  # ---- Case A: the session bus itself drops, then returns at the SAME address. --------------------
  kill "$DBUS_PID" 2>/dev/null
  wait "$DBUS_PID" 2>/dev/null
  if wait_log /tmp/watch.log "session bus error" 15; then
    ok "case A: bus drop detected (attempting to reconnect)"
  else
    sed 's/^/    /' /tmp/watch.log
    bad "case A: bus drop was not detected"
  fi
  if ! start_bus; then
    bad "case A: could not restart the session bus"
    cleanup_reconnect
    return
  fi
  if wait_log /tmp/watch.log "reconnected to the Flatpak portal" 30; then
    ok "case A: reconnected after the bus returned — watching again"
  else
    sed 's/^/    /' /tmp/watch.log
    bad "case A: never reconnected after the bus returned"
  fi

  cleanup_reconnect
}

case "$suite" in
launcher) run_launcher ;;
shortcut) run_shortcut ;;
portal) run_portal ;;
reconnect) run_reconnect ;;
all)
  run_launcher
  run_shortcut
  run_portal
  run_reconnect
  ;;
*)
  echo "unknown suite '$suite'" >&2
  exit 5
  ;;
esac

echo
echo "sim: COVERAGE — GPU-INDEPENDENT layers only. NOT covered here (Deck-only, never green in the sim):"
echo "     VA-API decode · green-band pixel verdict · power (<=9W) · suspend/resume."
if [ "$fail" -eq 0 ]; then
  echo "sim: suite '$suite' OK"
  exit 0
fi
echo "sim: suite '$suite' FAILED ($fail assertion(s))"
exit 2

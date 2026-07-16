#!/usr/bin/env bash
# In-container half of the test sim (invoked by scripts/sim/run.sh; do not run directly on the host).
# Source is read-only at /src; everything writable goes under /tmp.
set -uo pipefail
suite="${1:-all}"
SRC=/src
fail=0

note() { echo "== $* =="; }
ok() { echo "  ok  - $1"; }
bad() {
  echo "  FAIL - $1"
  fail=$((fail + 1))
}

# The launcher is the out-of-tree C++23 shim; building it on Arch (not the repo's Debian toolchain)
# also proves it isn't accidentally distro-locked. Out-of-source build so /src can stay read-only.
run_launcher() {
  note "suite: launcher — build the out-of-tree launcher on Arch + run its L0 tests"
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
    ok "launcher builds + L0 green on Arch"
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
  if python3 - "$cfg" <<'PY'
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
  then
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

case "$suite" in
launcher) run_launcher ;;
shortcut) run_shortcut ;;
portal) run_portal ;;
all)
  run_launcher
  run_shortcut
  run_portal
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

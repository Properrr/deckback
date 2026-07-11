#!/usr/bin/env python3
"""Manage the Deckback non-Steam shortcut + library artwork on a Steam Deck.

Runs ON the Deck (no third-party deps — Steam ships no `vdf` module, so we parse the binary
shortcuts.vdf ourselves). Two jobs:

  * `remove`      — delete every shortcut whose AppName matches from shortcuts.vdf (with a backup),
                    and delete its grid artwork.
  * `art`         — find the matching shortcut, derive its grid appid, and install the capsule /
                    hero / logo / header / icon PNGs into userdata/<id>/config/grid/.

Steam caches shortcuts.vdf in memory and rewrites it on exit, so an edit to the file only sticks
if Steam is NOT running. `remove` refuses to run while Steam is up. `art` only *adds* files to the
grid dir (Steam never rewrites those), so it is safe any time and simply appears on the next Steam
restart.

The grid-image appid convention is the widely-used one (SteamGridDB / boilr / steamgrid):
    legacy_id = crc32( (exe + appname).encode('utf-8') ) | 0x80000000
    grid/<legacy_id>.png        landscape / header capsule
    grid/<legacy_id>p.png       portrait library capsule
    grid/<legacy_id>_hero.png   hero background
    grid/<legacy_id>_logo.png   transparent logo overlay
    grid/<legacy_id>_icon.png   icon
"""
import argparse
import glob
import os
import shutil
import sys
import zlib

# ---------------------------------------------------------------------------------------------
# Minimal binary-VDF (Steam KeyValues) reader/writer for shortcuts.vdf.
# Node tags: 0x00 nested map (key, children..., 0x08) · 0x01 string (key, cstr) · 0x02 int32-le
# (key, 4 bytes) · 0x08 end-of-map. An entry value is an ordered list of (tag, key, value).
# ---------------------------------------------------------------------------------------------

def _read_cstr(buf, pos):
    end = buf.index(b"\x00", pos)
    return buf[pos:end].decode("utf-8", "replace"), end + 1


def _read_map(buf, pos):
    items = []
    while True:
        tag = buf[pos]; pos += 1
        if tag == 0x08:
            return items, pos
        key, pos = _read_cstr(buf, pos)
        if tag == 0x00:
            val, pos = _read_map(buf, pos)
        elif tag == 0x01:
            val, pos = _read_cstr(buf, pos)
        elif tag == 0x02:
            val = int.from_bytes(buf[pos:pos + 4], "little", signed=False); pos += 4
        else:
            raise ValueError(f"unknown VDF tag 0x{tag:02x} at {pos - 1}")
        items.append((tag, key, val))


def load_shortcuts(path):
    with open(path, "rb") as f:
        buf = f.read()
    # header: 0x00 "shortcuts" 0x00 <map>
    if buf[0] != 0x00:
        raise ValueError("not a binary shortcuts.vdf")
    root_key, pos = _read_cstr(buf, 1)
    items, pos = _read_map(buf, pos)
    return root_key, items


def _write_map(out, items):
    for tag, key, val in items:
        out.append(tag)
        out += key.encode("utf-8") + b"\x00"
        if tag == 0x00:
            _write_map(out, val)
        elif tag == 0x01:
            out += str(val).encode("utf-8") + b"\x00"
        elif tag == 0x02:
            out += int(val).to_bytes(4, "little", signed=False)
    out.append(0x08)


def dump_shortcuts(path, root_key, items):
    out = bytearray()
    out.append(0x00)
    out += root_key.encode("utf-8") + b"\x00"
    _write_map(out, items)   # entries + the 0x08 that closes the "shortcuts" map
    out.append(0x08)         # ... and the 0x08 that closes the implicit root map (the file wrapper)
    # Atomic replace: Steam may re-read this file the instant it restarts, so it must never observe a
    # half-written shortcuts.vdf (that would drop every non-Steam game). Write a temp + rename.
    tmp = path + ".deckback.tmp"
    with open(tmp, "wb") as f:
        f.write(out)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def _entry_field(entry_items, name):
    for tag, key, val in entry_items:
        if key.lower() == name.lower():
            return val
    return None


def _steam_running():
    """True if a live process named exactly 'steam' exists (Linux /proc scan, no deps)."""
    try:
        pids = os.listdir("/proc")
    except OSError:
        return False
    for pid in pids:
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/comm") as f:
                if f.read().strip() == "steam":
                    return True
        except OSError:
            continue
    return False


def _matches(entry, appname):
    """A shortcut matches if the query appears in its AppName or its Exe (case-insensitive)."""
    q = appname.lower()
    return (q in (_entry_field(entry, "AppName") or "").lower()
            or q in (_entry_field(entry, "Exe") or "").lower())


def grid_id(exe, appname):
    """Legacy 32-bit grid appid used for custom-artwork filenames."""
    crc = zlib.crc32((exe + appname).encode("utf-8")) & 0xFFFFFFFF
    return crc | 0x80000000


# ---------------------------------------------------------------------------------------------

def _find_shortcuts_vdf(explicit):
    if explicit:
        return [explicit] if os.path.isfile(explicit) else []
    pats = [
        os.path.expanduser("~/.steam/steam/userdata/*/config/shortcuts.vdf"),
        os.path.expanduser("~/.local/share/Steam/userdata/*/config/shortcuts.vdf"),
    ]
    seen, out = set(), []
    for p in pats:
        for f in glob.glob(p):
            real = os.path.realpath(f)
            if real not in seen:
                seen.add(real); out.append(f)
    return out


def cmd_remove(args):
    # Steam rewrites shortcuts.vdf from memory when it exits, so editing the file while Steam is up
    # silently loses the edit (and this is the destructive path over ALL non-Steam games). Refuse,
    # as the docstring and README promise, unless the caller insists with --force.
    if _steam_running() and not args.force:
        print("Steam is running — close it first (edits are lost when Steam exits), or pass --force",
              file=sys.stderr)
        return 3
    vdfs = _find_shortcuts_vdf(args.vdf)
    if not vdfs:
        print("no shortcuts.vdf found", file=sys.stderr); return 3
    total = 0
    for vdf in vdfs:
        root_key, items = load_shortcuts(vdf)
        # index of the last matching entry, so --keep-last can retain exactly one (the newest add)
        match_idx = [i for i, (_t, _k, e) in enumerate(items) if _matches(e, args.appname)]
        keep = match_idx[-1] if (args.keep_last and match_idx) else None
        kept, removed_ids = [], []
        for i, (tag, key, entry) in enumerate(items):   # each entry: (0x00, "0", [fields])
            if _matches(entry, args.appname) and i != keep:
                removed_ids.append(grid_id(_entry_field(entry, "Exe") or "",
                                           _entry_field(entry, "AppName") or ""))
            else:
                kept.append((tag, key, entry))
        if len(kept) == len(items):
            print(f"{vdf}: no '{args.appname}' entries"); continue
        # re-index the surviving entries 0..n-1 (Steam keys them by ordinal)
        reindexed = [(t, str(i), e) for i, (t, _k, e) in enumerate(kept)]
        # Back up ONCE: never overwrite an existing backup, or a second run would replace the pristine
        # original with the already-edited file and there would be nothing to restore.
        bak = vdf + ".deckback.bak"
        if not os.path.exists(bak):
            shutil.copy2(vdf, bak)
        dump_shortcuts(vdf, root_key, reindexed)
        removed = len(items) - len(kept)
        total += removed
        print(f"{vdf}: removed {removed} '{args.appname}' entries (backup: {vdf}.deckback.bak)")
        # clean grid art for the removed entries
        grid = os.path.join(os.path.dirname(vdf), "grid")
        for gid in removed_ids:
            for suf in ("", "p", "_hero", "_logo", "_icon"):
                for ext in (".png", ".jpg", ".ico"):
                    fp = os.path.join(grid, f"{gid}{suf}{ext}")
                    if os.path.exists(fp):
                        os.remove(fp); print(f"  rm grid/{gid}{suf}{ext}")
    print(f"removed {total} shortcut(s) named like '{args.appname}'")
    return 0


def cmd_art(args):
    vdfs = _find_shortcuts_vdf(args.vdf)
    if not vdfs:
        print("no shortcuts.vdf found", file=sys.stderr); return 3
    art = {
        "": os.path.join(args.assets, "header.png"),
        "p": os.path.join(args.assets, "capsule.png"),
        "_hero": os.path.join(args.assets, "hero.png"),
        "_logo": os.path.join(args.assets, "logo.png"),
        "_icon": os.path.join(args.assets, "icon.png"),
    }
    installed = 0
    for vdf in vdfs:
        root_key, items = load_shortcuts(vdf)
        grid = os.path.join(os.path.dirname(vdf), "grid")
        os.makedirs(grid, exist_ok=True)
        for tag, key, entry in items:
            name = _entry_field(entry, "AppName") or ""
            exe = _entry_field(entry, "Exe") or ""
            if not _matches(entry, args.appname):
                continue
            gid = grid_id(exe, name)
            print(f"{vdf}: '{name}' exe={exe!r} -> grid id {gid}")
            for suf, src in art.items():
                if not os.path.isfile(src):
                    continue
                dst = os.path.join(grid, f"{gid}{suf}.png")
                shutil.copyfile(src, dst)
                print(f"  wrote grid/{gid}{suf}.png")
                installed += 1
    if not installed:
        print(f"no '{args.appname}' shortcut found to skin", file=sys.stderr); return 2
    print(f"installed {installed} artwork file(s)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("remove", help="delete matching shortcuts + their grid art (Steam must be off)")
    r.add_argument("--appname", default="Deckback")
    r.add_argument("--keep-last", action="store_true",
                   help="retain the last matching entry (dedup instead of full removal)")
    r.add_argument("--force", action="store_true",
                   help="edit even if Steam is running (the edit will be lost when Steam exits)")
    r.add_argument("--vdf", default="")
    r.set_defaults(func=cmd_remove)
    a = sub.add_parser("art", help="install library artwork for the matching shortcut")
    a.add_argument("--appname", default="Deckback")
    a.add_argument("--assets", required=True, help="dir with capsule/hero/logo/header/icon .png")
    a.add_argument("--vdf", default="")
    a.set_defaults(func=cmd_art)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

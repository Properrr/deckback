#!/usr/bin/env python3
"""Manage the Deckback non-Steam shortcut + library artwork on a Steam Deck.

Runs ON the Deck, stdlib only (binary shortcuts.vdf parsed here).

  * `add`     — write the shortcut with appid = crc32(exe+appname) | 0x80000000 (backup kept).
  * `remove`  — delete every matching shortcut (backup kept) and its grid artwork.
  * `art`     — install capsule/hero/logo/header/icon PNGs into userdata/<id>/config/grid/.

Steam rewrites shortcuts.vdf from memory on exit, so `add`/`remove` refuse while Steam runs.
Grid files are named grid/<appid>{,p,_hero,_logo,_icon}.png after the entry's stored `appid`.
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
    # Atomic replace: a half-written shortcuts.vdf would drop every non-Steam game.
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


def art_id(entry, exe, appname):
    """The appid grid artwork must be named after: the entry's stored `appid` (unsigned), else
    grid_id(). Steam-assigned appids are not reproducible from the crc."""
    aid = _entry_field(entry, "appid")
    if isinstance(aid, int):
        return aid & 0xFFFFFFFF
    return grid_id(exe, appname)


def grid_id(exe, appname):
    """crc32(exe+appname) | 0x80000000, the conventional non-Steam grid appid."""
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
                removed_ids.append(art_id(entry, _entry_field(entry, "Exe") or "",
                                          _entry_field(entry, "AppName") or ""))
            else:
                kept.append((tag, key, entry))
        if len(kept) == len(items):
            print(f"{vdf}: no '{args.appname}' entries"); continue
        # re-index the surviving entries 0..n-1 (Steam keys them by ordinal)
        reindexed = [(t, str(i), e) for i, (t, _k, e) in enumerate(kept)]
        # Back up once: never overwrite the pristine original.
        bak = vdf + ".deckback.bak"
        if not os.path.exists(bak):
            shutil.copy2(vdf, bak)
        dump_shortcuts(vdf, root_key, reindexed)
        removed = len(items) - len(kept)
        total += removed
        print(f"{vdf}: removed {removed} '{args.appname}' entries (backup: {vdf}.deckback.bak)")
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
            gid = art_id(entry, exe, name)
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


def _new_shortcut_entry(appid, appname, exe, start_dir, icon, launch_opts):
    """A shortcuts.vdf entry with the field set Steam writes for a non-Steam game, with an explicit
    unsigned `appid` so Steam and `art` name the grid files alike."""
    return [
        (0x02, "appid", appid & 0xFFFFFFFF),
        (0x01, "AppName", appname),
        (0x01, "Exe", exe),
        (0x01, "StartDir", start_dir),
        (0x01, "icon", icon),
        (0x01, "ShortcutPath", ""),
        (0x01, "LaunchOptions", launch_opts),
        (0x02, "IsHidden", 0),
        (0x02, "AllowDesktopConfig", 1),
        (0x02, "AllowOverlay", 1),
        (0x02, "OpenVR", 0),
        (0x02, "Devkit", 0),
        (0x01, "DevkitGameID", ""),
        (0x02, "DevkitOverrideAppID", 0),
        (0x02, "LastPlayTime", 0),
        (0x01, "FlatpakAppID", ""),
        (0x00, "tags", []),
    ]


def _fresh_shortcuts_vdf_path():
    """A path to create shortcuts.vdf under when a Deck has never added a non-Steam game. Uses the
    first existing Steam `userdata/<id>/config/` dir (Steam has run at least once)."""
    for pat in (os.path.expanduser("~/.local/share/Steam/userdata/*/config"),
                os.path.expanduser("~/.steam/steam/userdata/*/config")):
        dirs = sorted(glob.glob(pat))
        if dirs:
            return os.path.join(dirs[0], "shortcuts.vdf")
    return ""


def cmd_add(args):
    if _steam_running() and not args.force:
        print("Steam is running — close it first (edits are lost when Steam exits), or pass --force",
              file=sys.stderr)
        return 3
    vdfs = _find_shortcuts_vdf(args.vdf)
    created = False
    if not vdfs:
        path = args.vdf or _fresh_shortcuts_vdf_path()
        if not path:
            print("no Steam userdata found — has Steam ever run on this Deck?", file=sys.stderr)
            return 3
        os.makedirs(os.path.dirname(path), exist_ok=True)
        root_key, items = "shortcuts", []
        created = True
        vdfs = [path]
    else:
        root_key, items = load_shortcuts(vdfs[0])

    path = vdfs[0]
    appid = grid_id(args.exe, args.appname)
    entry = _new_shortcut_entry(appid, args.appname, args.exe, args.start_dir, args.icon,
                                args.launch_options)
    # Drop any prior entries with our AppName so re-running is idempotent (no duplicate tiles).
    kept = [(t, k, e) for (t, k, e) in items if not _matches(e, args.appname)]
    kept.append((0x00, str(len(kept)), entry))
    reindexed = [(t, str(i), e) for i, (t, _k, e) in enumerate(kept)]
    if not created and not os.path.exists(path + ".deckback.bak"):
        shutil.copy2(path, path + ".deckback.bak")
    dump_shortcuts(path, root_key, reindexed)
    print(f"{path}: added '{args.appname}' (appid {appid}) exe={args.exe!r} opts={args.launch_options!r}"
          + ("" if created else f" (backup: {path}.deckback.bak)"))
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
    d = sub.add_parser("add", help="write the Deckback shortcut with a crc appid (Steam must be off)")
    d.add_argument("--appname", default="Deckback")
    d.add_argument("--exe", default="/usr/bin/flatpak")
    d.add_argument("--launch-options", default="run io.github.properrr.deckback")
    d.add_argument("--start-dir", default=os.path.expanduser("~/"))
    d.add_argument("--icon", default="")
    d.add_argument("--force", action="store_true",
                   help="edit even if Steam is running (the edit will be lost when Steam exits)")
    d.add_argument("--vdf", default="")
    d.set_defaults(func=cmd_add)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

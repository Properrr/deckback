#!/usr/bin/env python3
"""L0 coverage of the Flatpak packaging contract — no container, no runtime, no Deck.

`just flatpak` needs a 9.9 GB `out/deck` and pulls ~2 GB of runtime, so it will never run on every
push. What CAN run on every push is every *decision* the packaging encodes, and those decisions are
where the two shipped bugs lived:

  * `--device=input` was missing from `finish-args`, and its absence is silent. The app starts, the
    UI renders, and the controller does nothing. So does the touchscreen lock. Nothing logs.
  * The metainfo file was built, shipped in `flatpak/assets/`, and never installed — for a reason
    (flatpak-builder 1.2.3) that stopped being true the moment the pack image moved to trixie.

Both were invisible to every existing test, because every existing test looked at the launcher.
"""

import os
import re
import shlex
import subprocess
import sys
import unittest
from pathlib import Path

_HERE = Path(os.path.dirname(os.path.abspath(__file__)))
_REPO = _HERE.parent.parent

APP = "io.github.properrr.deckback"
MANIFEST = _REPO / "flatpak" / f"{APP}.yml"
DOCKERFILE = _REPO / "docker" / "Dockerfile"
INSTALL_SH = _REPO / "scripts" / "install.sh"
LINT_SH = _REPO / "scripts" / "flatpak-lint.sh"
METAINFO = _REPO / "flatpak" / "assets" / f"{APP}.metainfo.xml"

# `flatpak info --show-permissions io.github.properrr.deckback`, captured verbatim on 2026-07-10 from
# the bundle this repo built, installed into a fresh user dir inside the pack container. It is here so
# the parser is tested against what flatpak actually prints, not against what we imagine it prints.
REAL_PERMISSIONS = """[Context]
shared=network;ipc;
sockets=x11;pulseaudio;
devices=dri;input;
filesystems=xdg-run/pipewire-0:ro;

[Session Bus Policy]
org.freedesktop.ScreenSaver=talk

[System Bus Policy]
org.freedesktop.login1=talk

[Environment]
DECKBACK_COBALT_BIN=/app/bin/cobalt-zypak
"""


def finish_args(text):
    """The `- --flag` entries under `finish-args:`, ignoring comments and later blocks."""
    out, inside = [], False
    for line in text.splitlines():
        if re.match(r"^finish-args:\s*$", line):
            inside = True
            continue
        if inside:
            if line and not line[0].isspace():
                break
            m = re.match(r"^\s*-\s*(--\S+)", line)
            if m:
                out.append(m.group(1))
    return out


def grants_input(permissions):
    """Call the real `flatpak_grants_input` from scripts/lib.sh. True iff it exits 0.

    shlex.quote, not repr: Python's repr escapes newlines to a literal backslash-n, which bash inside
    single quotes passes through verbatim. `sed -n 's/^devices=//p'` then sees one long line and
    matches nothing, so every one of these tests would have "passed" by returning False for
    everything — including the case that must return True.
    """
    r = subprocess.run(
        ["bash", "-c", f". scripts/lib.sh && flatpak_grants_input {shlex.quote(permissions)}"],
        capture_output=True, text=True, cwd=_REPO)
    return r.returncode == 0


class FinishArgs(unittest.TestCase):
    def setUp(self):
        self.args = finish_args(MANIFEST.read_text())

    def test_the_parser_sees_the_real_manifest(self):
        # Guard the guard: if `finish_args` silently returned [], every test below would pass while
        # asserting nothing about anything.
        self.assertGreaterEqual(len(self.args), 8, self.args)
        self.assertIn("--share=network", self.args)

    def test_evdev_is_granted_and_it_is_not_device_all(self):
        """The gamepad and the FF_RUMBLE haptics both need /dev/input, and both fail
        silently without it — no error, no log, just a controller that does nothing."""
        self.assertIn("--device=input", self.args)
        self.assertNotIn("--device=all", self.args)

    def test_require_version_covers_the_flag_we_use(self):
        """`--device=input` landed in flatpak 1.15.6, so 1.16.0 is the first stable release that
        understands this app's metadata. Without --require-version a 1.14 host installs it happily
        and hands the user a dead gamepad."""
        rv = [a for a in self.args if a.startswith("--require-version=")]
        self.assertEqual(len(rv), 1, f"expected exactly one --require-version, got {rv}")
        ver = tuple(int(x) for x in rv[0].split("=", 1)[1].split("."))
        self.assertGreaterEqual(ver, (1, 16, 0), "must be >= the release that added --device=input")

    def test_mit_shm_is_available_to_chromium(self):
        # --socket=x11 without --share=ipc means every frame goes over the socket instead of shared
        # memory. 720p60 on a handheld.
        self.assertIn("--socket=x11", self.args)
        self.assertIn("--share=ipc", self.args)

    def test_the_sandbox_is_not_quietly_widened(self):
        for forbidden in ("--no-sandbox", "--device=all", "--filesystem=home", "--filesystem=host",
                          "--share=all", "--socket=session-bus", "--socket=system-bus"):
            self.assertNotIn(forbidden, self.args)

    def test_no_no_sandbox_anywhere_in_what_we_ship(self):
        """zypak is the sandbox story; Flathub rejects --no-sandbox. Check the launch wrappers too —
        `finish-args` is not the only place a flag can hide."""
        for f in sorted((_REPO / "flatpak" / "assets").glob("*.sh")):
            for i, line in enumerate(f.read_text().splitlines(), 1):
                code = line.split("#", 1)[0]
                self.assertNotIn("--no-sandbox", code, f"{f.name}:{i}")

    def test_the_app_id_never_says_youtube(self):
        self.assertIsNone(re.search(r"youtube|(^|[^a-z])yt([^a-z]|$)", APP, re.I))


class Manifest(unittest.TestCase):
    def test_the_metainfo_is_installed(self):
        """It exists, it validates, and until 2026-07-10 it was deliberately not installed because
        flatpak-builder 1.2.3 could not cope. That reason is gone; the file must actually ship, or
        `appstreamcli compose` has nothing to compose and the app has no catalogue entry."""
        self.assertIn(f"install -Dm644 {APP}.metainfo.xml", MANIFEST.read_text())
        self.assertTrue(METAINFO.exists())

    def test_the_metainfo_has_what_appstreamcli_demands(self):
        # These two were the exact warnings `appstreamcli validate` reported before this change.
        xml = METAINFO.read_text()
        self.assertIn('<url type="homepage">', xml, "url-homepage-missing")
        self.assertIn("<developer", xml, "developer-info-missing")

    def test_the_launcher_module_can_find_the_page_scripts(self):
        """flatpak-builder copies a module's sources FLAT, so `../config/scripts/` — the path
        `launcher/CMakeLists.txt` globs in-repo — does not exist inside the module build dir. The
        manifest must add the whole scripts dir as a source AND point CMake at it. Getting this wrong
        failed the build loudly; getting it *half* right (source added, flag missing) would embed
        nothing. The dir is the single source of truth for every injected page script (ScriptLibrary)."""
        text = MANIFEST.read_text()
        self.assertIn("-DDECKBACK_SCRIPTS_DIR=scripts", text)
        self.assertIn("dest: scripts", text)
        self.assertIn("path: ../config/scripts", text)

    def test_runtime_and_sdk_branches_agree(self):
        text = MANIFEST.read_text()
        rt = re.search(r"^runtime-version:\s*'([^']+)'", text, re.M)
        self.assertIsNotNone(rt)
        # 22.08 and 23.08 already carry EOL markers on Flathub; two branches are supported at a time.
        self.assertGreaterEqual(tuple(int(x) for x in rt.group(1).split(".")), (25, 8),
                                "24.08 goes EOL when 26.08 ships; see packaging-toolchain.md")


class PackImage(unittest.TestCase):
    def test_pack_does_not_inherit_the_debian_12_engine_image(self):
        """`pack` runs flatpak-builder, which builds every module inside the SDK sandbox. Nothing
        about its host distro reaches the artifact — and Debian 12's flatpak (1.14.10) cannot express
        --device=input, while its flatpak-builder (1.2.3) cannot install a metainfo file."""
        text = DOCKERFILE.read_text()
        self.assertNotIn("FROM dev AS pack", text)
        self.assertIn("FROM ${PACK_BASE} AS pack", text)
        self.assertRegex(text, r"ARG PACK_BASE=debian:1[3-9]-slim")

    def test_pack_base_arg_is_global_so_from_can_use_it(self):
        """An ARG only reaches a `FROM` line if it is declared before the first FROM. Getting this
        wrong fails with `base name (${PACK_BASE}) should not be blank`, which names neither."""
        text = DOCKERFILE.read_text()
        self.assertLess(text.index("ARG PACK_BASE="), text.index("FROM ${BASE} AS build"))

    def test_the_version_floors_are_asserted_in_the_image(self):
        text = DOCKERFILE.read_text()
        self.assertIn("1.16.0", text)  # flatpak: --device=input
        self.assertIn("1.4.0", text)   # flatpak-builder: appstreamcli compose
        for tool in ("appstreamcli", "desktop-file-validate", "dbus-run-session"):
            self.assertIn(tool, text)


class InstallScript(unittest.TestCase):
    def test_the_silent_override_is_gone(self):
        """It read:

            flatpak override --user --device=input ... 2>/dev/null \\
              && info "Granted ..." || info "note: ... not applied"

        Both branches return 0, and the failing one swallows flatpak's own error. `just install`
        printed a note and reported success while handing the user a dead gamepad. F1 in miniature.
        """
        # Match a COMMAND, not prose: the die_assert message legitimately tells the user how to run
        # `flatpak override` by hand, and the comment above it explains why the call was removed.
        for i, line in enumerate(INSTALL_SH.read_text().splitlines(), 1):
            self.assertNotRegex(line, r"^\s*flatpak override\b",
                                f"scripts/install.sh:{i} still calls flatpak override")
        self.assertIn("flatpak_grants_input", INSTALL_SH.read_text())

    def test_missing_evdev_is_an_assert_not_a_note(self):
        text = INSTALL_SH.read_text()
        self.assertIn("die_assert", text)

    def test_parser_accepts_the_real_flatpak_output(self):
        self.assertTrue(grants_input(REAL_PERMISSIONS))

    def test_parser_rejects_an_app_without_evdev(self):
        without = REAL_PERMISSIONS.replace("devices=dri;input;", "devices=dri;")
        self.assertFalse(grants_input(without))

    def test_parser_rejects_an_app_with_no_devices_line_at_all(self):
        without = "\n".join(l for l in REAL_PERMISSIONS.splitlines()
                            if not l.startswith("devices="))
        self.assertFalse(grants_input(without))

    def test_device_all_counts_as_evdev(self):
        """Our manifest must never ask for --device=all, but a user who granted it by hand has a
        working gamepad. Refusing to recognise it would fail `just install` on a working install."""
        alldev = REAL_PERMISSIONS.replace("devices=dri;input;", "devices=all;")
        self.assertTrue(grants_input(alldev))

    def test_a_filesystem_path_containing_input_is_not_a_device(self):
        # Only the `devices=` line may answer this question.
        tricky = REAL_PERMISSIONS.replace("devices=dri;input;", "devices=dri;") \
                                 .replace("filesystems=xdg-run/pipewire-0:ro;",
                                          "filesystems=/mnt/input;")
        self.assertFalse(grants_input(tricky))


class LintScript(unittest.TestCase):
    def test_it_exists_and_is_executable(self):
        self.assertTrue(os.access(LINT_SH, os.X_OK))

    def test_it_refuses_arguments_rather_than_ignoring_them(self):
        r = subprocess.run([str(LINT_SH), "check"], capture_output=True, text=True, cwd=_REPO)
        self.assertEqual(r.returncode, 5, r.stderr)  # EX_USAGE

    def test_every_finish_arg_invariant_has_a_gate(self):
        """The gate script and this file must not drift apart: anything asserted here should be
        catchable on a push, and `flatpak-lint.sh` is what CI runs."""
        text = LINT_SH.read_text()
        for needle in ("--device=input", "--device=all", "--share=ipc", "--require-version=1.16.",
                       "--no-sandbox", "metainfo.xml"):
            self.assertIn(needle, text, f"flatpak-lint.sh has no gate mentioning {needle}")


if __name__ == "__main__":
    unittest.main(verbosity=2)

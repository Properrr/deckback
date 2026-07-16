# Support

Deckback is an **unofficial** client for YouTube's TV interface, built on the open-source Cobalt
engine. It is not affiliated with, endorsed by, or supported by Google or YouTube.

## Controls

Deckback is controller-first (a 10-foot / "Leanback" TV layout). The default mapping:

| Control | Action |
|---|---|
| D-pad / left stick | Move focus |
| **Right stick** | Fast scroll — the harder you push, the faster the focus travels |
| **A** | Select / OK |
| **B** | Back |
| **X** | Play / pause |
| **L1 / R1** | Scrub back / forward in the player (see note) |
| **View (⧉)** | Toggle captions |
| **Menu (☰)** | Open the **Settings** menu (see below) |

The first time Deckback starts, it shows this controls table on screen as a one-time card. Press any
button to dismiss it. The card is generated from your `app.json`, so if you rebind a control it will
say what the control actually does — and it leaves out anything that does nothing. (Set
`first_run_overlay: false` to skip it, or delete `~/.local/state/deckback/first_run_v1` to see it
again on the next launch.) After that, the same list lives in the **Settings** menu's **Keys** tab.

## Settings menu

A small **Settings** button sits in the top-right corner (except while a video is playing). Press
**Menu (☰)** to open the on-screen menu. It has these tabs:

- **Settings ▸ Keys** — the controller hot-keys currently in use, read live from your `app.json` (so
  it always matches your real mapping). More settings arrive here over time.
- **Updates** — the configured update policy and its current status. In notify mode, a new release
  shows what's new and the actions to take; the screen does not claim your version is current before
  the portal has reported an update state.
- **About** — what Deckback is, its features, the version you're running, and links to the project
  and this support page.

Navigate with the **D-pad** (the **right stick** scrolls a long list), **A** to select, **B** to go
back or close, and **L1 / R1** to switch tabs. When an update is available a small amber dot appears
on the Settings button; on the **Updates** tab, **A** on *Update now* installs it (it applies the
next time you open Deckback) and **Y** ignores that version. Nothing installs on its own in the
default `notify` mode — see [Updates](#updates).

> **Note on seeking.** L1/R1 send the player's arrow keys, which *scrub* the progress bar — the same
> model the console YouTube apps use. They are **not** a fixed ±10 second jump. YouTube's TV
> interface publishes no keyboard shortcut for a fixed-interval seek, and Deckback will not guess at
> one. (Earlier versions of this page described L1/R1 as "seek 10s". That was wrong.)

**Y** (voice search) is deliberately unbound: voice search is not a keyboard key at all — the TV
interface reaches it through a platform service this build does not have, so the supported path is to
move focus onto the on-screen microphone button and press **A**. It is listed as unmapped in the
startup log. (**Menu (☰)** opens the Settings menu; **L2 / R2** jump to the previous/next chapter.)

The mapping lives in `app.json` (`keymap`) and is hot-swappable — a server-side Leanback change can
be worked around by editing config, without reinstalling. Each button maps to a semantic action or a
DOM key dispatched verbatim; an unmapped entry is logged at startup rather than silently doing
nothing.

## Updates

By default (`self_update_mode: "notify"` in `app.json`) Deckback **tells you** when a new version is
out but never installs it behind your back. A new release shows up as an amber dot on the Settings
button and, on the **Updates** tab, the changelog with **Update now** / **Ignore this version**.
Choosing *Update now* downloads only Deckback, from its own repo — no root, no password — and it
applies the next time you open the app. Prefer silent updates? Set `self_update_mode: "auto"`.
`"off"` disables it and you update from Desktop Mode with `flatpak update`. Keeping the runtime and
your other Flatpaks current is always a separate `flatpak update`.

## Audio

Deckback sends Chromium audio through PipeWire's PulseAudio compatibility socket. The Flatpak
launcher checks the Deckback Chromium sink input on startup and after resume, but SteamOS denies
mute changes from inside the sandbox (`Access denied`). The host-side user service installed by
`scripts/install-audio-repair.sh` watches only Deckback's sink input and clears stale mute state.
It was verified on the OLED Deck by muting the live stream and observing `Mute: no` after recovery.

If video is playing but silent, use the Deck's hardware volume controls and confirm the output is
set to **Speaker**, not headphones or HDMI. From Desktop Mode, inspect the route with:

```sh
wpctl status
XDG_RUNTIME_DIR=/run/user/1000 pactl list short sink-inputs
XDG_RUNTIME_DIR=/run/user/1000 pactl list sink-inputs
```

The Deckback Chromium sink input should show `Corked: no`, `Mute: no`, and a route to the Speaker
sink. A short hardware-path check is:

```sh
runuser -u deck -- env XDG_RUNTIME_DIR=/run/user/1000 \
  timeout 5 speaker-test -D pulse -c 2 -t sine -f 440
```

If the tone is audible but YouTube is silent, collect the Deckback log. If the tone is also silent,
troubleshoot SteamOS audio output or the hardware before debugging Deckback.

## Appearance

If the overlay text on the player or Nerd Stats shows up on opaque black rectangles, the app is
likely running with a desktop/SteamDeck UA override instead of the shipped TV-client UA. Restore the
bundled `config/app.json` value and relaunch.

### Applying the controller layout

Non-Steam apps can't ship an official layout, so apply it once:

1. In Game Mode, open the Deckback shortcut → the controller (gamepad) icon → layout settings.
2. **Import** `config/steam_input.vdf` (the "Deckback" layout), or pick the community layout of that
   name. This maps the rear grips (L4/L5/R4/R5), trackpads, and gyro, and passes the standard controls
   through to Deckback's input layer.

> The bundled `steam_input.vdf` is a reviewed starting template. If Steam won't import it cleanly on
> your SteamOS version, build a layout in Steam's configurator using the table above and export it.

### Touchscreen

By default the touchscreen is made **inert** (`disable_touch: true` in `app.json`), because under
Game Mode a stray finger or palm arrives as a mouse click that can navigate YouTube by accident.
Two mechanisms enforce this, and the cursor is hidden while Deckback is focused:

- The page ignores every pointer/mouse/touch event (an injected script, `no_pointer.js`).
- The compositor is held in *hover* mode for Deckback's window, so a tap moves the cursor but never
  clicks (Steam's own overlay touch is unaffected).

Verified on an OLED Deck (2026-07-10): a tap produced cursor movement and **no** click, and did not
navigate. The **controller is never affected**. To get tap-to-activate back, set
`"disable_touch": false` in `app.json` and relaunch.

> An earlier version tried to *lock* the panel with an exclusive `EVIOCGRAB` grab, toggled by an
> L3+R3 chord. That approach is **dead**: on SteamOS the compositor — not the app — reads the
> touchscreen, so the grab does nothing. The old `touch_lock_*` settings ship disabled and have no
> effect; `disable_touch` above is the supported mechanism.

## When it can't connect

If Deckback can't reach YouTube, it shows its own screen with a **Try again** button, already
focused — press **A** (or Enter) to retry. It also retries on its own every few seconds, backing off
to about every 30 seconds while the network stays down, and recovers by itself the moment it comes
back. The small grey line at the bottom is the underlying network error; include it if you file a
bug.

**B / Escape does nothing on that screen, on purpose.** The only thing it could do is quit the app,
and losing your place because the Wi-Fi blinked would be worse than waiting.

You can turn this off (`error_page: false` in `app.json`) to get Chromium's built-in error page back,
but be aware that page has no button a controller can reach.

## DRM / Widevine (optional)

DRM'd content (rentals, some originals) needs a Widevine CDM. **Deckback does not bundle or download
Google's CDM for you** — free YouTube works without it. To enable DRM yourself:

1. Obtain a Linux x64 `libwidevinecdm.so` you trust and note its SHA-256
   (`sha256sum libwidevinecdm.so`).
2. In `app.json`, set:
   ```json
   "cdm_url": "https://<a host you trust>/libwidevinecdm.so",
   "cdm_sha256": "<the 64-char hex digest>"
   ```
3. On next launch Deckback downloads it, **verifies the hash**, and installs it into the profile. A
   hash mismatch is refused. Without `cdm_url` the launcher only detects an already-installed CDM.

DRM'd streams are resolution-capped by design (L3 software CDM). See `docs/legal.md`.

## Typing (search and sign-in)

Deckback is controller-first. Move focus with the D-pad and press **A** — YouTube's TV interface
draws its own on-screen keyboard, so search and sign-in work with the controller alone.

If you want a keyboard, read this first:

> **⚠ Open the on-screen keyboard from the Quick Access Menu (the "…" button), not `STEAM + X`.**
> On non-Steam applications like Deckback, `STEAM + X` can summon the *desktop* on-screen keyboard
> over Game Mode, which can leave the session unresponsive.

Two further limitations, neither of which Deckback can fix:

- **The keyboard never appears by itself** when you focus a text field. Deckback runs as an X11
  client under gamescope, and the automatic pop-up requires a Wayland facility we do not have.
  Summon the keyboard manually.
- **A USB or Bluetooth keyboard works**, but Game Mode interprets it as **QWERTY** regardless of the
  layout you configured in SteamOS.

Voice search is reached by focusing the on-screen microphone button and pressing **A** — not by a
button on the controller. See the note under [Controls](#controls).

## Sign-in

Sign-in persists across launches, reboots, and updates: the profile lives in the Flatpak's durable
data dir (`~/.var/app/io.github.properrr.deckback/data/deckback/profile`). Deleting that directory
resets the app to a fresh, signed-out state.

### Profile migration during upgrades

New versions migrate a legacy profile on first launch when the durable profile is absent or empty.
The migration checks these legacy locations, in order:

1. `$XDG_RUNTIME_DIR/deckback-profile`
2. `$XDG_CONFIG_HOME/content_shell`
3. `$HOME/.config/content_shell`

The source is copied into a temporary durable directory and promoted only after the copy completes.
An existing durable profile is never overwritten. Invalid symlinks and failed copies are rejected and
reported in the Deckback log without exposing account data.

This can recover a profile during an in-place upgrade while the old runtime directory still exists.
It cannot recover a runtime profile after reboot or logout, because `$XDG_RUNTIME_DIR` is temporary;
in that case, sign in again once and future launches use the durable profile.

## Remote Game Mode launch

With the Deck configured in `.steamdeck_auth`, launch the installed Flatpak on the active Game Mode
display without navigating Steam manually:

```sh
just remote-run
```

This resolves the current non-Steam shortcut ID and sends Steam the full `steam://rungameid/`
URI. It launches through Steam's Game Mode surface, leaves Steam running, and preserves the Flatpak
profile and sign-in.

## Collecting logs for a bug report

```sh
# On the Deck, in Desktop Mode konsole:
flatpak run io.github.properrr.deckback --collect-logs
# → writes ~/deckback-logs-<date>.tar.gz  (no telemetry leaves the device)
```

Attach that archive to a GitHub issue along with:

- Deck model (LCD / OLED) and SteamOS version (`Settings → System`).
- Whether you were in Game Mode or Desktop Mode.
- What you did and what happened.

## Sleep & battery

SteamOS Game Mode dims the screen and auto-suspends after a few minutes with no **controller** input,
and it does **not** count video playback as activity — so by default it will black out the screen
(audio keeps playing) and eventually suspend in the middle of a video. Deckback can't stop this from
*inside* the Flatpak sandbox: SteamOS exposes no screensaver-inhibit service there, and gamescope
ignores sandbox-emulated input for its idle timer (only a real input device counts).

**The fix — the idle-nudge helper (recommended).** A small host-side service that keeps the screen on
and prevents auto-suspend by gently nudging a real input device every ~25 s **while a video is
playing** (SteamOS ignores anything the sandbox can do, so it has to run on the host — same as the
audio helper). It only nudges during playback, so the Deck still sleeps normally in menus, and the
power button works normally. Install it once, in **Desktop Mode**:

    just idle-nudge      # or: ./scripts/install-idle-nudge.sh

It self-uninstalls if you later remove Deckback (Flatpak can't run host commands on uninstall, so the
helper watches for that and cleans itself up). Uninstall manually with:

    systemctl --user disable --now deckback-idle-nudge.service
    rm ~/.local/bin/deckback-idle-nudge ~/.config/systemd/user/deckback-idle-nudge.service

Without the helper, the Deck simply auto-suspends during long idle playback and **resumes where you
left off** on wake — normal Deck behavior, just not TV-like.

## Known limitations

- DRM'd content (rentals, some originals) is resolution-capped by design (L3 software CDM). Free
  YouTube content is unaffected.
- **Video is hardware-decoded (VA-API, VP9)**, verified clean on the Steam Deck OLED. (Earlier builds
  fell back to software because an older engine corrupted the hardware path; the current engine fixes
  it. Not yet verified on the LCD — see the OLED-only note below.)
- AV1 is steered off in favour of VP9/H.264: hardware AV1 decode on the Deck is unproven.
- The app depends on Google continuing to serve the TV interface to this client; server-side
  changes can break it until a config hotfix ships.
- **The on-screen keyboard does not open automatically** when a text field is focused, and Deckback
  cannot make it. See [Typing](#typing-search-and-sign-in).
- **On-screen button prompts may not match your controller.** YouTube's TV interface draws its own
  remote-style glyphs from the server; Deckback cannot replace them with Steam Deck glyphs.
- L1/R1 scrub the progress bar rather than seeking a fixed number of seconds. YouTube's TV interface
  publishes no fixed-interval seek key, and Deckback does not guess at one.
- **Steam Deck LCD is untested.** Deckback is developed and verified on a Steam Deck **OLED**. The
  LCD model uses the same code path and should work, but nobody on the project owns one, so it has
  never been run there. Battery figures, refresh-rate advice, and resume timing in this document were
  measured on the OLED and do **not** carry over. If you run Deckback on an LCD, please open an issue
  with your results — that is currently the only way this gap gets closed.

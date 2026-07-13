#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace deckback {

// Runtime config loaded from config/app.json (doc §6). Kept intentionally flat here — the launcher
// only needs a handful of fields; the full keymap is forwarded to the input layer (Phase 3).
struct Config {
  std::string url = "https://www.youtube.com/tv";
  std::string user_agent;
  int remote_debugging_port = 0;
  bool watchdog_restart_on_crash = true;
  int watchdog_max_restarts_per_minute = 5;
  std::vector<std::string> cobalt_flags;

  // Phase 3 input. Deck control name ("a", "b", "lb", "start", ...) -> either a DOM key value
  // ("Enter", "MediaPlayPause", "c") or a semantic action ("select", "back", "playpause"). Order is
  // as written in app.json. Empty means "use the built-in defaults". Hot-swappable by design (doc
  // §6): a Leanback change can be worked around by shipping a new app.json, without a rebuild.
  std::vector<std::pair<std::string, std::string>> keymap;

  // Optional keymap layers (findings input-ux §2/§12), all hot-swappable like `keymap` itself.
  //   keymap_player / keymap_osk — context layers, applied when the watch view is up or a text
  //   field
  //     has focus. They *override* the base map; unbound controls fall through.
  //   keymap_lt / keymap_rt — held-trigger modifier layers. A non-empty layer turns that trigger
  //   into
  //     a modifier (it stops dispatching its own key) and *absorbs* controls it does not bind.
  // Ship empty: no Leanback key is bound here without an on-Deck spike (no-guessing policy).
  std::vector<std::pair<std::string, std::string>> keymap_player, keymap_osk, keymap_lt, keymap_rt;

  // Touchscreen handling. On the Deck under gamescope the panel is delivered as synthetic MOUSE
  // events (a finger moves the cursor and taps click — accidental navigation), and the app cannot
  // grab the panel from inside (findings durable/touch-lock.md). `disable_touch` makes touch inert
  // two ways: the Navigator injects `no_pointer.js` to swallow every pointer/mouse/touch event in
  // the page (Option A), and `TouchModeGuard` holds gamescope's global touch mode at hover while
  // our window is focused (Option B). Default on. This SUPERSEDES the touch_lock_* fields below,
  // whose EVIOCGRAB lock is proven non-functional on SteamOS — they ship disabled and remain only
  // so a remote config cannot resurrect a dead lock by accident.
  bool disable_touch = true;

  // DEAD — retained disabled. `block_touchscreen` was the initial EVIOCGRAB lock state; while
  // `touch_lock_enabled`, the `touch_lock_chord` combo toggled it. Proven not to block touch on
  // this platform (durable/touch-lock.md); replaced by `disable_touch`. Do not re-enable without a
  // hardware retest of the grab.
  bool block_touchscreen = false;
  bool touch_lock_enabled = false;
  std::string touch_lock_chord = "l3+r3";
  int touch_lock_unlock_hold_ms = 800;
  bool touch_lock_toast = true;
  bool touch_lock_haptic = true;

  // First-run controls overlay (findings input-ux §17). `View (⧉)` = captions and `L3+R3` = touch
  // lock are unguessable, and SUPPORT.md is invisible from Game Mode. Shown once, then whenever the
  // control bound to the `show_controls` action is pressed.
  bool first_run_overlay = true;

  // Right-stick fast list traversal (findings input-ux §7/§15). The second axis is otherwise
  // unused, while every console app puts fast list traversal there. Rate is analog:
  // `right_stick_slow_ms` at the deadzone edge down to `right_stick_fast_ms` at full deflection.
  bool right_stick_scroll = true;
  int right_stick_deadzone = 13000;
  int right_stick_slow_ms = 200;
  int right_stick_fast_ms = 45;

  // Fixed-interval skip (findings input-ux §18). A trigger bound to `skip_back`/`skip_fwd` jumps the
  // player by this many seconds via the player's own seekBy() over CDP — chapter-aware seeking is
  // unbuildable on TVHTML5 (m138.md S0.6), so this is the plain ±N s console model. Hot-swappable.
  int skip_seconds = 10;

  // Phase 5 voice search (findings input-ux §13). Ships DISABLED: `kSbKeyMicrophone` does nothing
  // on this build, so voice must click Leanback's own soft-mic button — and whether that button
  // even exists under our Cobalt UA is the unverified V0 spike. A dead button is worse than a
  // missing one.
  bool voice_enabled = false;
  int voice_hold_ms = 250;           // hold this long before the mic opens (debounces a stray tap)
  std::string voice_duck = "pause";  // none | mute | pause — speakers sit ~15 cm from the mic array
  bool voice_click_toggles = false;  // true when the page's mic control is tap-to-toggle, not hold
  std::vector<std::string> voice_mic_selectors;

  // Phase 4/5 startup CDP policy, applied by the Navigator. `steer_av1` mirrors
  // quality.steer_av1_unsupported (AV1 hw decode on the Deck unproven/disputed — findings
  // durable/hardware.md); `mic_autogrant`
  // auto-approves the mic for the app origin so voice search needs no prompt (Phase 5).
  bool steer_av1 = true;
  bool mic_autogrant = true;

  // Failure-state UX (findings input-ux §7). Chromium's error interstitial has no control a
  // controller can focus, in an app with no address bar and no keyboard. Replace it with our own
  // Retry page and keep retrying with backoff. `error_title`/`error_hint` override the page's text:
  // empty means "derive it from the net:: error", and setting them is the R1 hotfix surface — if
  // Leanback breaks under us, the explanation ships as config, not as a rebuild.
  bool error_page = true;
  int error_retry_min_ms = 2000;
  int error_retry_max_ms = 30000;
  std::string error_title;
  std::string error_hint;

  // Phase 7 Widevine (best-effort, opt-in). Both empty = detect-only, never fetch. `cdm_url` points
  // at a raw libwidevinecdm.so the user trusts; `cdm_sha256` pins its digest. Never redistributed
  // by us (docs/legal.md).
  std::string cdm_url;
  std::string cdm_sha256;

  // Log rotation (doc §2 P2). Empty log_dir means "derive at runtime" (env / XDG_STATE_HOME).
  std::string log_dir;
  long log_max_bytes = 5'000'000;
  int log_max_files = 5;
  bool log_to_stderr = true;

  // Phase 6 power (doc §2 P6). The DevTools poll cadence for play-state → idle-inhibit, and whether
  // to fall back to synthetic activity when no logind idle inhibitor is available.
  int devtools_poll_ms = 1000;
  bool idle_inhibit_synthetic_fallback = false;
  // On resume, wait until this host:port is reachable before nudging the player (0 ms = skip).
  std::string resume_probe_host = "www.youtube.com";
  int resume_probe_port = 443;
  int resume_online_timeout_ms = 8000;
  // Reload (not just nudge) on resume if the Deck slept at least this long, to refresh stale stream
  // URLs / auth token. 0 = disabled (always nudge). Duration is measured with CLOCK_BOOTTIME.
  int resume_reload_after_ms = 0;

  // Loads and parses the JSON at `path`. Returns nullopt on read/parse failure.
  // NOTE: this is a minimal top-level-key extractor sufficient for app.json's shape; replace with a
  // real JSON parser when the config grows nested structure that must round-trip.
  static std::optional<Config> load(const std::string& path);
};

}  // namespace deckback

#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace deckback {

// Self-update policy. See Config::self_update_mode.
enum class SelfUpdateMode { Off, Notify, Auto };

// Parses "off" | "notify" | "auto" (case-sensitive). Anything else falls back to Notify, which
// never deploys without consent.
SelfUpdateMode parse_self_update_mode(std::string_view s);
const char* self_update_mode_name(SelfUpdateMode m);

// Runtime config loaded from config/app.json.
struct Config {
  std::string url = "https://www.youtube.com/tv";
  std::string user_agent;
  int remote_debugging_port = 0;
  bool watchdog_restart_on_crash = true;
  int watchdog_max_restarts_per_minute = 5;
  std::vector<std::string> cobalt_flags;

  // Deck control name ("a", "b", "lb", "start", ...) -> a DOM key value ("Enter", "MediaPlayPause",
  // "c") or a semantic action ("select", "back", "playpause"), in app.json order. Empty means "use
  // the built-in defaults".
  std::vector<std::pair<std::string, std::string>> keymap;

  // Optional keymap layers:
  //   keymap_player / keymap_osk: context layers, applied while the watch view is up or a text
  //     field has focus. They *override* the base map; unbound controls fall through.
  //   keymap_lt / keymap_rt: held-trigger modifier layers. A non-empty layer turns that trigger
  //     into a modifier (it stops dispatching its own key) and *absorbs* controls it does not bind.
  std::vector<std::pair<std::string, std::string>> keymap_player, keymap_osk, keymap_lt, keymap_rt;

  // Makes the touchscreen inert: the Navigator injects `no_pointer.js` to swallow every
  // pointer/mouse/touch event in the page, and `TouchModeGuard` holds gamescope's global touch mode
  // at hover while our window is focused.
  bool disable_touch = true;

  // Touch gesture router; mutually exclusive with `disable_touch`. The Navigator injects
  // `touch_gestures.js` instead of `no_pointer.js`, TouchModeGuard holds gamescope's mode at 4
  // (passthrough), and the launcher polls the page for recognised gestures and dispatches them as
  // trusted keys. Config-gated because touch cannot be feature-detected: `navigator.maxTouchPoints`
  // reads 0 even where touch works.
  bool touch_gestures = false;

  // How often the launcher drains the page's gesture queue; this is the touch input latency.
  int touch_poll_ms = 50;

  // Recogniser thresholds, passed through to the page script.
  int touch_step_px = 70;  // travel at which a drag's single move fires
  // Moves per gesture. 1 = one swipe is one move however far it travels (Leanback moves selection
  // per key, so a proportional drag double-steps on a long swipe). >1 = proportional scrolling.
  int touch_max_steps = 1;
  int touch_edge_px = 40;         // a touch starting this close to the left edge may be Back
  int touch_long_press_ms = 550;  // hold this long, without moving, for the held-rate gesture
  int touch_double_tap_ms = 280;  // pairing window for a double-tap seek

  // Press-and-hold playback rate, by zone: the right third (and the centre) speeds up, the left
  // third slows down. Snapped to the nearest rate the player advertises.
  double touch_hold_rate = 2.0;
  double touch_hold_slow_rate = 0.5;

  // Hide the X cursor while the gesture router is running. Injected separately so it can be
  // switched off in isolation.
  bool touch_hide_cursor = true;

  // Show the first-run controls overlay once.
  bool first_run_overlay = true;

  // Right-stick fast list traversal. Rate is analog: `right_stick_slow_ms` at the deadzone edge
  // down to `right_stick_fast_ms` at full deflection.
  bool right_stick_scroll = true;
  int right_stick_deadzone = 13000;
  int right_stick_slow_ms = 200;
  int right_stick_fast_ms = 45;

  // Seconds a trigger bound to `skip_back`/`skip_fwd` jumps the player, via its seekBy() over CDP.
  int skip_seconds = 10;

  // Confirm each caption toggle with a brief on-screen toast ("Captions on"/"off").
  bool captions_toast = true;
  // Preferred caption language when captions turn ON (e.g. "en", "pt-BR"). Empty = the SteamOS
  // system language (from the LANG/LC_* locale). Legacy scalar: seeds `caption_languages` when that
  // list is empty.
  std::string caption_language;
  // Ordered preferred caption languages, tried in order when captions turn ON. The special entry
  // "system" means the SteamOS locale. Empty falls back to `caption_language`, then "system".
  // Persisted in user.json.
  std::vector<std::string> caption_languages;
  // How a track is chosen within a preferred language: "author_first" (authored > auto >
  // translated, the default), "auto_first" (auto > authored > translated), "author_only", or
  // "auto_only".
  std::string caption_type = "author_first";
  // "local" (our preferred-language override + auto-apply on video start, the default) or
  // "youtube" (defer to YouTube; View is a plain on/off toggle in the system language). Persisted
  // in user.json.
  std::string caption_control = "local";
  // Whether the caption on/off state (and the last language) survive an app restart. Within a
  // session the on/off state always carries across videos.
  bool caption_remember = true;
  // The persisted local on/off state (only trusted when caption_remember). Written by the launcher.
  bool caption_on = false;

  // Startup CDP policy, applied by the Navigator. `steer_av1` mirrors
  // quality.steer_av1_unsupported; `mic_autogrant` auto-approves the mic for the app origin so
  // voice search needs no prompt.
  bool steer_av1 = true;
  bool mic_autogrant = true;

  // Replace Chromium's error interstitial with our own controller-focusable Retry page that keeps
  // retrying with backoff. `error_title`/`error_hint` override the page's text; empty means "derive
  // it from the net:: error".
  bool error_page = true;
  int error_retry_min_ms = 2000;
  int error_retry_max_ms = 30000;
  std::string error_title;
  std::string error_hint;

  // Widevine (opt-in). Both empty = detect-only, never fetch. `cdm_url` points at a raw
  // libwidevinecdm.so the user trusts; `cdm_sha256` pins its digest.
  std::string cdm_url;
  std::string cdm_sha256;

  // Log rotation. Empty log_dir means "derive at runtime" (env / XDG_STATE_HOME).
  std::string log_dir;
  long log_max_bytes = 5'000'000;
  int log_max_files = 5;
  bool log_to_stderr = true;

  // The DevTools poll cadence for play-state → idle-inhibit, and whether to fall back to synthetic
  // activity when no logind idle inhibitor is available.
  int devtools_poll_ms = 1000;
  bool idle_inhibit_synthetic_fallback = false;
  // Warn once, on first playback, when the host keep-awake helper is not running.
  bool keep_awake_warn = true;
  // On resume, wait until this host:port is reachable before nudging the player (0 ms = skip).
  std::string resume_probe_host = "www.youtube.com";
  int resume_probe_port = 443;
  int resume_online_timeout_ms = 8000;
  // Reload (not just nudge) on resume if the Deck slept at least this long, to refresh stale stream
  // URLs / auth token. 0 = disabled (always nudge). Duration is measured with CLOCK_BOOTTIME.
  int resume_reload_after_ms = 0;

  // Self-update policy via the Flatpak portal:
  //   off    — the updater is never constructed; update only via Desktop Mode `flatpak update`.
  //   notify — DEFAULT. Detect a newer commit and surface a passive indicator + a one-time card,
  //            but never deploy without the user pressing "Update now".
  //   auto   — deploy any newer commit in the background; it binds on the next launch (no prompt,
  //            no couch-reachable rollback). Opt-in.
  // The portal updates ONLY this app, from its own `deckback` remote. The legacy boolean
  // `self_update` still parses (true -> auto, false -> off).
  SelfUpdateMode self_update_mode = SelfUpdateMode::Notify;

  // Loads and parses the JSON at `path`. Returns nullopt when the file cannot be read, cannot be
  // parsed, is not a JSON object, or declares a schema_version newer than this launcher supports.
  // A key of the wrong type or out of range warns and keeps the default (or clamps); unknown keys
  // are reported. Binding is by exact JSON path (see kStringFields et al. in config.cpp).
  static std::optional<Config> load(const std::string& path);
};

}  // namespace deckback

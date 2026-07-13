#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio.hpp"
#include "cdm_fetcher.hpp"
#include "config.hpp"
#include "input.hpp"
#include "log.hpp"
#include "navigator.hpp"
#include "onboarding.hpp"
#include "platform.hpp"
#include "player.hpp"
#include "profile.hpp"
#include "touchmode.hpp"
#include "util.hpp"
#include "voice.hpp"
#include "watchdog.hpp"

namespace {

using namespace deckback;

constexpr std::string_view kVersion = "deckback-launcher 0.0.0 (scaffolding)";

void on_signal(int) { Watchdog::request_shutdown(); }

std::string env_or(const char* key, std::string fallback) {
  const char* v = std::getenv(key);
  return v ? std::string(v) : std::move(fallback);
}

// Single-instance lock: flock a file in the runtime dir. Held for the process lifetime.
bool acquire_single_instance_lock(const std::string& runtime_dir) {
  const std::string path = runtime_dir + "/deckback.lock";
  int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
  if (fd < 0) {
    warn("lock: cannot open " + path + " (continuing without single-instance guard)");
    return true;
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    error("lock: another Deckback instance is already running");
    return false;
  }
  return true;  // fd intentionally leaked: lock lives as long as the process
}

// Where small persistent launcher state lives (the first-run marker). Deliberately NOT the Chromium
// profile: that directory is Chromium's to manage, and `just install`'s reset path may wipe it.
std::string resolve_state_dir(const std::string& runtime_dir) {
  if (const char* v = std::getenv("XDG_STATE_HOME"); v && *v) return std::string(v) + "/deckback";
  if (const char* v = std::getenv("HOME"); v && *v)
    return std::string(v) + "/.local/state/deckback";
  return runtime_dir + "/deckback";
}

// Resolve the log directory: explicit config wins, then $DECKBACK_LOG_DIR, else `logs/` under the
// state dir (XDG_STATE_HOME / ~/.local/state / the runtime dir, in that order).
std::string resolve_log_dir(const std::string& cfg_dir, const std::string& runtime_dir) {
  if (!cfg_dir.empty()) return cfg_dir;
  if (const char* v = std::getenv("DECKBACK_LOG_DIR"); v && *v) return v;
  return resolve_state_dir(runtime_dir) + "/logs";
}

// Resolve the Chromium profile dir. Must be PERSISTENT so sign-in survives reboot/updates (doc §6
// P1): $DECKBACK_PROFILE wins, then $XDG_DATA_HOME/deckback/profile (which Flatpak points at the
// durable ~/.var/app/<id>/data), then ~/.local/share/deckback/profile. The runtime dir is a last
// resort only — XDG_RUNTIME_DIR is a tmpfs wiped on logout, so a profile there loses the login.
std::string resolve_profile_dir(const std::string& runtime_dir) {
  if (const char* v = std::getenv("DECKBACK_PROFILE"); v && *v) return v;
  if (const char* v = std::getenv("XDG_DATA_HOME"); v && *v)
    return std::string(v) + "/deckback/profile";
  if (const char* v = std::getenv("HOME"); v && *v)
    return std::string(v) + "/.local/share/deckback/profile";
  warn(
      "startup: no XDG_DATA_HOME/HOME — profile falls back to the ephemeral runtime dir; sign-in "
      "will not survive a reboot");
  return runtime_dir + "/deckback-profile";
}

// Turn an `app://` deep-link arg into a Leanback URL fragment (doc §6 P2).
std::string apply_deep_link(std::string base_url, const std::string& deep_link) {
  constexpr std::string_view kPrefix = "app://";
  if (deep_link.starts_with(kPrefix)) {
    const std::string video = deep_link.substr(kPrefix.size());
    if (!video.empty()) return base_url + "#?v=" + video;
  }
  return base_url;
}

int collect_logs() {
  // TODO(Phase 10): tar the app log dir + journald slice into ~/deckback-logs-<date>.tar.gz.
  info("collect-logs: not yet implemented (Phase 10 / docs/SUPPORT.md)");
  return 0;
}

// Usage goes to stdout unadorned — it is a reply to the user, not a log line.
void print_usage() {
  std::puts(
      "deckback-launcher — unofficial YouTube TV shell for Steam Deck\n"
      "\n"
      "Usage: deckback-launcher [options] [app://<video-id>]\n"
      "\n"
      "Options:\n"
      "  --config <path>    Config file (default: config/app.json)\n"
      "  --collect-logs     Write a support bundle to stdout and exit\n"
      "  --selftest-dbus    Report whether the logind backend is live and exit\n"
      "  --version          Print version and exit\n"
      "  -h, --help         Print this help and exit\n"
      "\n"
      "Arguments:\n"
      "  app://<video-id>   Deep link; opens that video (youtube.com/tv#?v=<video-id>)\n"
      "\n"
      "Environment:\n"
      "  DECKBACK_COBALT_BIN   Path to the content_shell binary\n"
      "  DECKBACK_EXTRA_ARGS   Extra flags appended to the engine command line\n"
      "  DECKBACK_LOG_DIR      Log directory (else XDG_STATE_HOME)\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "config/app.json";
  std::string deep_link;
  bool selftest_dbus = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--help" || a == "-h") {
      print_usage();
      return 0;
    } else if (a == "--version") {
      info(std::string(kVersion));
      return 0;
    } else if (a == "--collect-logs") {
      return collect_logs();
    } else if (a == "--selftest-dbus") {
      selftest_dbus = true;
    } else if (a == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (a == "--config") {
      error("--config requires a path");
      print_usage();
      return 2;
    } else if (a.starts_with("app://")) {
      deep_link = a;
    } else {
      // Previously any unrecognised argument — including --help — fell through here and the app
      // launched anyway. Launching YouTube because someone asked for usage is a bad surprise.
      error(std::format("unknown argument '{}'", a));
      print_usage();
      return 2;
    }
  }

  if (selftest_dbus) {
    info(std::string("selftest: backend ") +
         (Platform::backend_available() ? "libsystemd" : "stub (no libsystemd)"));
    return Platform::create()->selftest();
  }

  const std::string runtime_dir = env_or("XDG_RUNTIME_DIR", "/tmp");
  if (!acquire_single_instance_lock(runtime_dir)) return 1;

  auto cfg = Config::load(config_path);
  if (!cfg) {
    error("startup: failed to load config " + config_path);
    return 1;
  }
  const std::string log_dir = resolve_log_dir(cfg->log_dir, runtime_dir);
  log_init(log_dir + "/deckback.log", cfg->log_max_bytes, cfg->log_max_files, cfg->log_to_stderr);
  info(std::string(kVersion));

  if (cfg->user_agent.empty() || cfg->user_agent.starts_with("SET_FROM_SPIKE")) {
    warn("startup: user_agent unset — Leanback will not serve the TV app until S0.2 seeds it");
  }

  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  auto platform = Platform::create();

  // The input context (browse/player/osk). PlayerController's poll thread writes it, GamepadInput's
  // thread reads it. Declared here so it outlives both.
  LayerState layers;

  // Phase 6: the DevTools bridge drives suspend/resume and the play-state idle inhibitor. It needs
  // Cobalt's remote-debugging port; without it we fall back to logging-only hooks.
  std::optional<PlayerController> player;
  if (cfg->remote_debugging_port > 0) {
    player.emplace(
        *platform, "127.0.0.1", cfg->remote_debugging_port, cfg->devtools_poll_ms,
        cfg->idle_inhibit_synthetic_fallback,
        ResumeProbe{cfg->resume_probe_host, cfg->resume_probe_port, cfg->resume_online_timeout_ms},
        cfg->resume_reload_after_ms);
    player->set_layer_sink(&layers);
    platform->on_suspend([&] { player->on_suspend(); });
    platform->on_resume([&] { player->on_resume(); });
    player->start();
  } else {
    warn("startup: remote_debugging_port unset — Phase 6 player hooks disabled");
    platform->on_suspend([] { info("suspend: no DevTools bridge (remote_debugging_port unset)"); });
    platform->on_resume([] { info("resume: no DevTools bridge (remote_debugging_port unset)"); });
  }

  const std::string profile_dir = resolve_profile_dir(runtime_dir);
  migrate_legacy_profile(profile_dir, runtime_dir);
  info("startup: profile dir " + profile_dir);
  // Best-effort Widevine CDM; free YouTube unaffected if absent. Opt-in fetch via
  // cdm_url/cdm_sha256.
  const bool cdm_ready =
      CdmFetcher::ensure_installed(profile_dir, CdmConfig{cfg->cdm_url, cfg->cdm_sha256});

  const std::string cobalt_bin = env_or("DECKBACK_COBALT_BIN", "./content_shell");
  const std::string url = apply_deep_link(cfg->url, deep_link);

  // content_shell ignores --user-agent (hardcodes Chrome/999) and takes the start URL positionally.
  // Handing it the app URL directly would load it once with that default UA -> YouTube desktop
  // redirect. So when we can drive CDP we boot on about:blank and let the Navigator inject the TV
  // UA and navigate; only without a debugging port do we fall back to the positional URL (wrong
  // UA).
  const bool cdp_nav = cfg->remote_debugging_port > 0;
  std::vector<std::string> args;
  // content_shell/cobalt-shell's data-dir switch is --data-path (kContentShellDataPath);
  // --user-data-dir is a chrome/-layer switch the shell silently ignores (m114.md corrections).
  args.push_back("--data-path=" + profile_dir);
  if (cdp_nav)
    args.push_back(std::format("--remote-debugging-port={}", cfg->remote_debugging_port));
  // P7. The engine registers a CDM only when handed one: `patches/0001-…-widevine-cdm-registration`
  // adds the `AddContentDecryptionModules` override that upstream content_shell lacks, and it reads
  // this switch. Passed ONLY when a CDM is actually installed — pointing the engine at a path that
  // does not exist buys a `LOG(ERROR)` on every launch and no Widevine either way.
  //
  // Nothing about the CDM is shipped by us; it is user-fetched and hash-verified (docs/legal.md).
  // Unencrypted YouTube plays with or without it.
  if (cdm_ready) {
    const std::string cdm_path = CdmFetcher::installed_path(profile_dir);
    if (CdmFetcher::usable_cdm_path(cdm_path))
      args.push_back("--widevine-cdm-path=" + cdm_path);
    else
      warn("cdm: '" + cdm_path +
           "' is not an absolute path, so the engine will refuse it — Widevine stays off. Set "
           "DECKBACK_PROFILE to an absolute path.");
  }
  for (const auto& f : cfg->cobalt_flags) args.push_back(f);
  // Dev/bring-up passthrough for flags we don't want baked into the shipped config (e.g.
  // --no-sandbox before zypak lands in Phase 8). Whitespace-separated; empty when unset.
  if (const char* extra = std::getenv("DECKBACK_EXTRA_ARGS"); extra && *extra)
    for (std::string& a : split_whitespace(extra)) args.push_back(std::move(a));
  args.push_back(cdp_nav ? "about:blank" : url);

  // First-run controls card (findings input-ux §17). Drawn once the TV app has actually loaded —
  // there is no document to draw on before that — and re-openable from the `show_controls` action.
  //
  // Declared BEFORE the navigator and the gamepad: the navigator's thread captures it by reference
  // and the input thread holds a pointer to it, so it must outlive both. Both are stopped
  // explicitly below, but relying on that rather than on declaration order is a use-after-free
  // waiting for someone to add an early return.
  std::optional<OnboardingController> onboarding;
  std::optional<Navigator> navigator;
  std::optional<GamepadInput> gamepad;
  // Phase 5 voice search. Disabled by default: voice cannot be a keypress on this engine, and
  // whether Leanback even renders a soft-mic button under our Cobalt UA is the unverified V0 spike
  // (findings input-ux §13.2). Declared here so it outlives the gamepad that borrows it.
  std::optional<VoiceController> voice;
  if (cdp_nav && cfg->voice_enabled) {
    auto duck = parse_duck_mode(cfg->voice_duck);
    if (!duck) {
      warn(std::format("startup: voice_duck '{}' unrecognised (none|mute|pause) — using pause",
                       cfg->voice_duck));
      duck = DuckMode::Pause;
    }
    voice.emplace("127.0.0.1", cfg->remote_debugging_port,
                  VoiceConfig{cfg->voice_enabled, cfg->voice_hold_ms, *duck,
                              cfg->voice_click_toggles, cfg->voice_mic_selectors});
  }
  if (cdp_nav && cfg->first_run_overlay) {
    onboarding.emplace("127.0.0.1", cfg->remote_debugging_port,
                       OverlayContext{cfg->keymap, cfg->voice_enabled, cfg->right_stick_scroll,
                                      cfg->touch_lock_enabled, cfg->touch_lock_chord},
                       first_run_marker_path(resolve_state_dir(runtime_dir)));
  }

  if (cdp_nav) {
    navigator.emplace(
        "127.0.0.1", cfg->remote_debugging_port, cfg->user_agent, url, cfg->devtools_poll_ms,
        NavPolicy{cfg->steer_av1, cfg->mic_autogrant, cfg->error_page, cfg->error_retry_min_ms,
                  cfg->error_retry_max_ms, cfg->error_title, cfg->error_hint, cfg->disable_touch});
    if (onboarding)
      navigator->set_on_app_loaded([&onboarding] { onboarding->show(/*first_run_only=*/true); });
    navigator->start();
    // Phase 3 input: gamepad evdev -> DOM key events over CDP (S0.6 mechanism), bindings from
    // config/app.json:keymap so a Leanback change can be hotfixed without a rebuild. The touch
    // config drives the runtime touchscreen block/unblock (findings input-ux §4). `layers` carries
    // the context (browse/player/osk) the player poll observed; it is only live when `player`
    // exists.
    GamepadOptions gp;
    gp.keymap = KeymapConfig{cfg->keymap, cfg->keymap_player, cfg->keymap_osk, cfg->keymap_lt,
                             cfg->keymap_rt};
    gp.touch =
        TouchConfig{cfg->touch_lock_enabled,        cfg->touch_lock_chord, cfg->block_touchscreen,
                    cfg->touch_lock_unlock_hold_ms, cfg->touch_lock_toast, cfg->touch_lock_haptic};
    gp.fast_scroll = FastScrollConfig{cfg->right_stick_scroll, cfg->right_stick_deadzone,
                                      cfg->right_stick_slow_ms, cfg->right_stick_fast_ms};
    gp.layers = player ? &layers : nullptr;
    gp.voice = voice ? &*voice : nullptr;
    gp.onboarding = onboarding ? &*onboarding : nullptr;
    gamepad.emplace("127.0.0.1", cfg->remote_debugging_port, std::move(gp));
    gamepad->start();
  } else {
    warn(
        "startup: remote_debugging_port unset — cannot inject the TV UA over CDP; content_shell "
        "will load with its default UA (desktop redirect likely)");
  }

  // Option B of disable_touch: hold gamescope's touch mode at hover while our window is focused, so
  // a stray finger cannot even generate a click at the compositor. Independent of CDP — the
  // page-side swallow (Option A, in the Navigator) needs remote debugging; this does not.
  std::optional<TouchModeGuard> touch_mode;
  if (cfg->disable_touch) {
    touch_mode.emplace();
    touch_mode->start();
  }

  info(std::format("startup: launching {} -> {}", cobalt_bin, url));
  AudioRepair audio;
  audio.start();
  Watchdog wd(cobalt_bin, std::move(args), *cfg);
  int rc = wd.run();

  // Shut down in dependency order: stop the input + navigator + poll threads (they call into the
  // engine/platform), then the logind backend (its callbacks call into player), then flush logs.
  if (touch_mode) touch_mode->stop();
  if (gamepad) gamepad->stop();
  if (navigator) navigator->stop();
  audio.stop();
  if (player) player->stop();
  platform.reset();
  log_shutdown();
  return rc;
}

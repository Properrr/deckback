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

#include "about.hpp"
#include "audio.hpp"
#include "caption_settings.hpp"
#include "cdm_fetcher.hpp"
#include "config.hpp"
#include "config_store.hpp"
#include "gestures.hpp"
#include "http.hpp"
#include "input.hpp"
#include "keymap.hpp"
#include "log.hpp"
#include "navigator.hpp"
#include "onboarding.hpp"
#include "osdmenu.hpp"
#include "platform.hpp"
#include "player.hpp"
#include "profile.hpp"
#include "scripts.hpp"
#include "simwatch.hpp"
#include "touchmode.hpp"
#include "updateprompt.hpp"
#include "updater.hpp"
#include "util.hpp"
#include "version.hpp"
#include "watchdog.hpp"

namespace {

using namespace deckback;

std::string version_line() { return std::string("deckback-launcher ") + kDeckbackVersion; }

void on_signal(int) { Watchdog::request_shutdown(); }

std::string env_or(const char* key, std::string fallback) {
  const char* v = std::getenv(key);
  return v ? std::string(v) : std::move(fallback);
}

// Idle Updates-tab text for each policy. UpdateMonitor only reports positive availability, so none
// of these may claim the installed version is the latest.
std::string update_policy_status(SelfUpdateMode mode) {
  switch (mode) {
    case SelfUpdateMode::Off:
      return "Updates are off. Update Deckback in Discover or run flatpak update.";
    case SelfUpdateMode::Notify:
      return "No update announced yet. Deckback checks at launch and about every half hour — or "
             "check now.";
    case SelfUpdateMode::Auto:
      return "Automatic updates are enabled. New versions install when they are available.";
  }
  return "Update status is not available.";
}

// The published ref the update probe reads: the commit `flatpak update` would move to on the
// official remote. DECKBACK_UPDATE_REF_URL overrides it.
constexpr std::string_view kUpdateRefUrl =
    "https://properrr.github.io/deckback/repo/refs/heads/app/io.github.properrr.deckback/x86_64/"
    "master";
constexpr long kUpdateRefTimeoutSec = 6;

// Single-instance lock: flock a file in the runtime dir. Held for the process lifetime.
bool acquire_single_instance_lock(const std::string& runtime_dir) {
  const std::string path = runtime_dir + "/deckback.lock";
  // O_CLOEXEC: the engine must not inherit the lock, or an engine outliving the launcher would keep
  // it held and refuse the next launch.
  int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
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

// Small persistent launcher state (markers). Not the Chromium profile, which a reset may wipe.
std::string resolve_state_dir(const std::string& runtime_dir) {
  if (const char* v = std::getenv("XDG_STATE_HOME"); v && *v) return std::string(v) + "/deckback";
  if (const char* v = std::getenv("HOME"); v && *v)
    return std::string(v) + "/.local/state/deckback";
  return runtime_dir + "/deckback";
}

// Dir of the user.json settings overlay: XDG_CONFIG_HOME, then ~/.config, else the state dir.
std::string resolve_config_dir(const std::string& runtime_dir) {
  if (const char* v = std::getenv("XDG_CONFIG_HOME"); v && *v) return std::string(v) + "/deckback";
  if (const char* v = std::getenv("HOME"); v && *v) return std::string(v) + "/.config/deckback";
  return resolve_state_dir(runtime_dir);
}

// The system language (SteamOS sets LANG) for the caption "system" pseudo-language. POSIX
// precedence, skipping C/POSIX values (SteamOS ships one in LC_ALL alongside a real LANG); LANGUAGE
// is a colon-list, so its first entry is used.
std::string system_language() {
  for (const char* var : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char* v = std::getenv(var);
    if (!v || !*v) continue;
    std::string_view sv(v);
    if (const size_t c = sv.find(':'); c != std::string_view::npos) sv = sv.substr(0, c);
    std::string lang = caption_language_subtag(sv);
    if (!lang.empty()) return lang;
  }
  return {};
}

// Log directory: explicit config wins, then $DECKBACK_LOG_DIR, else `logs/` under the state dir.
std::string resolve_log_dir(const std::string& cfg_dir, const std::string& runtime_dir) {
  if (!cfg_dir.empty()) return cfg_dir;
  if (const char* v = std::getenv("DECKBACK_LOG_DIR"); v && *v) return v;
  return resolve_state_dir(runtime_dir) + "/logs";
}

// Persistent Chromium profile dir, so sign-in survives reboots and updates: $DECKBACK_PROFILE, then
// $XDG_DATA_HOME/deckback/profile, then ~/.local/share/deckback/profile. The runtime dir (a tmpfs
// wiped on logout) is a last resort.
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

// Turn an `app://<video-id>` deep link into a Leanback URL fragment.
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
      "  --selftest-update  Probe the Flatpak update portal (run inside the flatpak) and exit\n"
      "  --selftest-deploy  Drive one portal Update cycle (RAW permission) and report; exit\n"
      "  --selftest-deploy-seed  As above, but pre-grant self-update consent first\n"
      "  --selftest-watch <s>  Run the updater loop for <s>s (portal reconnect drive); exit\n"
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
  bool selftest_update = false;
  bool selftest_deploy = false;
  bool selftest_deploy_seed = false;
  bool selftest_watch = false;
  int selftest_watch_secs = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--help" || a == "-h") {
      print_usage();
      return 0;
    } else if (a == "--version") {
      info(version_line());
      return 0;
    } else if (a == "--collect-logs") {
      return collect_logs();
    } else if (a == "--selftest-dbus") {
      selftest_dbus = true;
    } else if (a == "--selftest-update") {
      selftest_update = true;
    } else if (a == "--selftest-deploy") {
      selftest_deploy = true;
    } else if (a == "--selftest-deploy-seed") {
      selftest_deploy = true;
      selftest_deploy_seed = true;
    } else if (a == "--selftest-watch" && i + 1 < argc) {
      selftest_watch = true;
      selftest_watch_secs = std::atoi(argv[++i]);
    } else if (a == "--selftest-watch") {
      error("--selftest-watch requires a duration in seconds");
      print_usage();
      return 2;
    } else if (a == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (a == "--config") {
      error("--config requires a path");
      print_usage();
      return 2;
    } else if (a.starts_with("app://")) {
      deep_link = a;
    } else {
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

  if (selftest_update) {
    info(std::string("selftest-update: backend ") +
         (Updater::backend_available() ? "libsystemd" : "stub (no libsystemd)"));
    return Updater::create(UpdaterConfig{})->selftest();
  }

  if (selftest_deploy) {
    info(std::string("selftest-deploy: backend ") +
         (Updater::backend_available() ? "libsystemd" : "stub (no libsystemd)") +
         (selftest_deploy_seed ? " — seeding consent first" : " — RAW permission (not seeded)"));
    return Updater::create(UpdaterConfig{})->selftest_deploy(selftest_deploy_seed);
  }

  if (selftest_watch) {
    return run_selftest_watch(selftest_watch_secs);
  }

  const std::string runtime_dir = env_or("XDG_RUNTIME_DIR", "/tmp");
  if (!acquire_single_instance_lock(runtime_dir)) return 1;

  auto cfg = Config::load(config_path);
  if (!cfg) {
    error("startup: failed to load config " + config_path);
    return 1;
  }
  ConfigStore user_store(resolve_config_dir(runtime_dir) + "/user.json");
  user_store.load();
  user_store.apply(*cfg);

  std::vector<std::string> cc_languages = cfg->caption_languages;
  if (cc_languages.empty() && !cfg->caption_language.empty())
    cc_languages = {cfg->caption_language};
  CaptionSettings captions(&user_store, cfg->caption_control, cc_languages, cfg->caption_type,
                           cfg->caption_remember, cfg->captions_toast, cfg->caption_on,
                           system_language());

  const std::string log_dir = resolve_log_dir(cfg->log_dir, runtime_dir);
  log_init(log_dir + "/deckback.log", cfg->log_max_bytes, cfg->log_max_files, cfg->log_to_stderr);
  info(version_line());

  // Page-script overrides: a same-named .js in the scripts dir (default <config dir>/scripts,
  // DECKBACK_SCRIPTS_DIR overrides) shadows the embedded default. Loaded before any worker thread
  // starts, so the registry is read-only once shared.
  {
    std::string scripts_dir = env_or("DECKBACK_SCRIPTS_DIR", "");
    if (scripts_dir.empty()) {
      const auto slash = config_path.find_last_of('/');
      const std::string cfg_dir = slash == std::string::npos ? "." : config_path.substr(0, slash);
      scripts_dir = cfg_dir + "/scripts";
    }
    ScriptLibrary::instance().load_overrides(scripts_dir);
  }

  if (cfg->user_agent.empty() || cfg->user_agent.starts_with("SET_FROM_SPIKE")) {
    warn("startup: user_agent unset — Leanback will not serve the TV app until S0.2 seeds it");
  }

  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  auto platform = Platform::create();

  // The input context (browse/player/osk). PlayerController's poll thread writes it, GamepadInput's
  // thread reads it. Declared here so it outlives both.
  LayerState layers;

  // The DevTools bridge drives suspend/resume and the idle inhibitor; it needs the engine's
  // remote-debugging port, otherwise the hooks only log.
  std::optional<PlayerController> player;
  if (cfg->remote_debugging_port > 0) {
    player.emplace(
        *platform, "127.0.0.1", cfg->remote_debugging_port, cfg->devtools_poll_ms,
        cfg->idle_inhibit_synthetic_fallback,
        ResumeProbe{cfg->resume_probe_host, cfg->resume_probe_port, cfg->resume_online_timeout_ms},
        cfg->resume_reload_after_ms);
    player->set_layer_sink(&layers);
    if (cfg->keep_awake_warn) player->set_keep_awake_probe(&keep_awake_state);
    platform->on_suspend([&] { player->on_suspend(); });
    platform->on_resume([&] { player->request_resume(); });
    player->start();
  } else {
    warn("startup: remote_debugging_port unset — Phase 6 player hooks disabled");
    platform->on_suspend([] { info("suspend: no DevTools bridge (remote_debugging_port unset)"); });
    platform->on_resume([] { info("resume: no DevTools bridge (remote_debugging_port unset)"); });
  }

  const std::string profile_dir = resolve_profile_dir(runtime_dir);
  migrate_legacy_profile(profile_dir, runtime_dir);
  info("startup: profile dir " + profile_dir);
  // Best-effort, opt-in Widevine CDM (cdm_url/cdm_sha256); unencrypted YouTube plays without it.
  const bool cdm_ready =
      CdmFetcher::ensure_installed(profile_dir, CdmConfig{cfg->cdm_url, cfg->cdm_sha256});

  const std::string cobalt_bin = env_or("DECKBACK_COBALT_BIN", "./content_shell");
  const std::string url = apply_deep_link(cfg->url, deep_link);

  // content_shell ignores --user-agent and takes the start URL positionally, so with CDP we boot on
  // about:blank and the Navigator injects the TV UA and navigates. Without a debugging port the
  // positional URL loads with the wrong UA (desktop redirect).
  const bool cdp_nav = cfg->remote_debugging_port > 0;
  std::vector<std::string> args;
  // The shell's data-dir switch is --data-path; it silently ignores --user-data-dir.
  args.push_back("--data-path=" + profile_dir);
  if (cdp_nav)
    args.push_back(std::format("--remote-debugging-port={}", cfg->remote_debugging_port));
  // Read by the Widevine registration patch. Passed only when a CDM is installed: a missing path
  // just logs an error on every launch. The CDM is user-fetched and hash-verified, never shipped.
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
  // Dev passthrough for engine flags not in the shipped config; whitespace-separated.
  if (const char* extra = std::getenv("DECKBACK_EXTRA_ARGS"); extra && *extra)
    for (std::string& a : split_whitespace(extra)) args.push_back(std::move(a));
  args.push_back(cdp_nav ? "about:blank" : url);

  // First-run controls card, drawn once the TV app has loaded. Declared BEFORE the navigator and
  // gamepad, whose threads borrow it, so it outlives both even on an early return.
  std::optional<OnboardingController> onboarding;
  // Self-update, also declared before the navigator and gamepad, which borrow update_prompt. The
  // updater owns its thread and is stopped first at shutdown. update_state is the hand-off from the
  // updater to the notify UI.
  UpdateState update_state;
  std::unique_ptr<Updater> updater;
  std::optional<UpdatePromptController> update_prompt;
  std::optional<OsdMenuController> osd;
  std::optional<Navigator> navigator;
  std::optional<GamepadInput> gamepad;
  // touch_gestures and disable_touch are mutually exclusive: no_pointer.js swallows the touches the
  // router reads, and they want opposite gamescope touch modes. Resolved once here; the lock wins.
  bool gestures_on = cfg->touch_gestures;
  if (gestures_on && cfg->disable_touch) {
    warn(
        "startup: touch_gestures and disable_touch are mutually exclusive (no_pointer.js would "
        "swallow the events the router reads). Keeping disable_touch; set it false to use "
        "gestures.");
    gestures_on = false;
  }

  if (cdp_nav && cfg->first_run_overlay) {
    onboarding.emplace("127.0.0.1", cfg->remote_debugging_port,
                       OverlayContext{cfg->keymap, cfg->right_stick_scroll, gestures_on},
                       first_run_marker_path(resolve_state_dir(runtime_dir)));
  }
  // In-app OSD Settings menu, whenever CDP is up. The update callbacks route to update_prompt,
  // which is emplaced below.
  if (cdp_nav) {
    // Checkpoint BEFORE the shutdown request. request_shutdown() is the only exit the watchdog
    // treats as success rather than a crash to restart. Runs on the input thread.
    const auto graceful_exit = [&player](const char* why) {
      if (player) player->on_suspend();
      info(why);
      Watchdog::request_shutdown();
    };
    osd.emplace(OsdMenuConfig{
        .cdp_host = "127.0.0.1",
        .cdp_port = cfg->remote_debugging_port,
        .local_version = kDeckbackVersion,
        .overlay = OverlayContext{cfg->keymap, cfg->right_stick_scroll, gestures_on},
        .about = parse_metainfo(load_metainfo().value_or("")),
        .captions = &captions,
        .on_update_confirm =
            [&update_prompt] {
              if (update_prompt) update_prompt->confirm_update();
            },
        .on_update_ignore =
            [&update_prompt] {
              if (update_prompt) update_prompt->ignore_version();
            },
        .on_update_check =
            [&update_prompt] {
              if (update_prompt) update_prompt->check_now();
            },
        .on_update_quit =
            [graceful_exit] {
              graceful_exit("exit: closing so the staged update applies on relaunch");
            },
        .on_exit = [graceful_exit] { graceful_exit("exit: user confirmed — shutting down"); }});
    // "Check for updates" is handled by the notify-mode prompt, so it is offered only there.
    osd->set_update_model(false, cfg->self_update_mode == SelfUpdateMode::Notify, false,
                          update_policy_status(cfg->self_update_mode), "");
  }

  // Self-update: auto deploys on detection; notify publishes availability to update_state and waits
  // for "Update now" via update_prompt.
  if (cfg->self_update_mode != SelfUpdateMode::Off) {
    UpdaterConfig uc;
    uc.auto_deploy = cfg->self_update_mode == SelfUpdateMode::Auto;
    uc.state = &update_state;
    uc.cdp_port = cdp_nav ? cfg->remote_debugging_port : 0;
    // Only when the notify prompt exists: a non-null on_terminal suppresses the updater's own
    // "restart to apply" toast. Runs on the updater's loop thread; the updater starts after
    // update_prompt is emplaced and stops first at shutdown, so the reference stays live.
    if (cfg->self_update_mode == SelfUpdateMode::Notify && cdp_nav) {
      uc.on_terminal = [&update_prompt](UpdateProgress p, std::string name, std::string msg) {
        if (update_prompt) update_prompt->on_deploy_result(p, name, msg);
      };
      uc.on_running = [&update_prompt](std::uint32_t op, std::uint32_t n_ops, std::uint32_t pct) {
        if (update_prompt) update_prompt->on_deploy_progress(op, n_ops, pct);
      };
    }
    updater = Updater::create(std::move(uc));
    info(std::format("startup: self-update mode = {}",
                     self_update_mode_name(cfg->self_update_mode)));
    if (cfg->self_update_mode == SelfUpdateMode::Notify && cdp_nav) {
      UpdatePromptConfig pc{.cdp_port = cfg->remote_debugging_port,
                            .state_dir = resolve_state_dir(runtime_dir),
                            .local_version = kDeckbackVersion,
                            .idle_status = update_policy_status(cfg->self_update_mode),
                            .state = &update_state,
                            .updater = updater.get(),
                            .osd = osd ? &*osd : nullptr};
      // Outside the sandbox there is no running commit, so the ref probe is off.
      pc.running_commit = read_running_commit();
      if (pc.running_commit.empty())
        info("update: no running commit (not sandboxed) — Check for updates uses the portal");
      pc.fetch_remote_ref = [url = env_or("DECKBACK_UPDATE_REF_URL", std::string(kUpdateRefUrl))] {
        return http_get(url, kUpdateRefTimeoutSec, "deckback-updater/1");
      };
      update_prompt.emplace(std::move(pc));
      // Test aid: DECKBACK_FAKE_UPDATE=<commit> publishes availability at launch. Confirming still
      // runs the real portal deploy.
      if (const char* fake = std::getenv("DECKBACK_FAKE_UPDATE"); fake && *fake) {
        update_state.set_available(true, fake);
        warn(std::string("startup: DECKBACK_FAKE_UPDATE set — simulating an available update (") +
             fake + ") for UI testing");
      }
    }
    updater->start();
  } else {
    info(
        "startup: self_update off — update via 'flatpak update'/Discover (durable/self-update.md)");
  }

  // Declared before the Navigator so the input layer gets a stable pointer; started after it so the
  // router does not poll a page that has no script in it yet.
  std::optional<GestureRouter> gestures;
  if (cdp_nav && gestures_on) {
    GestureRouterConfig gc;
    gc.cdp_port = cfg->remote_debugging_port;
    gc.poll_ms = cfg->touch_poll_ms;
    gc.step_px = cfg->touch_step_px;
    gc.max_steps = cfg->touch_max_steps;
    gc.edge_px = cfg->touch_edge_px;
    gc.long_press_ms = cfg->touch_long_press_ms;
    gc.double_tap_ms = cfg->touch_double_tap_ms;
    gc.skip_seconds = cfg->skip_seconds;
    gc.hold_rate = cfg->touch_hold_rate;
    gc.hold_slow_rate = cfg->touch_hold_slow_rate;
    gc.modal = [&osd, &onboarding] {
      return (osd && osd->open()) || (onboarding && onboarding->visible());
    };
    gestures.emplace(std::move(gc));
  }

  if (cdp_nav) {
    NavPolicy nav_policy{cfg->steer_av1,          cfg->mic_autogrant,      cfg->error_page,
                         cfg->error_retry_min_ms, cfg->error_retry_max_ms, cfg->error_title,
                         cfg->error_hint,         cfg->disable_touch};
    nav_policy.touch_gestures = gestures_on;
    nav_policy.hide_cursor = gestures_on && cfg->touch_hide_cursor;
    navigator.emplace("127.0.0.1", cfg->remote_debugging_port, cfg->user_agent, url,
                      cfg->devtools_poll_ms, nav_policy);
    if (onboarding || osd)
      navigator->set_on_app_loaded([&onboarding, &osd] {
        if (onboarding) onboarding->show(/*first_run_only=*/true);
        // After a full reload, redraw the Settings button and re-inject the menu if open, or the
        // menu is stranded and traps input.
        if (osd) osd->on_page_reloaded();
      });
    navigator->start();
    // Gamepad evdev -> DOM key events over CDP, bindings from the config keymaps. `layers` is only
    // live when `player` exists.
    GamepadOptions gp;
    gp.keymap = KeymapConfig{cfg->keymap, cfg->keymap_player, cfg->keymap_osk, cfg->keymap_lt,
                             cfg->keymap_rt};
    gp.fast_scroll = FastScrollConfig{cfg->right_stick_scroll, cfg->right_stick_deadzone,
                                      cfg->right_stick_slow_ms, cfg->right_stick_fast_ms};
    gp.skip_seconds = cfg->skip_seconds;
    gp.captions = &captions;
    gp.layers = player ? &layers : nullptr;
    gp.onboarding = onboarding ? &*onboarding : nullptr;
    gp.update_prompt = update_prompt ? &*update_prompt : nullptr;
    gp.osd = osd ? &*osd : nullptr;
    gp.gestures = gestures ? &*gestures : nullptr;
    gamepad.emplace("127.0.0.1", cfg->remote_debugging_port, std::move(gp));
    gamepad->start();
    if (gestures) {
      gestures->start();
      info(std::format(
          "startup: touch gestures on — drag/flick scrolls, left-edge swipe = Back, double-tap "
          "seeks +/-{}s, hold = 2x (poll {} ms)",
          cfg->skip_seconds, cfg->touch_poll_ms));
    }
  } else {
    warn(
        "startup: remote_debugging_port unset — cannot inject the TV UA over CDP; content_shell "
        "will load with its default UA (desktop redirect likely)");
  }

  // disable_touch: hold gamescope's touch mode at hover while our window is focused, so a finger
  // cannot generate a click at the compositor. Unlike the page-side swallow, this needs no CDP.
  std::optional<TouchModeGuard> touch_mode;
  if (cfg->disable_touch) {
    touch_mode.emplace();
    touch_mode->start();
  } else if (gestures_on) {
    // Mode 4 (passthrough) is the only one that delivers real wl_touch, which the router reads.
    touch_mode.emplace(750, TouchMode::kPassthrough);
    touch_mode->start();
  }

  info(std::format("startup: launching {} -> {}", cobalt_bin, url));
  AudioRepair audio;
  audio.start();
  Watchdog wd(cobalt_bin, std::move(args), *cfg);
  int rc = wd.run();

  // Shut down in dependency order: stop the input + navigator + poll threads (they call into the
  // engine/platform), then the logind backend (its callbacks call into player), then flush logs.
  if (updater) updater->stop();
  if (touch_mode) touch_mode->stop();
  if (gamepad) gamepad->stop();
  if (navigator) navigator->stop();
  audio.stop();
  if (player) player->stop();
  platform.reset();
  log_shutdown();
  return rc;
}

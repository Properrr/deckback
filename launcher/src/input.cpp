#include "input.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>

#include "caption_settings.hpp"
#include "log.hpp"
#include "osdmenu.hpp"
#include "overlay.hpp"
#include "scripts.hpp"
#include "updateprompt.hpp"
#include "util.hpp"

namespace deckback {
namespace {

constexpr long kInitialDelayMs = 350;  // hold-to-repeat: delay before the first auto-repeat

// Exit hold completed. A single firm pulse, distinct from the touch-lock pair: it is the only
// confirmation the hold took, and the app is about to disappear.
constexpr uint16_t kExitRumbleStrong = 0xE000;
constexpr uint16_t kExitRumbleWeak = 0x6000;
constexpr uint16_t kExitRumbleMs = 220;
constexpr long kRepeatMs = 130;    // base repeat interval
constexpr long kMinRepeatMs = 55;  // fastest interval after acceleration
constexpr int kStickDeadzone =
    13000;  // |axis| beyond this counts as a direction (Xbox range ~32767)

// How often to look for newly-attached pads while others are already open (hotplug).
constexpr long kHotplugScanMs = 2000;

// A device is a gamepad if it advertises the standard South face button (a.k.a. BTN_GAMEPAD).
bool is_gamepad(int fd) {
  unsigned long keys[(KEY_MAX / (8 * sizeof(long))) + 1] = {};
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) return false;
  return test_bit(BTN_SOUTH, keys);
}

std::string device_name(int fd) {
  char name[256] = {};
  if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) return "?";
  return name;
}

}  // namespace

GamepadInput::GamepadInput(std::string host, int port, GamepadOptions opts)
    : client_(std::move(host), port),
      block_initial_(opts.touch.block_initial),
      chord_(opts.touch.unlock_hold_ms),
      chord_label_(opts.touch.chord),
      touch_toast_(opts.touch.toast),
      touch_haptic_(opts.touch.haptic),
      layers_(opts.layers),
      captions_(opts.captions),
      onboarding_(opts.onboarding),
      update_prompt_(opts.update_prompt),
      osd_(opts.osd),
      fast_cfg_(opts.fast_scroll) {
  KeymapConfig& keymap = opts.keymap;
  // Menu is the OSD's fixed physical entry point, not a `show_controls` keymap action. The OSD
  // must remain reachable after a hot-swapped keymap removes or repurposes that legacy action.
  // Drop the old action anywhere it occurs so it can neither dispatch nor be reported as unmapped.
  if (osd_) {
    menu_code_ = control_code("start");
    info(std::format("input: Menu (code {}) opens the settings menu", menu_code_));
    keymap.base = without_action(keymap.base, "show_controls");
  }
  if (osd_) {
    osd_a_ = control_code("a");
    osd_b_ = control_code("b");
    osd_x_ = control_code("x");
    osd_y_ = control_code("y");
    osd_lb_ = control_code("lb");
    osd_rb_ = control_code("rb");
  }
  if (opts.touch.lock_enabled) {
    const auto [a, b] = parse_chord(opts.touch.chord);
    chord_a_code_ = a;
    chord_b_code_ = b;
    if (chord_a_code_ < 0)
      warn(std::format("input: touch_lock_chord '{}' unrecognised — chord lock disabled",
                       opts.touch.chord));
    else
      info(std::format("input: touch lock chord = {} (codes {}+{}), unlock hold {} ms",
                       opts.touch.chord, chord_a_code_, chord_b_code_, opts.touch.unlock_hold_ms));
  }

  std::vector<std::string> unmapped;
  maps_ = build_keymaps(keymap, &unmapped);

  // The triggers keep their own binding only while they are not modifiers.
  for (const auto& [name, value] : keymap.base) {
    if (name != "lt" && name != "rt") continue;
    const bool is_mod = (name == "lt") ? !maps_.lt_mod.empty() : !maps_.rt_mod.empty();
    if (is_mod) continue;
    // A seek action is not a DOM key: prebuild its eval expression from config/scripts/
    // (ScriptLibrary, hot-swappable) so the press edge is a single eval_void (input-ux §18).
    // Checked before resolve_binding so it never falls through to "no DOM key". Two kinds:
    //   skip_back/skip_fwd       -> skip.js: a fixed ±skip_seconds seek.
    //   chapter_back/chapter_fwd -> chapter_seek.js: jump to the prev/next chapter boundary,
    //                               falling back to a fixed skip when the video has no chapters.
    if (int sign = skip_action_sign(value); sign != 0) {
      (name == "lt" ? lt_skip_js_ : rt_skip_js_) = ScriptLibrary::instance().render(
          "skip", ScriptParams().set("delta", sign * opts.skip_seconds));
      info(std::format("input: bind {} -> skip {}{}s", name, sign < 0 ? "-" : "+",
                       opts.skip_seconds));
      continue;
    }
    if (int dir = chapter_action_sign(value); dir != 0) {
      (name == "lt" ? lt_skip_js_ : rt_skip_js_) = ScriptLibrary::instance().render(
          "chapter_seek", ScriptParams().set("dir", dir).set("skip", opts.skip_seconds));
      info(std::format("input: bind {} -> chapter {} (fallback skip {}s)", name,
                       dir < 0 ? "prev" : "next", opts.skip_seconds));
      continue;
    }
    std::string key = resolve_binding(value);
    if (key.empty())
      unmapped.push_back(std::format("keymap.{}={} (no DOM key)", name, value));
    else
      (name == "lt" ? lt_key_ : rt_key_) = std::move(key);
  }

  captions_code_ = find_control_for_action(keymap.base, "toggle_captions");
  if (captions_code_ < 0) captions_code_ = find_control_for_action(keymap.base, "captions");
  if (captions_code_ >= 0 && !captions_) {
    warn("input: captions bound but no CaptionSettings — toggle disabled");
    captions_code_ = -1;
  } else if (captions_code_ >= 0) {
    info(std::format("input: bind captions (code {}) -> player caption module", captions_code_));
  }

  for (const ButtonBinding& b : maps_.base.buttons)
    info(std::format("input: bind {} -> {}", b.code, b.key));
  if (!lt_key_.empty()) info(std::format("input: bind lt -> {}", lt_key_));
  if (!rt_key_.empty()) info(std::format("input: bind rt -> {}", rt_key_));
  if (!maps_.lt_mod.empty()) info("input: lt is a held modifier layer (keymap_lt)");
  if (!maps_.rt_mod.empty()) info("input: rt is a held modifier layer (keymap_rt)");
  if (fast_cfg_.enabled)
    info(std::format("input: right stick = fast scroll ({}..{} ms, deadzone {})", fast_cfg_.slow_ms,
                     fast_cfg_.fast_ms, fast_cfg_.deadzone));
  if (!maps_.player.empty()) info("input: context layer 'player' active (keymap_player)");
  if (!maps_.osk.empty()) info("input: context layer 'osk' active (keymap_osk)");
  if (!layers_ && (!maps_.player.empty() || !maps_.osk.empty()))
    warn("input: context layers configured but no play-state source — they will never activate");

  for (const auto* section :
       {&keymap.base, &keymap.player, &keymap.osk, &keymap.lt_mod, &keymap.rt_mod}) {
    for (const auto& [name, value] : *section) {
      const std::string_view repl = deprecated_action_replacement(value);
      if (!repl.empty())
        warn(std::format("input: keymap {}='{}' is deprecated and will be removed — use '{}'", name,
                         value, repl));
    }
  }
  if (!unmapped.empty()) {
    // One warning at startup beats a silent no-op on every press.
    std::string list;
    for (const std::string& u : unmapped) list += (list.empty() ? "" : ", ") + u;
    warn(std::format("input: {} keymap entries unmapped and will do nothing: {}", unmapped.size(),
                     list));
  }
}

Layer GamepadInput::layer() const { return layers_ ? layers_->get() : Layer::Browse; }

GamepadInput::~GamepadInput() { stop(); }

void GamepadInput::start() {
  worker_.start([this] { loop(); });
}

void GamepadInput::stop() { worker_.stop(); }

void GamepadInput::close_devices() {
  for (int fd : fds_)
    if (fd >= 0) close(fd);
  fds_.clear();
  paths_.clear();
}

void GamepadInput::discover_new_devices() {
  DIR* d = opendir("/dev/input");
  if (!d) return;
  for (dirent* e; (e = readdir(d)) != nullptr;) {
    if (std::strncmp(e->d_name, "event", 5) != 0) continue;
    std::string path = std::string("/dev/input/") + e->d_name;
    bool already_open = false;
    for (const std::string& p : paths_)
      if (p == path) already_open = true;
    if (already_open) continue;
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    if (is_gamepad(fd)) {
      info(std::format("input: gamepad {} ({})", path, device_name(fd)));
      fds_.push_back(fd);
      paths_.push_back(std::move(path));
    } else {
      close(fd);
    }
  }
  closedir(d);
}

void GamepadInput::rescan_devices() {
  close_devices();
  discover_new_devices();  // paths_ is empty, so this opens everything afresh
  if (fds_.empty()) info("input: no gamepad device found yet (will retry)");
}

namespace {
const char* arrow_focus_cmd(const char* arrow) {
  std::string_view a(arrow ? arrow : "");
  if (a == "ArrowUp") return "up";
  if (a == "ArrowDown") return "down";
  if (a == "ArrowLeft") return "left";
  if (a == "ArrowRight") return "right";
  return nullptr;
}
}  // namespace

void GamepadInput::dispatch_key(const char* base_arrow) {
  // While the OSD is open, D-pad/left-stick directions move menu focus, not Leanback's.
  if (osd_ && osd_->open()) {
    if (const char* c = arrow_focus_cmd(base_arrow)) osd_->exec(c);
    return;
  }
  // Resolved per dispatch, not once per press: releasing a modifier or opening the player mid-hold
  // takes effect on the next repeat without the user re-pressing the stick.
  const std::string key = resolve_direction_key(maps_, base_arrow, layer(), lt_down_, rt_down_);
  if (key.empty()) return;  // absorbed by a held modifier layer that does not bind this direction
  client_.dispatch_key(key);
}

void GamepadInput::dispatch_scroll(const char* base_arrow) {
  // Right stick: scrolls the focused OSD region when the menu is open, else the normal fast-scroll.
  if (osd_ && osd_->open()) {
    std::string_view a(base_arrow ? base_arrow : "");
    if (a == "ArrowUp")
      osd_->exec("scroll_up");
    else if (a == "ArrowDown")
      osd_->exec("scroll_down");
    return;
  }
  dispatch_key(base_arrow);
}

void GamepadInput::dispatch_direction() {
  if (!dir_key_) return;
  dispatch_key(dir_key_);
}

void GamepadInput::set_direction(const char* key) {
  if (key != dir_key_) {  // pointer identity: same literal constant
    dir_key_ = key;
    repeat_count_ = 0;
    if (key) {
      info(std::format("input: {}", key));
      dispatch_direction();  // immediate press
      next_repeat_ms_ = mono_ms() + kInitialDelayMs;
    }
  }
  // Unconditional, even when the direction did not change: the D-pad/left stick own the direction
  // while they hold one, and clearing it (a release, or a lost device) must hand the right stick
  // back. Cheap — update_fast_scroll() no-ops unless the resolved direction moved.
  update_fast_scroll();
}

void GamepadInput::update_fast_scroll() {
  // A held D-pad/left-stick direction suppresses the right stick entirely — and so does the modal
  // first-run card, which would otherwise scroll Leanback's focus behind itself. The OSD is NOT
  // blocked here: while it is open the right stick scrolls the menu (dispatch_scroll routes it).
  const bool blocked = dir_key_ || (onboarding_ && onboarding_->visible());
  const FastScrollTick t =
      blocked ? FastScrollTick{nullptr, 0} : fast_scroll(rstick_x_, rstick_y_, fast_cfg_);
  if (t.key == fast_key_) return;  // pointer identity: same direction, only the rate moved
  fast_key_ = t.key;
  if (!fast_key_) return;
  dispatch_scroll(fast_key_);  // the first step lands on the push, not after a delay
  fast_next_ms_ = mono_ms() + t.interval_ms;
}

bool GamepadInput::handle_chord(int code, int value) {
  if (chord_a_code_ < 0) return false;  // chord disabled
  if (code != chord_a_code_ && code != chord_b_code_) return false;
  const bool down = (value != 0);  // press or kernel autorepeat = held; release = up
  if (code == chord_a_code_)
    chord_a_down_ = down;
  else
    chord_b_down_ = down;
  apply_touch_lock(chord_.on_chord(chord_a_down_ && chord_b_down_, mono_ms()));
  return true;  // the chord buttons are ours; don't also treat them as mapped buttons
}

void GamepadInput::apply_touch_lock(TouchLockChord::Action a) {
  if (a == TouchLockChord::Action::None) return;
  const bool want = (a == TouchLockChord::Action::Lock);
  if (!touch_.set_blocked(want)) {
    // The grab failed (TouchGuard already warned). Do not let the machine believe it succeeded, and
    // do not tell the user the touchscreen is locked when it is not.
    chord_.set_locked(!want);
    return;
  }
  info(std::format("input: touchscreen {} (chord)", want ? "LOCKED" : "unlocked"));
  announce_touch_lock(want);
}

void GamepadInput::announce_touch_lock(bool locked) {
  // The lock is otherwise *invisible*: a locked screen and a hung screen look identical, and that
  // ambiguity is the bug report this exists to prevent (findings input-ux §4).
  if (touch_toast_) {
    show_toast(client_,
               locked ? std::format("Touchscreen locked\nHold {} to unlock", chord_label_)
                      : "Touchscreen unlocked",
               locked ? 2600 : 1600);
  }
  if (!touch_haptic_) return;
  ensure_haptic();
  // Two magnitudes, not one: locking and unlocking must feel different in the pocket, since the
  // user may trip the chord without looking at the screen.
  if (locked)
    haptic_.rumble(0xC000, 0x4000, 180);
  else
    haptic_.rumble(0x3000, 0x8000, 90);
}

void GamepadInput::ensure_haptic() {
  if (haptic_tried_) return;
  // Deferred to the first pulse: at construction no pad has been discovered yet. Once only — a pad
  // without FF must not re-open a device on every toggle.
  haptic_tried_ = true;
  for (const std::string& p : paths_)
    if (haptic_.attach(p)) break;
  if (!haptic_.attached()) info("input: no force-feedback device — haptics unavailable");
}

void GamepadInput::toggle_captions() {
  if (!captions_) return;
  const std::string r = client_.eval_string(captions_->toggle_js()).value_or("na");
  info(std::format("input: captions toggle -> {}", r));
  caption_apply_pending_ = false;

  std::string on_lang;
  bool off = false, none = false;
  if (r.rfind("on:", 0) == 0)
    on_lang = r.substr(3);
  else if (r == "off")
    off = true;
  else if (r == "none")
    none = true;
  if (!on_lang.empty())
    captions_->set_on(true);
  else if (off)
    captions_->set_on(false);
  if (!captions_->toast_enabled()) return;
  if (!on_lang.empty())
    show_toast(client_, "Captions: " + language_name(on_lang), 1400);
  else if (off)
    show_toast(client_, "Captions off", 1400);
  else if (none)
    show_toast(client_, "No captions for this video", 1800);
}

void GamepadInput::tick_caption_apply(bool on_watch) {
  constexpr int kMaxTicks = 40;
  constexpr long kCaptionTickMs = 300;
  constexpr int kRelaxTicks = 6;
  if (!captions_ || !captions_->local_mode()) {
    prev_video_up_ = on_watch;
    caption_apply_pending_ = false;
    return;
  }
  if (on_watch && !prev_video_up_) {
    caption_apply_pending_ = true;
    caption_apply_ticks_ = 0;
    caption_next_ms_ = mono_ms();
    caption_apply_last_.clear();
  }
  if (!on_watch) caption_apply_pending_ = false;
  prev_video_up_ = on_watch;

  if (!caption_apply_pending_) return;
  if (mono_ms() < caption_next_ms_) return;
  caption_next_ms_ = mono_ms() + kCaptionTickMs;

  const bool relax = caption_apply_ticks_ >= kMaxTicks - kRelaxTicks;
  const std::string r = client_.eval_string(captions_->apply_js(relax)).value_or("na");
  captions_->note_apply(r);
  ++caption_apply_ticks_;
  if (r != "wait" && r != "na" && r != "pending" && r != caption_apply_last_) {
    info(std::format("input: captions apply -> {}", r));
    caption_apply_last_ = r;
  }
  if (r == "none" || caption_apply_ticks_ >= kMaxTicks) caption_apply_pending_ = false;
}

void GamepadInput::osd_event(int type, int code, int value) {
  if (type == EV_KEY) {
    if (value == 1) {  // press edge; fixed nav buttons
      if (code == osd_a_ && osd_a_ >= 0)
        osd_->exec("select");
      else if ((code == osd_b_ && osd_b_ >= 0) || (code == menu_code_ && menu_code_ >= 0))
        osd_->exec("back");
      else if (code == osd_x_ && osd_x_ >= 0)
        osd_->exec("delete");
      else if (code == osd_y_ && osd_y_ >= 0) {
        // Y is tap-vs-hold: hold exits, tap keeps its existing "ignore this update" meaning (which
        // is consumed with no effect anywhere else). Exit is a GLOBAL gesture, deliberately not a
        // focusable control — in the Keys sub-tab focus cannot leave `subsel`, so a focusable Exit
        // is unreachable there. The tap fires from the release edge below.
        if (parse_verdict(osd_->exec("hold_start")).kind == OsdVerdict::Kind::Hold)
          exit_hold_deadline_ = mono_ms() + osd_->exit_hold_ms();
      } else if (code == osd_lb_ && osd_lb_ >= 0)
        osd_->exec("tab_prev");
      else if (code == osd_rb_ && osd_rb_ >= 0)
        osd_->exec("tab_next");
    }
    // Modal capture must not freeze physical state machines that started before the menu opened:
    // swallowing a trigger release leaves its next press dead or its modifier layer stuck. Presses
    // still stay modal — only release bookkeeping is allowed through.
    if (value == 0) {
      if (code == osd_y_ && osd_y_ >= 0 && exit_hold_deadline_ != 0) {
        exit_hold_deadline_ = 0;  // released before the hold completed — no exit
        osd_->exec("hold_cancel");
        osd_->exec("ignore");  // ...so Y still reads as a tap
      }
      handle_chord(code, value);
    }
    return;  // swallow every button while the menu is up
  }
  if (type != EV_ABS) return;
  switch (code) {
    case ABS_HAT0X:
      hat_x_ = (value < 0) ? -1 : (value > 0 ? 1 : 0);
      break;
    case ABS_HAT0Y:
      hat_y_ = (value < 0) ? -1 : (value > 0 ? 1 : 0);
      break;
    case ABS_X:
      stick_x_ = (value < -kStickDeadzone) ? -1 : (value > kStickDeadzone ? 1 : 0);
      break;
    case ABS_Y:
      stick_y_ = (value < -kStickDeadzone) ? -1 : (value > kStickDeadzone ? 1 : 0);
      break;
    case ABS_RX:
      rstick_x_ = value;
      update_fast_scroll();
      return;
    case ABS_RY:
      rstick_y_ = value;
      update_fast_scroll();
      return;
    case ABS_Z:
      // Keep the physical trigger latch honest without firing a seek under the modal menu.
      lt_down_ = trigger_pressed(value, lt_down_);
      return;
    case ABS_RZ:
      rt_down_ = trigger_pressed(value, rt_down_);
      return;
    default:
      return;  // triggers etc. do nothing while the menu is open
  }
  // dispatch_key / dispatch_scroll are OSD-aware, so this drives menu focus + scroll with the same
  // auto-repeat machinery Leanback navigation uses.
  set_direction(resolve_direction(hat_x_, hat_y_, stick_x_, stick_y_));
}

bool GamepadInput::osd_open_edge(int type, int code, int value) {
  if (!osd_ || osd_->open()) return false;
  if (type == EV_KEY && value == 1 && code == menu_code_ && menu_code_ >= 0) {
    // Menu opens the OSD during playback too. It has nothing else to do there: `show_controls` is
    // stripped from the keymap in the ctor and resolves to no DOM key, so Menu was simply dead on
    // the watch screen — and mid-video is exactly when Exit and the settings are wanted. The
    // on-screen Settings button stays hidden on watch (tick(): want = !on_watch); only the physical
    // button reaches the menu there.
    osd_->open_menu();
    return true;
  }
  return false;
}

bool GamepadInput::handle_onboarding(int type, int /*code*/, int value) {
  if (!onboarding_ || !onboarding_->visible()) return false;
  // Modal first-run card: any button press dismisses it; sticks/D-pad are swallowed (a resting
  // thumb must not dismiss it, and the D-pad must not move focus behind it).
  if (type == EV_KEY && value == 1) onboarding_->hide();
  return true;
}

void GamepadInput::handle_event(int type, int code, int value) {
  if (osd_ && osd_->open()) {
    osd_event(type, code, value);
    return;
  }
  if (handle_onboarding(type, code, value)) return;
  if (osd_open_edge(type, code, value)) return;
  if (type == EV_KEY) {
    if (handle_chord(code, value)) return;
    if (value != 1) return;  // press edge only (ignore release=0 and kernel autorepeat=2)
    if (code == captions_code_ && captions_code_ >= 0) {
      toggle_captions();
      return;
    }
    const std::string key = resolve_button(maps_, code, layer(), lt_down_, rt_down_);
    if (key.empty()) return;  // unbound, or absorbed by a held modifier layer
    info(std::format("input: btn {} -> {} [{}]", code, key, layer_name(layer())));
    client_.dispatch_key(key);
    return;
  }
  if (type != EV_ABS) return;
  switch (code) {
    case ABS_HAT0X:
      hat_x_ = (value < 0) ? -1 : (value > 0 ? 1 : 0);
      break;
    case ABS_HAT0Y:
      hat_y_ = (value < 0) ? -1 : (value > 0 ? 1 : 0);
      break;
    case ABS_X:
      stick_x_ = (value < -kStickDeadzone) ? -1 : (value > kStickDeadzone ? 1 : 0);
      break;
    case ABS_Y:
      stick_y_ = (value < -kStickDeadzone) ? -1 : (value > kStickDeadzone ? 1 : 0);
      break;
    // Right stick: kept raw. The deflection is the repeat rate (input-ux §7).
    case ABS_RX:
      rstick_x_ = value;
      update_fast_scroll();
      return;
    case ABS_RY:
      rstick_y_ = value;
      update_fast_scroll();
      return;
    // Analog triggers. When a modifier layer is configured the trigger dispatches nothing itself —
    // holding it only changes what the *other* controls mean (lt_key_/rt_key_ stay empty in that
    // case, so the press-edge dispatch below is naturally skipped).
    case ABS_Z: {
      const bool now = trigger_pressed(value, lt_down_);
      if (now && !lt_down_) {
        if (!lt_skip_js_.empty()) {
          info("input: lt -> skip");
          client_.eval_void(lt_skip_js_);
        } else if (!lt_key_.empty()) {
          info(std::format("input: lt -> {}", lt_key_));
          client_.dispatch_key(lt_key_);
        }
      }
      lt_down_ = now;
      return;
    }
    case ABS_RZ: {
      const bool now = trigger_pressed(value, rt_down_);
      if (now && !rt_down_) {
        if (!rt_skip_js_.empty()) {
          info("input: rt -> skip");
          client_.eval_void(rt_skip_js_);
        } else if (!rt_key_.empty()) {
          info(std::format("input: rt -> {}", rt_key_));
          client_.dispatch_key(rt_key_);
        }
      }
      rt_down_ = now;
      return;
    }
    default:
      return;
  }
  set_direction(resolve_direction(hat_x_, hat_y_, stick_x_, stick_y_));
}

void GamepadInput::loop() {
  if (block_initial_) {
    chord_.set_locked(touch_.set_blocked(true));
    if (chord_.locked()) info("input: touchscreen starts LOCKED (config block_touchscreen)");
  }
  rescan_devices();
  long last_scan = mono_ms();

  while (!worker_.stopping()) {
    // Poll timeout: the earliest pending deadline, a rescan tick when idle, else a periodic wake.
    long timeout = 1000;
    if (fds_.empty() && !dir_key_) timeout = 2000;  // retry device discovery
    auto consider = [&](long deadline) {
      long d = deadline - mono_ms();
      if (d < 0) d = 0;
      if (d < timeout) timeout = d;
    };
    // Directional auto-repeat, then the timers that mature with no further evdev event coming: the
    // right stick held at a constant deflection, and the deliberate-unlock chord hold.
    if (dir_key_) consider(next_repeat_ms_);
    if (fast_key_) consider(fast_next_ms_);
    if (chord_.pending()) consider(chord_.deadline_ms());
    if (caption_apply_pending_) consider(caption_next_ms_);
    if (exit_hold_deadline_ != 0) consider(exit_hold_deadline_);

    std::vector<pollfd> pfds;
    pfds.reserve(fds_.size());
    for (int fd : fds_) pfds.push_back(pollfd{fd, POLLIN, 0});
    int n = pfds.empty() ? 0 : poll(pfds.data(), pfds.size(), static_cast<int>(timeout));
    if (n < 0 && errno != EINTR) { /* transient */
    }

    bool lost = false;
    for (size_t i = 0; i < pfds.size(); ++i) {
      if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
      input_event evs[32];
      ssize_t r = read(pfds[i].fd, evs, sizeof(evs));
      if (r <= 0) {
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        lost = true;  // device went away
        continue;
      }
      const size_t count = static_cast<size_t>(r) / sizeof(input_event);
      for (size_t k = 0; k < count; ++k) handle_event(evs[k].type, evs[k].code, evs[k].value);
    }

    // Feed the OSD update state, then reconcile the Settings button (drawn off playback). Cheap on
    // every tick (atomic reads + change-gated CDP). The raw watch signal is video_up, not
    // `layer() == Layer::Player`, because the OSK outranks the player while a video plays
    // underneath.
    const bool on_watch = layers_ && layers_->video_up();
    if (update_prompt_) update_prompt_->tick(on_watch);
    if (osd_) osd_->tick(on_watch);
    tick_caption_apply(on_watch);

    // The first-run card is modal; a direction held when it appeared would keep auto-repeating into
    // the page behind it. Not the OSD: while it is open a held direction drives the menu.
    if (onboarding_ && onboarding_->visible()) set_direction(nullptr);

    if (chord_.pending()) apply_touch_lock(chord_.on_tick(mono_ms()));

    // Hold-to-confirm completed on the OSD's Exit row. Rumble first: it is the only feedback that
    // the hold took, and the app is about to go away.
    if (exit_hold_deadline_ != 0 && mono_ms() >= exit_hold_deadline_) {
      exit_hold_deadline_ = 0;
      ensure_haptic();
      haptic_.rumble(kExitRumbleStrong, kExitRumbleWeak, kExitRumbleMs);
      if (osd_) osd_->fire_exit();
    }

    // Directional auto-repeat with acceleration.
    if (dir_key_ && mono_ms() >= next_repeat_ms_) {
      dispatch_direction();
      long interval = kRepeatMs - static_cast<long>(repeat_count_) * 15;
      if (interval < kMinRepeatMs) interval = kMinRepeatMs;
      ++repeat_count_;
      next_repeat_ms_ = mono_ms() + interval;
    }

    // Right-stick repeat. The interval is re-read from the *current* deflection every step, so
    // easing off the stick slows the scroll on the next arrow rather than at the next event.
    if (fast_key_ && mono_ms() >= fast_next_ms_) {
      const FastScrollTick t = fast_scroll(rstick_x_, rstick_y_, fast_cfg_);
      if (t.key != fast_key_) {
        update_fast_scroll();  // the stick crossed into another direction between polls
      } else {
        dispatch_scroll(fast_key_);
        fast_next_ms_ = mono_ms() + t.interval_ms;
      }
    }

    if (lost || (fds_.empty() && mono_ms() - last_scan >= 2000)) {
      rescan_devices();
      last_scan = mono_ms();
      hat_x_ = hat_y_ = stick_x_ = stick_y_ = 0;
      rstick_x_ = rstick_y_ = 0;
      set_direction(nullptr);  // drop any held direction (and the right-stick scroll) across a
                               // device change: with the pad gone, no release event is coming
    } else if (mono_ms() - last_scan >= kHotplugScanMs) {
      // Hotplug: a pad attached while another is already open would otherwise never be seen,
      // because neither `lost` nor `fds_.empty()` ever becomes true. Non-destructive — held keys
      // survive.
      discover_new_devices();
      last_scan = mono_ms();
    }
  }
  close_devices();
  haptic_.detach();
  if (chord_.locked()) touch_.set_blocked(false);  // never leave the touchscreen grabbed on exit
}

}  // namespace deckback

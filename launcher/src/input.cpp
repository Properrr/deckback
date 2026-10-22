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
#include "gestures.hpp"
#include "log.hpp"
#include "osdmenu.hpp"
#include "overlay.hpp"
#include "scripts.hpp"
#include "updateprompt.hpp"
#include "util.hpp"

namespace deckback {
namespace {

constexpr long kInitialDelayMs = 350;  // hold-to-repeat: delay before the first auto-repeat

// Rumble for a completed Exit hold (also reused to confirm the gesture toggle).
constexpr uint16_t kExitRumbleStrong = 0xE000;
constexpr uint16_t kExitRumbleWeak = 0x6000;
constexpr uint16_t kExitRumbleMs = 220;
constexpr long kRepeatMs = 130;    // base repeat interval
constexpr long kMinRepeatMs = 55;  // fastest interval after acceleration

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

// The analog axes whose range matters, in GamepadInput::ranges_ order. -1 = read by sign or unused.
constexpr int kRangedAxes[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ};
int axis_slot(int code) {
  for (int i = 0; i < 6; ++i)
    if (kRangedAxes[i] == code) return i;
  return -1;
}

std::array<AxisRange, 6> axis_ranges(int fd) {
  std::array<AxisRange, 6> out{};
  for (int i = 0; i < 6; ++i) {
    input_absinfo ai{};
    if (ioctl(fd, EVIOCGABS(kRangedAxes[i]), &ai) == 0) out[i] = {ai.minimum, ai.maximum};
  }
  return out;
}

}  // namespace

GamepadInput::GamepadInput(std::string host, int port, GamepadOptions opts)
    : client_(std::move(host), port),
      layers_(opts.layers),
      captions_(opts.captions),
      gestures_(opts.gestures),
      onboarding_(opts.onboarding),
      update_prompt_(opts.update_prompt),
      osd_(opts.osd),
      fast_cfg_(opts.fast_scroll) {
  KeymapConfig& keymap = opts.keymap;
  // Menu is the OSD's fixed physical entry point; drop the legacy `show_controls` action so it can
  // neither dispatch nor be reported as unmapped.
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
  std::vector<std::string> unmapped;
  maps_ = build_keymaps(keymap, &unmapped);

  // The triggers keep their own binding only while they are not modifiers.
  for (const auto& [name, value] : keymap.base) {
    if (name != "lt" && name != "rt") continue;
    const bool is_mod = (name == "lt") ? !maps_.lt_mod.empty() : !maps_.rt_mod.empty();
    if (is_mod) continue;
    // A seek action is not a DOM key: prebuild its eval expression from config/scripts/ so the
    // press edge is a single eval_void. Checked before resolve_binding.
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

  gestures_code_ = find_control_for(keymap.base, gestures_action);
  if (gestures_code_ >= 0 && !gestures_) {
    // Bound but with no router: the normal state while touch_gestures is off.
    info("input: touch-gesture toggle bound but gestures are off (config touch_gestures) — inert");
    gestures_code_ = -1;
  } else if (gestures_code_ >= 0) {
    info(std::format("input: bind touch gestures (code {}) -> router on/off", gestures_code_));
  }

  captions_code_ = find_control_for(keymap.base, captions_action);
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
  ranges_.clear();
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
      const std::array<AxisRange, 6> r = axis_ranges(fd);
      info(std::format("input: gamepad {} ({}; stick {}..{}, trigger {}..{})", path,
                       device_name(fd), r[0].min, r[0].max, r[4].min, r[4].max));
      fds_.push_back(fd);
      paths_.push_back(std::move(path));
      ranges_.push_back(r);
    } else {
      close(fd);
    }
  }
  closedir(d);
}

void GamepadInput::close_device(size_t i) {
  info(std::format("input: gamepad {} removed", paths_[i]));
  if (fds_[i] >= 0) close(fds_[i]);
  fds_.erase(fds_.begin() + static_cast<long>(i));
  paths_.erase(paths_.begin() + static_cast<long>(i));
  ranges_.erase(ranges_.begin() + static_cast<long>(i));
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
  // Resolved per dispatch, not once per press, so a modifier or layer change mid-hold applies on
  // the next repeat.
  const std::string key = resolve_direction_key(maps_, base_arrow, layer(), axes_.lt, axes_.rt);
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

// Forget every held control, for when a release cannot be trusted to arrive: the pad went away, or
// the kernel dropped queued events. A still-held control resumes on its next press.
void GamepadInput::drop_held_state() {
  axes_ = AxisState{};
  if (exit_hold_deadline_ != 0) {  // a lost Y release must not complete the hold and exit
    exit_hold_deadline_ = 0;
    if (osd_ && osd_->open()) osd_->exec("hold_cancel");
  }
  set_direction(nullptr);  // also hands back / stops the right-stick scroll
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
  // Unconditional: clearing the direction must hand the right stick back. update_fast_scroll()
  // no-ops unless the resolved direction moved.
  update_fast_scroll();
}

void GamepadInput::update_fast_scroll() {
  // A held D-pad/left-stick direction or the modal first-run card suppresses the right stick. The
  // OSD does not: while it is open the right stick scrolls the menu (dispatch_scroll routes it).
  const bool blocked = dir_key_ || (onboarding_ && onboarding_->visible());
  const FastScrollTick t =
      blocked ? FastScrollTick{nullptr, 0} : fast_scroll(axes_.rstick_x, axes_.rstick_y, fast_cfg_);
  if (t.key == fast_key_) return;  // pointer identity: same direction, only the rate moved
  fast_key_ = t.key;
  if (!fast_key_) return;
  dispatch_scroll(fast_key_);  // the first step lands on the push, not after a delay
  fast_next_ms_ = mono_ms() + t.interval_ms;
}

void GamepadInput::ensure_haptic() {
  if (haptic_tried_) return;
  // Once only, so a pad without FF is not re-opened on every pulse.
  haptic_tried_ = true;
  for (const std::string& p : paths_)
    if (haptic_.attach(p)) break;
  if (!haptic_.attached()) info("input: no force-feedback device — haptics unavailable");
}

void GamepadInput::toggle_gestures() {
  if (!gestures_) return;
  const bool on = gestures_->toggle();
  info(std::format("input: touch gestures -> {}", on ? "on" : "off"));
  // Confirm on two channels: toast and rumble.
  show_toast(client_, on ? "Touch gestures on" : "Touch gestures off", 1400);
  ensure_haptic();
  haptic_.rumble(kExitRumbleStrong, kExitRumbleWeak, kExitRumbleMs);
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
        // Y is tap-vs-hold: hold exits the app, a tap means "ignore this update" and fires from the
        // release edge below.
        if (parse_verdict(osd_->exec("hold_start")).kind == OsdVerdict::Kind::Hold)
          exit_hold_deadline_ = mono_ms() + osd_->exit_hold_ms();
      } else if (code == osd_lb_ && osd_lb_ >= 0)
        osd_->exec("tab_prev");
      else if (code == osd_rb_ && osd_rb_ >= 0)
        osd_->exec("tab_next");
    }
    if (value == 0 && code == osd_y_ && osd_y_ >= 0 && exit_hold_deadline_ != 0) {
      exit_hold_deadline_ = 0;  // released before the hold completed — no exit
      osd_->exec("hold_cancel");
      osd_->exec("ignore");  // ...so Y still reads as a tap
    }
    return;  // swallow every button while the menu is up
  }
  if (type != EV_ABS) return;
  // Triggers only update their latch here: a seek must not fire under the modal menu, but a release
  // it swallowed would leave the next press dead or a modifier layer stuck.
  switch (track_axes(axes_, code, value)) {
    case AxisEvent::Direction:
      // dispatch_key / dispatch_scroll are OSD-aware, so this drives menu focus with the same
      // auto-repeat machinery.
      set_direction(resolve_direction(axes_.hat_x, axes_.hat_y, axes_.stick_x, axes_.stick_y));
      return;
    case AxisEvent::RightStick:
      update_fast_scroll();
      return;
    default:
      return;
  }
}

bool GamepadInput::osd_open_edge(int type, int code, int value) {
  if (!osd_ || osd_->open()) return false;
  if (type == EV_KEY && value == 1 && code == menu_code_ && menu_code_ >= 0) {
    // Menu opens the OSD during playback too; on the watch screen it is the only route in (the
    // on-screen Settings button is hidden there).
    osd_->open_menu();
    return true;
  }
  return false;
}

bool GamepadInput::handle_onboarding(int type, int code, int value) {
  if (!onboarding_ || !onboarding_->visible()) return false;
  // Modal first-run card: any button press dismisses it; sticks/D-pad are swallowed but still
  // tracked, so a direction released under the card is not still "held" once it goes.
  if (type == EV_KEY && value == 1) onboarding_->hide();
  if (type == EV_ABS) track_axes(axes_, code, value);
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
    if (value != 1) return;  // press edge only (ignore release=0 and kernel autorepeat=2)
    const Press p =
        resolve_press(maps_, code, layer(), axes_.lt, axes_.rt, captions_code_, gestures_code_);
    switch (p.kind) {
      case Press::Kind::Captions:
        toggle_captions();
        return;
      case Press::Kind::Gestures:
        toggle_gestures();
        return;
      case Press::Kind::Key:
        info(std::format("input: btn {} -> {} [{}]", code, p.key, layer_name(layer())));
        client_.dispatch_key(p.key);
        return;
      case Press::Kind::None:
        return;  // unbound, or absorbed by a held modifier layer
    }
    return;
  }
  if (type != EV_ABS) return;
  // A trigger acting as a modifier dispatches nothing itself (its lt_key_/rt_key_ stay empty).
  switch (track_axes(axes_, code, value)) {
    case AxisEvent::Direction:
      set_direction(resolve_direction(axes_.hat_x, axes_.hat_y, axes_.stick_x, axes_.stick_y));
      return;
    case AxisEvent::RightStick:
      update_fast_scroll();
      return;
    case AxisEvent::LtPressed:
      if (!lt_skip_js_.empty()) {
        info("input: lt -> skip");
        client_.eval_void(lt_skip_js_);
      } else if (!lt_key_.empty()) {
        info(std::format("input: lt -> {}", lt_key_));
        client_.dispatch_key(lt_key_);
      }
      return;
    case AxisEvent::RtPressed:
      if (!rt_skip_js_.empty()) {
        info("input: rt -> skip");
        client_.eval_void(rt_skip_js_);
      } else if (!rt_key_.empty()) {
        info(std::format("input: rt -> {}", rt_key_));
        client_.dispatch_key(rt_key_);
      }
      return;
    case AxisEvent::Trigger:
    case AxisEvent::Ignored:
      return;
  }
}

void GamepadInput::process_event(int type, int code, int value) {
  // SYN_DROPPED: the kernel overflowed this device's queue and discarded events, releases included,
  // so nothing held can be trusted until it is pressed again.
  if (type == EV_SYN && code == SYN_DROPPED)
    drop_held_state();
  else
    handle_event(type, code, value);
}

void GamepadInput::loop() {
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
    if (dir_key_) consider(next_repeat_ms_);
    if (fast_key_) consider(fast_next_ms_);
    if (caption_apply_pending_) consider(caption_next_ms_);
    if (exit_hold_deadline_ != 0) consider(exit_hold_deadline_);

    std::vector<pollfd> pfds;
    pfds.reserve(fds_.size());
    for (int fd : fds_) pfds.push_back(pollfd{fd, POLLIN, 0});
    // poll() on an empty set returns at once; sleep on the worker instead, which stop() also wakes.
    if (pfds.empty())
      worker_.wait_or_stop(static_cast<int>(timeout));
    else
      poll(pfds.data(), pfds.size(), static_cast<int>(timeout));  // EINTR/errors: just loop again

    std::vector<size_t> gone;
    for (size_t i = 0; i < pfds.size(); ++i) {
      if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
      input_event evs[32];
      ssize_t r = read(pfds[i].fd, evs, sizeof(evs));
      if (r <= 0) {
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        gone.push_back(i);  // device went away
        continue;
      }
      const size_t count = static_cast<size_t>(r) / sizeof(input_event);
      for (size_t k = 0; k < count; ++k) {
        int value = evs[k].value;
        if (evs[k].type == EV_ABS)
          if (const int slot = axis_slot(evs[k].code); slot >= 0)
            value = normalize_axis(evs[k].code, value, ranges_[i][slot]);
        process_event(evs[k].type, evs[k].code, value);
      }
    }

    // The watch signal is video_up, not `layer() == Layer::Player`: the OSK outranks the player
    // while a video plays underneath.
    const bool on_watch = layers_ && layers_->video_up();
    if (update_prompt_) update_prompt_->tick(on_watch);
    if (osd_) osd_->tick(on_watch);
    tick_caption_apply(on_watch);

    // Health-check the first-run card before acting on its visibility: a reload deletes it
    // silently.
    if (onboarding_) onboarding_->tick();
    // Stop a held direction auto-repeating into the page behind the modal first-run card.
    if (onboarding_ && onboarding_->visible()) set_direction(nullptr);

    // The Exit hold belongs to the open menu: if the menu closed under it, the Y release never
    // reaches osd_event, so cancel here. On completion, rumble before exiting.
    if (exit_hold_deadline_ != 0 && !(osd_ && osd_->open())) exit_hold_deadline_ = 0;
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

    // Right-stick repeat. The interval is re-read from the current deflection every step.
    if (fast_key_ && mono_ms() >= fast_next_ms_) {
      const FastScrollTick t = fast_scroll(axes_.rstick_x, axes_.rstick_y, fast_cfg_);
      if (t.key != fast_key_) {
        update_fast_scroll();  // the stick crossed into another direction between polls
      } else {
        dispatch_scroll(fast_key_);
        fast_next_ms_ = mono_ms() + t.interval_ms;
      }
    }

    if (!gone.empty()) {
      // Close only the pads that went away; the others keep their fds and queued events.
      for (auto it = gone.rbegin(); it != gone.rend(); ++it) close_device(*it);
      // Steam re-creates its virtual pad under a new node (e.g. across resume): pick it up now.
      discover_new_devices();
      if (fds_.empty()) info("input: no gamepad device found yet (will retry)");
      last_scan = mono_ms();
      drop_held_state();  // the lost pad's release events will never come; state is shared
      haptic_.detach();   // re-attach rumble on the next pulse
      haptic_tried_ = false;
    } else if (fds_.empty() && mono_ms() - last_scan >= 2000) {
      rescan_devices();
      last_scan = mono_ms();
    } else if (mono_ms() - last_scan >= kHotplugScanMs) {
      // Hotplug while another pad is open. Non-destructive: held keys survive.
      discover_new_devices();
      last_scan = mono_ms();
    }
  }
  close_devices();
  haptic_.detach();
}

}  // namespace deckback

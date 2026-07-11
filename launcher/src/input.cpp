#include "input.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <format>

#include "log.hpp"
#include "overlay.hpp"

namespace deckback {
namespace {

constexpr long kInitialDelayMs = 350;  // hold-to-repeat: delay before the first auto-repeat
constexpr long kRepeatMs = 130;        // base repeat interval
constexpr long kMinRepeatMs = 55;      // fastest interval after acceleration
constexpr int kStickDeadzone =
    13000;                       // |axis| beyond this counts as a direction (Xbox range ~32767)
constexpr int kAxisMax = 32767;  // full stick deflection
// Floor on the right-stick repeat interval. Each arrow is a keyDown+keyUp pair, each awaited over
// the CDP socket; faster than this and the input thread stops draining evdev.
constexpr long kMinScrollMs = 25;

// Direction key constants — compared by pointer identity within this TU.
constexpr const char* kUp = "ArrowUp";
constexpr const char* kDown = "ArrowDown";
constexpr const char* kLeft = "ArrowLeft";
constexpr const char* kRight = "ArrowRight";

// Analog trigger thresholds (Xbox pad reports 0..255). Hysteresis keeps a half-pulled trigger from
// chattering a key press on every jittering report.
constexpr int kTriggerPress = 192;
constexpr int kTriggerRelease = 64;

// How often to look for newly-attached pads while others are already open (hotplug).
constexpr long kHotplugScanMs = 2000;

// Semantic action -> DOM key, for the doc §6 P3 table. Actions absent here have no DOM key we can
// synthesise yet (voice_search, player_menu, scan_rewind, scan_forward): Leanback's bindings for
// them are not established, so we refuse to guess and report them as unmapped at startup instead of
// dispatching a key that silently does nothing. `navigate` is handled by the D-pad/stick path.
//
// `replaced_by` marks a deprecated name that still resolves. `seek_back_10`/`seek_fwd_10` were
// misnamed: they dispatch ArrowLeft/ArrowRight, which *scrub* the progress bar (the console model),
// never a fixed 10 s jump. They keep working for one release so a remotely hot-swapped app.json
// written against the old names does not silently lose its bindings (doc §6 R1).
struct ActionAlias {
  std::string_view action;
  std::string_view key;
  std::string_view replaced_by;  // "" = current name
};
constexpr ActionAlias kActionAliases[] = {
    {"select", "Enter", ""},
    {"back", "Escape", ""},
    {"playpause", "MediaPlayPause", ""},
    {"toggle_captions", "c", ""},
    {"scrub_back", "ArrowLeft", ""},
    {"scrub_fwd", "ArrowRight", ""},
    {"seek_back_10", "ArrowLeft", "scrub_back"},
    {"seek_fwd_10", "ArrowRight", "scrub_fwd"},
};

// Deck control name -> evdev EV_KEY code. Xbox-pad naming: WEST is X, NORTH is Y.
struct ControlCode {
  std::string_view name;
  int code;
};
constexpr ControlCode kControlCodes[] = {
    {"a", BTN_SOUTH},   {"b", BTN_EAST},    {"x", BTN_WEST},      {"y", BTN_NORTH},
    {"lb", BTN_TL},     {"rb", BTN_TR},     {"start", BTN_START}, {"select", BTN_SELECT},
    {"l3", BTN_THUMBL}, {"r3", BTN_THUMBR},
};

// Direction control names, for layers that rebind the arrows (e.g. an LT modifier layer turning
// Left/Right into scan keys). Keyed by the base arrow `resolve_direction()` produces.
struct DirectionControl {
  std::string_view name;
  std::string_view arrow;
};
constexpr DirectionControl kDirectionControls[] = {
    {"up", "ArrowUp"},
    {"down", "ArrowDown"},
    {"left", "ArrowLeft"},
    {"right", "ArrowRight"},
};

const DirectionControl* direction_control(std::string_view name) {
  for (const DirectionControl& d : kDirectionControls)
    if (d.name == name) return &d;
  return nullptr;
}

// Look a control up inside one resolved layer. "" = the layer does not bind it.
std::string find_button(const KeyLayer& l, int code) {
  for (const ButtonBinding& b : l.buttons)
    if (b.code == code) return b.key;
  return {};
}

std::string find_direction(const KeyLayer& l, std::string_view base_arrow) {
  for (const auto& [arrow, key] : l.directions)
    if (arrow == base_arrow) return key;
  return {};
}

long now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1'000'000L;
}

bool test_bit(int bit, const unsigned long* arr) {
  return (arr[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

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

// ---- pure helpers ------------------------------------------------------------------------------

std::string resolve_binding(std::string_view value) {
  if (value.empty()) return {};
  // A DOM key value wins: that is what lets app.json rebind a control without a launcher rebuild.
  if (DevToolsClient::key_supported(value)) return std::string(value);
  for (const ActionAlias& a : kActionAliases)
    if (a.action == value) return std::string(a.key);
  return {};
}

std::string_view deprecated_action_replacement(std::string_view value) {
  // A DOM key value is never a deprecated action, and single printable chars are DOM keys — check
  // that first so a hypothetical one-character action name can't shadow a key.
  if (DevToolsClient::key_supported(value)) return {};
  for (const ActionAlias& a : kActionAliases)
    if (a.action == value) return a.replaced_by;
  return {};
}

const char* resolve_direction(int hat_x, int hat_y, int stick_x, int stick_y) {
  const int dx = hat_x != 0 ? hat_x : stick_x;
  const int dy = hat_y != 0 ? hat_y : stick_y;
  if (dy < 0) return kUp;
  if (dy > 0) return kDown;
  if (dx < 0) return kLeft;
  if (dx > 0) return kRight;
  return nullptr;
}

std::vector<ButtonBinding> build_button_map(
    const std::vector<std::pair<std::string, std::string>>& keymap,
    std::vector<std::string>* unmapped) {
  std::vector<ButtonBinding> out;
  for (const auto& [name, value] : keymap) {
    if (name == "dpad" || name == "lt" || name == "rt") continue;  // not EV_KEY buttons
    const ControlCode* cc = nullptr;
    for (const ControlCode& c : kControlCodes)
      if (c.name == name) cc = &c;
    if (!cc) {
      if (unmapped) unmapped->push_back(std::format("{}={} (unknown control)", name, value));
      continue;
    }
    std::string key = resolve_binding(value);
    if (key.empty()) {
      if (unmapped) unmapped->push_back(std::format("{}={} (no DOM key)", name, value));
      continue;
    }
    out.push_back(ButtonBinding{cc->code, std::move(key)});
  }
  return out;
}

bool trigger_pressed(int value, bool was_pressed) {
  if (was_pressed) return value > kTriggerRelease;
  return value >= kTriggerPress;
}

FastScrollTick fast_scroll(int rx, int ry, const FastScrollConfig& cfg) {
  if (!cfg.enabled) return {nullptr, 0};

  // We normalise against the Xbox-pad range, which is what Steam Input's virtual pad reports
  // (input-ux §1). A pad with a narrower range would scroll slower than intended, never faster —
  // and on a Deck in Game Mode there is only ever the one virtual pad. Not read from `absinfo`
  // because the merged-device path has no single authoritative range.
  //
  // Saturate before taking |v|: the axis minimum is -32768, one past the maximum, so an unclamped
  // full down-left push would compare |x| > |y| and resolve horizontally at the physical corner.
  auto mag = [](int v) {
    if (v < -kAxisMax) v = -kAxisMax;
    if (v > kAxisMax) v = kAxisMax;
    return v < 0 ? -v : v;
  };
  const int ax = mag(rx), ay = mag(ry);
  const int dz = cfg.deadzone < 0 ? 0 : cfg.deadzone;

  // Vertical takes the tie, so a perfectly diagonal push resolves the way rails run. This is why
  // the clamp above matters: unclamped, |-32768| > |32767| and a full down-left push would resolve
  // left.
  const bool vertical = ay >= ax;
  const int m = vertical ? ay : ax;
  // Also the guard for a degenerate deadzone: with dz >= kAxisMax no `m` can exceed it, so the
  // division below never sees a zero (or negative) span. No separate check is needed, and one that
  // could never fire would only look like safety.
  if (m <= dz) return {nullptr, 0};

  const char* key;
  if (vertical)
    key = (ry < 0) ? kUp : kDown;  // evdev Y is inverted: negative is up
  else
    key = (rx < 0) ? kLeft : kRight;

  // Linear in deflection past the deadzone: slow_ms at the edge, fast_ms at full travel. Linear and
  // not curved, because the curve belongs to the user's thumb.
  const double t = static_cast<double>(m - dz) / static_cast<double>(kAxisMax - dz);
  long interval = cfg.slow_ms + static_cast<long>(t * (cfg.fast_ms - cfg.slow_ms));
  // An inverted config (fast_ms > slow_ms) is a preference, not an error. Clamp into the interval's
  // own range either way, so it can never go negative and busy-loop the poll.
  const long lo = cfg.fast_ms < cfg.slow_ms ? cfg.fast_ms : cfg.slow_ms;
  const long hi = cfg.fast_ms < cfg.slow_ms ? cfg.slow_ms : cfg.fast_ms;
  if (interval < lo) interval = lo;
  if (interval > hi) interval = hi;
  // Each arrow costs two CDP round trips (keyDown + keyUp, both awaited). Below this the input
  // thread would spend all its time in the socket and stop reading the pad.
  if (interval < kMinScrollMs) interval = kMinScrollMs;
  return {key, interval};
}

// ---- layered keymaps ---------------------------------------------------------------------------

KeyLayer build_layer(const std::vector<std::pair<std::string, std::string>>& keymap,
                     std::string_view label, std::vector<std::string>* unmapped) {
  KeyLayer out;
  std::vector<std::pair<std::string, std::string>> button_entries;
  for (const auto& [name, value] : keymap) {
    if (const DirectionControl* d = direction_control(name)) {
      std::string key = resolve_binding(value);
      if (key.empty()) {
        if (unmapped) unmapped->push_back(std::format("{}.{}={} (no DOM key)", label, name, value));
        continue;
      }
      out.directions.emplace_back(std::string(d->arrow), std::move(key));
      continue;
    }
    button_entries.emplace_back(name, value);
  }

  // build_button_map reports its own unmapped entries without the layer label; collect and
  // re-prefix so a warning names the layer that is broken rather than just the control.
  std::vector<std::string> raw;
  out.buttons = build_button_map(button_entries, &raw);
  if (unmapped)
    for (std::string& u : raw) unmapped->push_back(std::format("{}.{}", label, u));
  return out;
}

Keymaps build_keymaps(const KeymapConfig& cfg, std::vector<std::string>* unmapped) {
  Keymaps m;
  auto base = cfg.base;
  if (base.empty()) base = {{"a", "select"}, {"b", "back"}};  // the M1 set, before keymaps existed
  m.base = build_layer(base, "keymap", unmapped);
  m.player = build_layer(cfg.player, "keymap_player", unmapped);
  m.osk = build_layer(cfg.osk, "keymap_osk", unmapped);
  m.lt_mod = build_layer(cfg.lt_mod, "keymap_lt", unmapped);
  m.rt_mod = build_layer(cfg.rt_mod, "keymap_rt", unmapped);
  return m;
}

namespace {
// The context layer for `layer`, or nullptr for Browse (which *is* base).
const KeyLayer* context_layer(const Keymaps& m, Layer layer) {
  switch (layer) {
    case Layer::Osk:
      return &m.osk;
    case Layer::Player:
      return &m.player;
    case Layer::Browse:
      return nullptr;
  }
  return nullptr;
}

// The active modifier layer, or nullptr when no trigger is acting as a modifier right now. An empty
// modifier layer is not a modifier at all — the trigger keeps its own binding.
const KeyLayer* active_modifier(const Keymaps& m, bool lt_held, bool rt_held) {
  if (lt_held && !m.lt_mod.empty()) return &m.lt_mod;  // LT wins over RT: deterministic, documented
  if (rt_held && !m.rt_mod.empty()) return &m.rt_mod;
  return nullptr;
}
}  // namespace

std::string resolve_button(const Keymaps& maps, int code, Layer layer, bool lt_held, bool rt_held) {
  // A modifier layer absorbs: an unbound control under a held modifier dispatches nothing, rather
  // than falling through and firing the normal action the user was trying not to fire.
  if (const KeyLayer* mod = active_modifier(maps, lt_held, rt_held)) return find_button(*mod, code);

  // A context layer only overrides: unbound controls fall through to base.
  if (const KeyLayer* ctx = context_layer(maps, layer)) {
    std::string k = find_button(*ctx, code);
    if (!k.empty()) return k;
  }
  return find_button(maps.base, code);
}

std::string resolve_direction_key(const Keymaps& maps, std::string_view base_arrow, Layer layer,
                                  bool lt_held, bool rt_held) {
  if (const KeyLayer* mod = active_modifier(maps, lt_held, rt_held))
    return find_direction(*mod, base_arrow);

  if (const KeyLayer* ctx = context_layer(maps, layer)) {
    std::string k = find_direction(*ctx, base_arrow);
    if (!k.empty()) return k;
  }
  std::string k = find_direction(maps.base, base_arrow);
  return k.empty() ? std::string(base_arrow) : k;  // unbound: a D-pad press is just its arrow
}

std::pair<int, int> parse_chord(std::string_view chord) {
  const size_t plus = chord.find('+');
  if (plus == std::string_view::npos) return {-1, -1};
  auto trim_lower = [](std::string_view v) {
    size_t b = 0, e = v.size();
    while (b < e && (v[b] == ' ' || v[b] == '\t')) ++b;
    while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t')) --e;
    std::string out(v.substr(b, e - b));
    for (char& ch : out)
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return out;
  };
  auto lookup = [](const std::string& n) -> int {
    for (const ControlCode& c : kControlCodes)
      if (c.name == n) return c.code;
    return -1;
  };
  const int a = lookup(trim_lower(chord.substr(0, plus)));
  const int b = lookup(trim_lower(chord.substr(plus + 1)));
  if (a < 0 || b < 0 || a == b) return {-1, -1};
  return {a, b};
}

TouchLockChord::Action TouchLockChord::on_chord(bool both_down, long now_ms) {
  if (!both_down) {
    engaged_ = false;  // the chord may adjudicate again on the next press
    waiting_ = false;
    return Action::None;
  }
  if (engaged_) return Action::None;  // one action per engagement; a held chord does not repeat
  // A repeat report while the hold is already counting down must not restart it: evdev sends
  // value==2 for autorepeat, and Steam merges pads so the same press can arrive twice. Re-arming
  // here would push the deadline out on every event and the unlock would never mature.
  if (waiting_) return on_tick(now_ms);
  if (!locked_) {
    engaged_ = true;
    locked_ = true;
    return Action::Lock;
  }
  // Locked: arm the deliberate-unlock hold. It matures in on_tick(), not here — a chord press that
  // is released early must leave the lock in place.
  waiting_ = true;
  hold_start_ms_ = now_ms;
  return on_tick(now_ms);  // an unlock_hold_ms of 0 unlocks on the press, as configured
}

TouchLockChord::Action TouchLockChord::on_tick(long now_ms) {
  if (!waiting_ || now_ms < deadline_ms()) return Action::None;
  waiting_ = false;
  engaged_ = true;  // consumed: keeping the chord held must not immediately re-lock
  locked_ = false;
  return Action::Unlock;
}

int find_control_for_action(const std::vector<std::pair<std::string, std::string>>& keymap,
                            std::string_view action) {
  for (const auto& [name, value] : keymap) {
    if (value != action) continue;
    for (const ControlCode& c : kControlCodes)
      if (c.name == name) return c.code;
  }
  return -1;
}

std::vector<std::pair<std::string, std::string>> without_action(
    const std::vector<std::pair<std::string, std::string>>& keymap, std::string_view action) {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(keymap.size());
  for (const auto& kv : keymap)
    if (kv.second != action) out.push_back(kv);
  return out;
}

// ------------------------------------------------------------------------------------------------

GamepadInput::GamepadInput(std::string host, int port, KeymapConfig keymap, TouchConfig touch,
                           const LayerState* layers, VoiceController* voice, long voice_hold_ms,
                           FastScrollConfig fast_scroll_cfg, OnboardingController* onboarding)
    : client_(std::move(host), port),
      block_initial_(touch.block_initial),
      chord_(touch.unlock_hold_ms),
      chord_label_(touch.chord),
      touch_toast_(touch.toast),
      touch_haptic_(touch.haptic),
      layers_(layers),
      voice_(voice),
      hold_(voice_hold_ms),
      onboarding_(onboarding),
      fast_cfg_(fast_scroll_cfg) {
  // Voice is not a DOM key. When it is enabled, take its control out of the button map so it is not
  // reported as an unmapped binding; when it is not, leave it there so startup says so out loud.
  if (voice_) {
    voice_code_ = find_control_for_action(keymap.base, "voice_search");
    if (voice_code_ < 0)
      warn(
          "input: voice is enabled but no control is bound to 'voice_search' — hold-to-talk is "
          "off");
    else
      info(std::format("input: hold-to-talk on evdev code {} ({} ms)", voice_code_, voice_hold_ms));
    keymap.base = without_action(keymap.base, "voice_search");
  }
  // `show_controls` is a launcher action too: it has no DOM key, and re-opens the controls card.
  if (onboarding_) {
    help_code_ = find_control_for_action(keymap.base, "show_controls");
    if (help_code_ >= 0)
      info(std::format("input: controls card re-opens on evdev code {}", help_code_));
    keymap.base = without_action(keymap.base, "show_controls");
  }
  if (touch.lock_enabled) {
    const auto [a, b] = parse_chord(touch.chord);
    chord_a_code_ = a;
    chord_b_code_ = b;
    if (chord_a_code_ < 0)
      warn(std::format("input: touch_lock_chord '{}' unrecognised — chord lock disabled",
                       touch.chord));
    else
      info(std::format("input: touch lock chord = {} (codes {}+{}), unlock hold {} ms", touch.chord,
                       chord_a_code_, chord_b_code_, touch.unlock_hold_ms));
  }

  std::vector<std::string> unmapped;
  maps_ = build_keymaps(keymap, &unmapped);

  // The triggers keep their own binding only while they are not modifiers.
  for (const auto& [name, value] : keymap.base) {
    if (name != "lt" && name != "rt") continue;
    const bool is_mod = (name == "lt") ? !maps_.lt_mod.empty() : !maps_.rt_mod.empty();
    if (is_mod) continue;
    std::string key = resolve_binding(value);
    if (key.empty())
      unmapped.push_back(std::format("keymap.{}={} (no DOM key)", name, value));
    else
      (name == "lt" ? lt_key_ : rt_key_) = std::move(key);
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
  {
    std::lock_guard lk(mu_);
    if (started_) return;
    started_ = true;
    stop_ = false;
  }
  thread_ = std::thread([this] { loop(); });
}

void GamepadInput::stop() {
  {
    std::lock_guard lk(mu_);
    if (!started_) return;
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard lk(mu_);
  started_ = false;
}

bool GamepadInput::stopping() {
  std::lock_guard lk(mu_);
  return stop_;
}

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
  DIR* d = opendir("/dev/input");
  if (!d) return;
  for (dirent* e; (e = readdir(d)) != nullptr;) {
    if (std::strncmp(e->d_name, "event", 5) != 0) continue;
    std::string path = std::string("/dev/input/") + e->d_name;
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
  if (fds_.empty()) info("input: no gamepad device found yet (will retry)");
}

void GamepadInput::dispatch_key(const char* base_arrow) {
  // Resolved per dispatch, not once per press: releasing a modifier or opening the player mid-hold
  // takes effect on the next repeat without the user re-pressing the stick.
  const std::string key = resolve_direction_key(maps_, base_arrow, layer(), lt_down_, rt_down_);
  if (key.empty()) return;  // absorbed by a held modifier layer that does not bind this direction
  client_.dispatch_key(key);
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
      next_repeat_ms_ = now_ms() + kInitialDelayMs;
    }
  }
  // Unconditional, even when the direction did not change: the D-pad/left stick own the direction
  // while they hold one, and clearing it (a release, or a lost device) must hand the right stick
  // back. Cheap — update_fast_scroll() no-ops unless the resolved direction moved.
  update_fast_scroll();
}

void GamepadInput::update_fast_scroll() {
  // A held D-pad/left-stick direction suppresses the right stick entirely — and so does the modal
  // controls card, which would otherwise scroll Leanback's focus behind itself.
  const bool blocked = dir_key_ || (onboarding_ && onboarding_->visible());
  const FastScrollTick t =
      blocked ? FastScrollTick{nullptr, 0} : fast_scroll(rstick_x_, rstick_y_, fast_cfg_);
  if (t.key == fast_key_) return;  // pointer identity: same direction, only the rate moved
  fast_key_ = t.key;
  if (!fast_key_) return;
  info(std::format("input: {} (right stick)", fast_key_));
  dispatch_key(fast_key_);  // the first step lands on the push, not after a delay
  fast_next_ms_ = now_ms() + t.interval_ms;
}

bool GamepadInput::handle_chord(int code, int value) {
  if (chord_a_code_ < 0) return false;  // chord disabled
  if (code != chord_a_code_ && code != chord_b_code_) return false;
  const bool down = (value != 0);  // press or kernel autorepeat = held; release = up
  if (code == chord_a_code_)
    chord_a_down_ = down;
  else
    chord_b_down_ = down;
  apply_touch_lock(chord_.on_chord(chord_a_down_ && chord_b_down_, now_ms()));
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
  if (!haptic_tried_) {
    // Deferred to the first pulse: at construction no pad has been discovered yet. Once only —
    // a pad without FF must not re-open a device on every toggle.
    haptic_tried_ = true;
    for (const std::string& p : paths_)
      if (haptic_.attach(p)) break;
    if (!haptic_.attached())
      info("input: no force-feedback device — touch-lock haptics unavailable (toast only)");
  }
  // Two magnitudes, not one: locking and unlocking must feel different in the pocket, since the
  // user may trip the chord without looking at the screen.
  if (locked)
    haptic_.rumble(0xC000, 0x4000, 180);
  else
    haptic_.rumble(0x3000, 0x8000, 90);
}

bool GamepadInput::handle_voice(int code, int value) {
  if (!voice_ || voice_code_ < 0 || code != voice_code_) return false;
  const long now = now_ms();
  if (value == 1) {
    hold_.on_press(now);  // never starts here — a stray tap must not open the mic
  } else if (value == 0) {
    if (hold_.on_release(now) == HoldToTalk::Action::Stop) voice_->stop();
  }
  // value == 2 is kernel auto-repeat while held: nothing to do; the loop tick drives the start.
  return true;  // the voice button is ours; never also dispatch it as a mapped button
}

bool GamepadInput::handle_onboarding(int type, int code, int value) {
  if (!onboarding_) return false;

  if (onboarding_->visible()) {
    // Modal. Every event is swallowed, or the D-pad would move Leanback's focus behind a card the
    // user cannot see through — and the first thing they would do after dismissing it is wonder
    // where they are.
    //
    // Only a *button* dismisses, never a stick or D-pad: an analog stick at rest drifts, and a card
    // dismissed by a resting thumb before it is read is a card that was never shown.
    if (type == EV_KEY && value == 1) onboarding_->hide();
    return true;
  }

  // Re-open. Deliberate, so it is a press edge and never an auto-repeat.
  if (type == EV_KEY && value == 1 && code == help_code_ && help_code_ >= 0) {
    onboarding_->show(/*first_run_only=*/false);
    return true;
  }
  return false;
}

void GamepadInput::handle_event(int type, int code, int value) {
  if (handle_onboarding(type, code, value)) return;
  if (type == EV_KEY) {
    if (handle_chord(code, value)) return;
    if (handle_voice(code, value)) return;
    if (value != 1) return;  // press edge only (ignore release=0 and kernel autorepeat=2)
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
      if (now && !lt_down_ && !lt_key_.empty()) {
        info(std::format("input: lt -> {}", lt_key_));
        client_.dispatch_key(lt_key_);
      }
      lt_down_ = now;
      return;
    }
    case ABS_RZ: {
      const bool now = trigger_pressed(value, rt_down_);
      if (now && !rt_down_ && !rt_key_.empty()) {
        info(std::format("input: rt -> {}", rt_key_));
        client_.dispatch_key(rt_key_);
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
  long last_scan = now_ms();

  while (!stopping()) {
    // Poll timeout: the next auto-repeat deadline, a rescan tick when idle, else a periodic wake.
    long timeout = 1000;
    if (dir_key_) {
      long d = next_repeat_ms_ - now_ms();
      timeout = d > 0 ? d : 0;
    } else if (fds_.empty()) {
      timeout = 2000;  // retry device discovery
    }
    // The right stick emits no events while held at a constant deflection, so its repeat, like the
    // hold-to-talk deadline, has to be a timer the poll wakes for.
    if (fast_key_) {
      long d = fast_next_ms_ - now_ms();
      if (d < 0) d = 0;
      if (d < timeout) timeout = d;
    }
    // A held button emits no further events, so without waking at the hold deadline the mic would
    // only open on the next unrelated event — or never.
    if (hold_.pending()) {
      long d = hold_.deadline_ms() - now_ms();
      if (d < 0) d = 0;
      if (d < timeout) timeout = d;
    }
    // Same for the deliberate-unlock hold: the chord is held, so no further event is coming.
    if (chord_.pending()) {
      long d = chord_.deadline_ms() - now_ms();
      if (d < 0) d = 0;
      if (d < timeout) timeout = d;
    }

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

    // The controls card is modal, and a direction held when it appeared would keep auto-repeating
    // into the page behind it. Dropping the direction also releases the right-stick scroll.
    if (onboarding_ && onboarding_->visible()) set_direction(nullptr);

    // Hold-to-talk matures on a timer, not on an event.
    if (hold_.pending() && hold_.on_tick(now_ms()) == HoldToTalk::Action::Start) voice_->start();
    if (chord_.pending()) apply_touch_lock(chord_.on_tick(now_ms()));

    // Directional auto-repeat with acceleration.
    if (dir_key_ && now_ms() >= next_repeat_ms_) {
      dispatch_direction();
      long interval = kRepeatMs - static_cast<long>(repeat_count_) * 15;
      if (interval < kMinRepeatMs) interval = kMinRepeatMs;
      ++repeat_count_;
      next_repeat_ms_ = now_ms() + interval;
    }

    // Right-stick repeat. The interval is re-read from the *current* deflection every step, so
    // easing off the stick slows the scroll on the next arrow rather than at the next event.
    if (fast_key_ && now_ms() >= fast_next_ms_) {
      const FastScrollTick t = fast_scroll(rstick_x_, rstick_y_, fast_cfg_);
      if (t.key != fast_key_) {
        update_fast_scroll();  // the stick crossed into another direction between polls
      } else {
        dispatch_key(fast_key_);
        fast_next_ms_ = now_ms() + t.interval_ms;
      }
    }

    if (lost || (fds_.empty() && now_ms() - last_scan >= 2000)) {
      rescan_devices();
      last_scan = now_ms();
      hat_x_ = hat_y_ = stick_x_ = stick_y_ = 0;
      rstick_x_ = rstick_y_ = 0;
      set_direction(nullptr);  // drop any held direction (and the right-stick scroll) across a
                               // device change: with the pad gone, no release event is coming
    } else if (now_ms() - last_scan >= kHotplugScanMs) {
      // Hotplug: a pad attached while another is already open would otherwise never be seen,
      // because neither `lost` nor `fds_.empty()` ever becomes true. Non-destructive — held keys
      // survive.
      discover_new_devices();
      last_scan = now_ms();
    }
  }
  close_devices();
  haptic_.detach();
  if (voice_) voice_->stop();                      // never leave the mic open (or playback ducked)
  if (chord_.locked()) touch_.set_blocked(false);  // never leave the touchscreen grabbed on exit
}

}  // namespace deckback

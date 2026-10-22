#include "keymap.hpp"

#include <linux/input.h>

#include <format>

#include "devtools.hpp"
#include "util.hpp"

namespace deckback {
namespace {

constexpr int kAxisMax = 32767;  // full stick deflection
// Floor on the right-stick repeat interval. Each arrow is a keyDown+keyUp pair, each awaited over
// the CDP socket; faster than this and the input thread stops draining evdev.
constexpr long kMinScrollMs = 25;

// Direction key constants — compared by pointer identity within this TU.
constexpr const char* kUp = "ArrowUp";
constexpr const char* kDown = "ArrowDown";
constexpr const char* kLeft = "ArrowLeft";
constexpr const char* kRight = "ArrowRight";

// Analog trigger thresholds (Xbox pad reports 0..255), with hysteresis.
constexpr int kTriggerPress = 192;
constexpr int kTriggerRelease = 64;

// Semantic action -> DOM key. Actions with no known Leanback key (voice_search, player_menu,
// scan_rewind, scan_forward) are absent and get reported as unmapped. `navigate` is handled by the
// D-pad/stick path. `replaced_by` marks a deprecated name that still resolves.
struct ActionAlias {
  std::string_view action;
  std::string_view key;
  std::string_view replaced_by;  // "" = current name
};
// The launcher actions toggle_captions and toggle_gestures have no DOM key and are absent here
// (youtube.com/tv ignores the desktop `c` caption hotkey).
constexpr ActionAlias kActionAliases[] = {
    {"select", "Enter", ""},
    {"back", "Escape", ""},
    {"playpause", "MediaPlayPause", ""},
    {"scrub_back", "ArrowLeft", ""},
    {"scrub_fwd", "ArrowRight", ""},
    {"seek_back_10", "ArrowLeft", "scrub_back"},
    {"seek_fwd_10", "ArrowRight", "scrub_fwd"},
};

// Deck control name -> evdev EV_KEY code. Use the labelled codes BTN_X/BTN_Y: the Steam Input
// virtual pad reports face buttons by label, and in <linux/input-event-codes.h> BTN_NORTH is BTN_X
// while BTN_WEST is BTN_Y, so the positional names would swap X and Y.
struct ControlCode {
  std::string_view name;
  int code;
};
constexpr ControlCode kControlCodes[] = {
    {"a", BTN_SOUTH},   {"b", BTN_EAST},    {"x", BTN_X},         {"y", BTN_Y},
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
  if (lt_held && !m.lt_mod.empty()) return &m.lt_mod;  // LT wins over RT
  if (rt_held && !m.rt_mod.empty()) return &m.rt_mod;
  return nullptr;
}

}  // namespace

std::string resolve_binding(std::string_view value) {
  if (value.empty()) return {};
  // A DOM key value wins over an action name.
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
    // Launcher actions are handled by input.cpp, not dispatched; not reported as unmapped.
    if (captions_action(value) || gestures_action(value)) continue;
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

namespace {
constexpr int kStickDeadzone = 13000;  // |axis| beyond this counts as a direction (Xbox range)

int quantize(int value) { return value < -kStickDeadzone ? -1 : (value > kStickDeadzone ? 1 : 0); }
int sign(int value) { return value < 0 ? -1 : (value > 0 ? 1 : 0); }
}  // namespace

AxisEvent track_axes(AxisState& s, int code, int value) {
  switch (code) {
    case ABS_HAT0X:
      s.hat_x = sign(value);
      return AxisEvent::Direction;
    case ABS_HAT0Y:
      s.hat_y = sign(value);
      return AxisEvent::Direction;
    case ABS_X:
      s.stick_x = quantize(value);
      return AxisEvent::Direction;
    case ABS_Y:
      s.stick_y = quantize(value);
      return AxisEvent::Direction;
    case ABS_RX:
      s.rstick_x = value;
      return AxisEvent::RightStick;
    case ABS_RY:
      s.rstick_y = value;
      return AxisEvent::RightStick;
    case ABS_Z:
    case ABS_RZ: {
      bool& latch = code == ABS_Z ? s.lt : s.rt;
      const bool now = trigger_pressed(value, latch);
      const bool edge = now && !latch;
      latch = now;
      if (!edge) return AxisEvent::Trigger;
      return code == ABS_Z ? AxisEvent::LtPressed : AxisEvent::RtPressed;
    }
    default:
      return AxisEvent::Ignored;
  }
}

int normalize_axis(int code, int value, AxisRange r) {
  long lo, hi;
  switch (code) {
    case ABS_X:
    case ABS_Y:
    case ABS_RX:
    case ABS_RY:
      lo = -32768;
      hi = 32767;
      break;
    case ABS_Z:
    case ABS_RZ:
      lo = 0;
      hi = 255;
      break;
    default:
      return value;  // the hat is read by sign alone
  }
  if (r.max <= r.min || (r.min == lo && r.max == hi)) return value;
  const long v = value < r.min ? r.min : (value > r.max ? r.max : value);
  return static_cast<int>(lo + (v - r.min) * (hi - lo) / (static_cast<long>(r.max) - r.min));
}

int skip_action_sign(std::string_view value) {
  if (value == "skip_fwd" || value == "skip_forward") return 1;
  if (value == "skip_back" || value == "skip_backward") return -1;
  return 0;
}

int chapter_action_sign(std::string_view value) {
  if (value == "chapter_fwd" || value == "chapter_forward" || value == "chapter_next") return 1;
  if (value == "chapter_back" || value == "chapter_backward" || value == "chapter_prev") return -1;
  return 0;
}

bool captions_action(std::string_view value) {
  return value == "toggle_captions" || value == "captions";
}

bool gestures_action(std::string_view value) {
  return value == "toggle_gestures" || value == "touch_gestures" || value == "toggle_touch";
}

std::string caption_language_subtag(std::string_view locale) {
  const size_t end = locale.find_first_of("_-.@");
  std::string out = ascii_lower(end == std::string_view::npos ? locale : locale.substr(0, end));
  if (out == "c" || out == "posix") return {};
  return out;
}

FastScrollTick fast_scroll(int rx, int ry, const FastScrollConfig& cfg) {
  if (!cfg.enabled) return {nullptr, 0};

  // Axes are in the Xbox-pad range. Saturate before taking |v|: the minimum -32768 is one past the
  // maximum, so an unclamped full down-left push would compare |x| > |y| and resolve horizontally.
  auto mag = [](int v) {
    if (v < -kAxisMax) v = -kAxisMax;
    if (v > kAxisMax) v = kAxisMax;
    return v < 0 ? -v : v;
  };
  const int ax = mag(rx), ay = mag(ry);
  const int dz = cfg.deadzone < 0 ? 0 : cfg.deadzone;

  // Vertical takes the tie, so a perfectly diagonal push resolves the way rails run.
  const bool vertical = ay >= ax;
  const int m = vertical ? ay : ax;
  // Also guards a degenerate deadzone: with dz >= kAxisMax no `m` can exceed it, so the division
  // below never sees a zero (or negative) span.
  if (m <= dz) return {nullptr, 0};

  const char* key;
  if (vertical)
    key = (ry < 0) ? kUp : kDown;  // evdev Y is inverted: negative is up
  else
    key = (rx < 0) ? kLeft : kRight;

  // Linear in deflection past the deadzone: slow_ms at the edge, fast_ms at full travel.
  const double t = static_cast<double>(m - dz) / static_cast<double>(kAxisMax - dz);
  long interval = cfg.slow_ms + static_cast<long>(t * (cfg.fast_ms - cfg.slow_ms));
  // An inverted config (fast_ms > slow_ms) is allowed. Clamp into the interval's own range so it
  // can never go negative and busy-loop the poll.
  const long lo = cfg.fast_ms < cfg.slow_ms ? cfg.fast_ms : cfg.slow_ms;
  const long hi = cfg.fast_ms < cfg.slow_ms ? cfg.slow_ms : cfg.fast_ms;
  if (interval < lo) interval = lo;
  if (interval > hi) interval = hi;
  if (interval < kMinScrollMs) interval = kMinScrollMs;
  return {key, interval};
}

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

  // build_button_map's unmapped entries lack the layer label; re-prefix them.
  std::vector<std::string> raw;
  out.buttons = build_button_map(button_entries, &raw);
  if (unmapped)
    for (std::string& u : raw) unmapped->push_back(std::format("{}.{}", label, u));
  return out;
}

Keymaps build_keymaps(const KeymapConfig& cfg, std::vector<std::string>* unmapped) {
  Keymaps m;
  auto base = cfg.base;
  if (base.empty()) base = {{"a", "select"}, {"b", "back"}};
  m.base = build_layer(base, "keymap", unmapped);
  m.player = build_layer(cfg.player, "keymap_player", unmapped);
  m.osk = build_layer(cfg.osk, "keymap_osk", unmapped);
  m.lt_mod = build_layer(cfg.lt_mod, "keymap_lt", unmapped);
  m.rt_mod = build_layer(cfg.rt_mod, "keymap_rt", unmapped);
  return m;
}

std::string resolve_button(const Keymaps& maps, int code, Layer layer, bool lt_held, bool rt_held) {
  // A modifier layer absorbs: an unbound control under a held modifier dispatches nothing.
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

Press resolve_press(const Keymaps& maps, int code, Layer layer, bool lt_held, bool rt_held,
                    int captions_code, int gestures_code) {
  if (!active_modifier(maps, lt_held, rt_held)) {
    if (code == captions_code && captions_code >= 0) return {Press::Kind::Captions, {}};
    if (code == gestures_code && gestures_code >= 0) return {Press::Kind::Gestures, {}};
  }
  std::string key = resolve_button(maps, code, layer, lt_held, rt_held);
  if (key.empty()) return {};
  return {Press::Kind::Key, std::move(key)};
}

int find_control_for(const std::vector<std::pair<std::string, std::string>>& keymap,
                     bool (*matches)(std::string_view)) {
  for (const auto& [name, value] : keymap) {
    if (!matches(value)) continue;
    for (const ControlCode& c : kControlCodes)
      if (c.name == name) return c.code;
  }
  return -1;
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

int control_code(std::string_view name) {
  for (const ControlCode& c : kControlCodes)
    if (c.name == name) return c.code;
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

}  // namespace deckback

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

// Analog trigger thresholds (Xbox pad reports 0..255). Hysteresis keeps a half-pulled trigger from
// chattering a key press on every jittering report.
constexpr int kTriggerPress = 192;
constexpr int kTriggerRelease = 64;

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

// Deck control name -> evdev EV_KEY code. Use the LABELLED codes BTN_X/BTN_Y, not the positional
// BTN_NORTH/BTN_WEST: on the Deck's Steam Input virtual xpad the face buttons report by their label
// (physical X -> BTN_X = 0x133, physical Y -> BTN_Y = 0x134), and in <linux/input-event-codes.h>
// BTN_NORTH *is* BTN_X (0x133) while BTN_WEST *is* BTN_Y (0x134). The old
// {"x",BTN_WEST},{"y",BTN_NORTH} therefore SWAPPED them — physical Y hit the "x" action
// (play/pause), voice bound to physical X. Confirmed on-Deck 2026-07-10: log showed `btn 308 ->
// MediaPlayPause` while the user pressed Y.
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
  if (lt_held && !m.lt_mod.empty()) return &m.lt_mod;  // LT wins over RT: deterministic, documented
  if (rt_held && !m.rt_mod.empty()) return &m.rt_mod;
  return nullptr;
}

}  // namespace

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

int skip_action_sign(std::string_view value) {
  if (value == "skip_fwd" || value == "skip_forward") return 1;
  if (value == "skip_back" || value == "skip_backward") return -1;
  return 0;
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
    return ascii_lower(v.substr(b, e - b));
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

}  // namespace deckback

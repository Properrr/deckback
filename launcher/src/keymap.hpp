#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "layers.hpp"

namespace deckback {

// Pure keymap/binding resolution — no evdev, no CDP, no threads. Shared by GamepadInput (input.hpp)
// and the first-run controls card (onboarding.cpp) so the two cannot disagree.

// Resolve one `config/app.json:keymap` value to a DOM key. A value may be a DOM key value directly
// ("Enter", "MediaPlayPause", "c") or a semantic action ("select", "back", "playpause",
// "scrub_back", "scrub_fwd"). Returns "" when the value cannot be mapped to a key this build can
// synthesise — including the launcher actions (toggle_captions, chapter/skip seeks) driven over CDP
// rather than by a DOM key.
std::string resolve_binding(std::string_view value);

// Whether `value` is the touch-gesture toggle action.
bool gestures_action(std::string_view value);

// Whether `value` is the caption-toggle launcher action ("toggle_captions"/"captions"). It has no
// DOM key (youtube.com/tv ignores the desktop `c` hotkey): the input layer intercepts the bound
// control by evdev code and drives config/scripts/toggle_captions.js over CDP. build_button_map()
// skips it so it is neither mapped to a key nor reported as an unmapped binding.
bool captions_action(std::string_view value);

// The primary language subtag of one locale token, lowercased: "en_US.UTF-8" -> "en", "pt-BR" ->
// "pt", "" / "C" / "POSIX" -> "". The caller splits a LANGUAGE colon-list first.
std::string caption_language_subtag(std::string_view locale);

// The current name for a deprecated action alias, or "" when `value` is not deprecated. Deprecated
// names still resolve.
std::string_view deprecated_action_replacement(std::string_view value);

// Arrow key for a D-pad/stick state, or nullptr for "no direction". D-pad wins over stick; vertical
// wins over horizontal. Returned pointers are stable module constants, so callers may compare by
// identity (GamepadInput::set_direction relies on that).
const char* resolve_direction(int hat_x, int hat_y, int stick_x, int stick_y);

// One resolved EV_KEY button binding: evdev code -> DOM key to dispatch.
struct ButtonBinding {
  int code;
  std::string key;
};

// Build the EV_KEY bindings from a config keymap. Control names: a, b, x, y, lb, rb, start, select.
// Triggers (lt/rt) are analog axes, not EV_KEY, so they are NOT returned here — ask
// `resolve_binding` for them directly. Anything unrecognised or unmappable is appended to
// `unmapped` as "name=value".
std::vector<ButtonBinding> build_button_map(
    const std::vector<std::pair<std::string, std::string>>& keymap,
    std::vector<std::string>* unmapped);

// Analog trigger -> pressed, with hysteresis. Xbox-pad triggers report 0..255. Press at >=192,
// release only below 64.
bool trigger_pressed(int value, bool was_pressed);

// Every axis the input layer reads, in its digital form: the D-pad and left stick quantized to
// -1/0/1, the right stick raw (its deflection is the scroll rate), and the two trigger latches. A
// modal surface (the OSD, the first-run card) must keep tracking releases into it without acting on
// them, or a direction released underneath stays "held".
struct AxisState {
  int hat_x = 0, hat_y = 0;
  int stick_x = 0, stick_y = 0;
  int rstick_x = 0, rstick_y = 0;
  bool lt = false, rt = false;
};

// What one EV_ABS event changed. A trigger reports its press edge separately, because that edge is
// the only moment a trigger binding fires.
enum class AxisEvent { Ignored, Direction, RightStick, Trigger, LtPressed, RtPressed };

// Apply one EV_ABS event (value already in the Xbox range, see normalize_axis) to `s`.
AxisEvent track_axes(AxisState& s, int code, int value);

// The range one device reports for one axis (EVIOCGABS). max <= min means unknown.
struct AxisRange {
  int min = 0;
  int max = 0;
};

// Map a raw axis value from its device's range into the Xbox 360 pad's, which every threshold here
// assumes: sticks -32768..32767, triggers 0..255. Steam Input's virtual pad already reports that
// and passes through unchanged, as does an unknown range.
int normalize_axis(int code, int value, AxisRange range);

// The sign of a fixed-interval skip action (+1 forward, -1 back), or 0 when `value` is not one. The
// jump goes through the player over CDP (config/scripts/skip.js), not a DOM key.
int skip_action_sign(std::string_view value);

// Like skip_action_sign, but for chapter-aware seek (chapter_back/chapter_fwd → -1/+1, else 0),
// done by config/scripts/chapter_seek.js, which degrades to a fixed skip when the video has no
// chapters.
int chapter_action_sign(std::string_view value);

// Right-stick fast list traversal. Unlike the digital D-pad/left stick, whose repeat accelerates
// over time, the right stick's rate is analog: deflection sets it directly.
struct FastScrollConfig {
  bool enabled = true;
  int deadzone = 13000;  // |axis| beyond this scrolls (Xbox range ~32767)
  long slow_ms = 200;    // repeat interval at the deadzone edge
  long fast_ms = 45;     // repeat interval at full deflection. Each arrow is two CDP round trips,
                         // so this is a floor, not a target.
};

// One poll-tick decision for the right stick. `key` is nullptr when the stick is idle (inside the
// deadzone, or disabled), in which case `interval_ms` is 0.
struct FastScrollTick {
  const char* key;   // a stable module constant, comparable by identity (as resolve_direction)
  long interval_ms;  // how long until this key should repeat
};

// Resolve raw right-stick axes to a direction and a repeat rate. The dominant (larger-deflection)
// axis wins, vertical taking a tie, and the rate comes from that same axis.
FastScrollTick fast_scroll(int rx, int ry, const FastScrollConfig& cfg);

// One named set of bindings: the base map, a context layer ("player", "osk"), or a modifier layer
// (held LT / RT). Directions are keyed by the *base* arrow they would otherwise produce, so a layer
// can rebind "up" without knowing whether it came from the D-pad or the stick.
struct KeyLayer {
  std::vector<ButtonBinding> buttons;                           // evdev EV_KEY code -> DOM key
  std::vector<std::pair<std::string, std::string>> directions;  // base arrow -> DOM key
  bool empty() const { return buttons.empty() && directions.empty(); }
};

// Raw (unresolved) keymap sections, straight from app.json. `base` is `keymap`; the rest are the
// optional `keymap_player` / `keymap_osk` / `keymap_lt` / `keymap_rt` objects.
struct KeymapConfig {
  std::vector<std::pair<std::string, std::string>> base, player, osk, lt_mod, rt_mod;
};

// The resolved form of KeymapConfig.
struct Keymaps {
  KeyLayer base, player, osk, lt_mod, rt_mod;
};

// Resolve one section. Control names are a, b, x, y, lb, rb, start, select, l3, r3 plus the four
// directions (up, down, left, right). Unrecognised or unmappable entries are appended to `unmapped`
// as "name=value", prefixed with `label`.
KeyLayer build_layer(const std::vector<std::pair<std::string, std::string>>& keymap,
                     std::string_view label, std::vector<std::string>* unmapped);

// Resolve every section. An empty `base` falls back to the built-in defaults (a -> select, b ->
// back).
Keymaps build_keymaps(const KeymapConfig& cfg, std::vector<std::string>* unmapped);

// Resolve a button press to the DOM key to dispatch, or "" for "dispatch nothing".
//
// Precedence: a held modifier layer, then the context layer, then base.
//   * A context layer (player/osk) is an override set — an unbound control falls through to base.
//   * A modifier layer (LT/RT held) absorbs an unbound control, so LT+A never fires a plain A.
// A modifier layer is only active when it is non-empty; otherwise the trigger keeps its own binding
// and is not a modifier at all. LT wins over RT when both are held.
std::string resolve_button(const Keymaps& maps, int code, Layer layer, bool lt_held, bool rt_held);

// Same precedence for a direction. `base_arrow` is what `resolve_direction()` produced. Falls back
// to `base_arrow` itself when nothing rebinds it.
std::string resolve_direction_key(const Keymaps& maps, std::string_view base_arrow, Layer layer,
                                  bool lt_held, bool rt_held);

// What a button press does. The launcher actions (captions, the touch-gesture toggle) are taken by
// evdev code, but only when no modifier layer is held: a held LT/RT layer owns every button.
struct Press {
  enum class Kind { None, Key, Captions, Gestures } kind = Kind::None;
  std::string key;  // for Key
};
Press resolve_press(const Keymaps& maps, int code, Layer layer, bool lt_held, bool rt_held,
                    int captions_code, int gestures_code);

// The evdev EV_KEY code of the control bound to `action`, or -1 when nothing is. `voice_search` has
// no DOM key and is intercepted by the launcher. `show_controls` is accepted only to read old
// configs.
int find_control_for_action(const std::vector<std::pair<std::string, std::string>>& keymap,
                            std::string_view action);

// The same lookup for an action with aliases, matched by its predicate (gestures_action,
// captions_action), so every alias is found.
int find_control_for(const std::vector<std::pair<std::string, std::string>>& keymap,
                     bool (*matches)(std::string_view));

// The evdev EV_KEY code of a Deck control by name ("a", "b", "y", ...), or -1 if no such control.
// Independent of the user's keymap, for fixed-button UIs (e.g. the update card's A/B/Y).
int control_code(std::string_view name);

// Drop an entry so build_button_map() does not report it as an unmapped binding. Only correct when
// the launcher really will handle that action.
std::vector<std::pair<std::string, std::string>> without_action(
    const std::vector<std::pair<std::string, std::string>>& keymap, std::string_view action);

}  // namespace deckback

#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "layers.hpp"

namespace deckback {

// Pure keymap/binding resolution — no evdev, no CDP, no threads. Unit-tested in
// tests/input_test.cpp; the evdev/CDP consumer is GamepadInput (input.hpp), and the first-run
// controls card (onboarding.cpp) reads the same resolution so the two can never disagree.

// Resolve one `config/app.json:keymap` value to a DOM key. A value may be a DOM key value directly
// ("Enter", "MediaPlayPause", "c") — which is what makes the keymap hot-swappable without a rebuild
// — or one of the semantic actions from the design doc's P3 table ("select", "back", "playpause",
// "toggle_captions", "scrub_back", "scrub_fwd"). Returns "" when the value cannot be mapped to a
// key this build can synthesise.
std::string resolve_binding(std::string_view value);

// The current name for a deprecated action alias, or "" when `value` is not deprecated. Deprecated
// names still resolve (a remotely hot-swapped app.json must not break); startup warns once per
// entry.
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

// Build the EV_KEY bindings from a config keymap. Control names are the doc's: a, b, x, y, lb, rb,
// start, select. Triggers (lt/rt) are analog axes, not EV_KEY, so they are NOT returned here — ask
// `resolve_binding` for them directly. Anything unrecognised or unmappable is appended to
// `unmapped` as "name=value" instead of being silently dropped, so startup can warn about it once.
std::vector<ButtonBinding> build_button_map(
    const std::vector<std::pair<std::string, std::string>>& keymap,
    std::vector<std::string>* unmapped);

// Analog trigger -> pressed, with hysteresis so a trigger resting near the threshold cannot
// chatter. Xbox-pad triggers report 0..255. Press at >=192, release only below 64.
bool trigger_pressed(int value, bool was_pressed);

// ---- trigger seek (findings input-ux §18) -------------------------------------------------------

// A skip binding performs a fixed-interval jump through the Leanback player over CDP, NOT a DOM key.
// Returns the sign of the jump (+1 forward, -1 back) for a skip action value, or 0 when `value` is
// not one. Bound only to the analog triggers today; the input layer renders config/scripts/skip.js
// (ScriptLibrary) with the signed interval and evaluates it on the press edge.
int skip_action_sign(std::string_view value);

// Like skip_action_sign, but for chapter-aware seek (chapter_back/chapter_fwd → -1/+1, else 0). The
// input layer renders config/scripts/chapter_seek.js with the direction + the skip_seconds fallback;
// that script jumps to the prev/next chapter boundary (TVHTML5 /next macroMarkersListEntity) and
// degrades to a fixed skip when the video has no chapters.
int chapter_action_sign(std::string_view value);

// ---- right-stick fast traversal (findings input-ux §7) -----------------------------------------

// The right stick is otherwise unused (only its R3 click, as half the touch-lock chord), while
// every console app puts fast list traversal on the second axis. Leanback rails are long and the
// D-pad's time-based acceleration takes seconds to reach the end of one.
//
// The two sticks ramp differently, on purpose. The D-pad/left stick are digital: they know only
// "held", so their repeat rate must accelerate over *time*. The right stick reports deflection, so
// its rate is analog — **the user's thumb is the accelerator**, and a time ramp on top would fight
// the thumb for control of the same variable.
struct FastScrollConfig {
  bool enabled = true;
  int deadzone = 13000;  // |axis| beyond this scrolls (Xbox range ~32767)
  long slow_ms = 200;    // repeat interval at the deadzone edge — a nudge steps deliberately
  long fast_ms = 45;     // repeat interval at full deflection. Each arrow is two CDP round trips,
                         // so this is a floor, not a target.
};

// One poll-tick decision for the right stick. `key` is nullptr when the stick is idle (inside the
// deadzone, or disabled), in which case `interval_ms` is 0.
struct FastScrollTick {
  const char* key;   // a stable module constant, comparable by identity (as resolve_direction)
  long interval_ms;  // how long until this key should repeat
};

// Resolve raw right-stick axes to a direction and a repeat rate. The **dominant** axis wins (larger
// deflection), with vertical taking a tie: unlike the digital resolve_direction, magnitude is
// already meaningful here, so a diagonal push means whichever way the thumb pushed harder. The rate
// comes from that same axis, so a hard push down scrolls fast and a nudge steps once.
FastScrollTick fast_scroll(int rx, int ry, const FastScrollConfig& cfg);

// ---- layered keymaps ----------------------------------------------------------------------------

// One named set of bindings: the base map, a context layer ("player", "osk"), or a modifier layer
// (held LT / RT). Directions are keyed by the *base* arrow they would otherwise produce, so a layer
// can rebind "up" without knowing whether it came from the D-pad or the stick.
struct KeyLayer {
  std::vector<ButtonBinding> buttons;                           // evdev EV_KEY code -> DOM key
  std::vector<std::pair<std::string, std::string>> directions;  // base arrow -> DOM key
  bool empty() const { return buttons.empty() && directions.empty(); }
};

// Raw (unresolved) keymap sections, straight from app.json. `base` is `keymap`; the rest are the
// optional `keymap_player` / `keymap_osk` / `keymap_lt` / `keymap_rt` objects. All are
// hot-swappable.
struct KeymapConfig {
  std::vector<std::pair<std::string, std::string>> base, player, osk, lt_mod, rt_mod;
};

// The resolved form of KeymapConfig.
struct Keymaps {
  KeyLayer base, player, osk, lt_mod, rt_mod;
};

// Resolve one section. Control names are the doc's (a, b, x, y, lb, rb, start, select, l3, r3) plus
// the four directions (up, down, left, right). Unrecognised or unmappable entries are appended to
// `unmapped` as "name=value" rather than silently dropped. `label` prefixes those messages so a
// startup warning says *which* layer is broken.
KeyLayer build_layer(const std::vector<std::pair<std::string, std::string>>& keymap,
                     std::string_view label, std::vector<std::string>* unmapped);

// Resolve every section. An empty `base` falls back to the built-in defaults (a -> select, b ->
// back).
Keymaps build_keymaps(const KeymapConfig& cfg, std::vector<std::string>* unmapped);

// Resolve a button press to the DOM key to dispatch, or "" for "dispatch nothing".
//
// Precedence: a held modifier layer, then the context layer, then base. The two kinds of layer
// differ deliberately:
//   * A **context** layer (player/osk) is an *override set* — an unbound control falls through to
//     base, because browsing bindings mostly still make sense inside the player.
//   * A **modifier** layer (LT/RT held) *absorbs* an unbound control. Falling through would mean
//     LT+A silently fires a plain A, which is the classic modifier bug: the user holds a modifier
//     precisely to signal "not the normal action".
// A modifier layer is only active when it is non-empty; otherwise the trigger keeps its own binding
// and is not a modifier at all. LT wins over RT when both are held, so the result is deterministic.
std::string resolve_button(const Keymaps& maps, int code, Layer layer, bool lt_held, bool rt_held);

// Same precedence for a direction. `base_arrow` is what `resolve_direction()` produced. Falls back
// to `base_arrow` itself when nothing rebinds it — a plain D-pad press stays a plain arrow key.
std::string resolve_direction_key(const Keymaps& maps, std::string_view base_arrow, Layer layer,
                                  bool lt_held, bool rt_held);

// The evdev EV_KEY code of the control bound to `action`, or -1 when nothing is. Some actions have
// no DOM key by design — `voice_search` (input-ux §8.2) and `show_controls` are performed by the
// launcher itself — so they never appear in the button map, and the input layer intercepts their
// press edges directly.
int find_control_for_action(const std::vector<std::pair<std::string, std::string>>& keymap,
                            std::string_view action);

// Drop an entry so build_button_map() does not report it as an unmapped binding. Only correct when
// the launcher really will handle that action — otherwise the startup warning is the honest output,
// because the control genuinely does nothing.
std::vector<std::pair<std::string, std::string>> without_action(
    const std::vector<std::pair<std::string, std::string>>& keymap, std::string_view action);

// Parse a touch-lock chord string ("l3+r3", "select+start", ...) into the two evdev EV_KEY codes it
// names. Control names are the doc's plus l3/r3 (stick clicks). Returns {-1,-1} when the string is
// malformed, names an unknown control, or repeats one control — the caller then disables the chord.
std::pair<int, int> parse_chord(std::string_view chord);

// The lock/unlock policy of the touch-lock chord. Pure and clock-injected.
//
// Locking and unlocking are deliberately NOT symmetric. Locking is what the user asked for and is
// cheap to undo, so it happens on the press. Unlocking must be *deliberate*: `l3+r3` is two stick
// clicks, which a resting thumb finds by accident, and an accidental unlock hands the touchscreen
// back to a palm already resting on the panel — the exact failure the lock exists to prevent. So
// unlock requires holding the chord for `unlock_hold_ms`.
//
// One action per engagement: continuing to hold the chord after it locked must not then unlock it.
class TouchLockChord {
 public:
  enum class Action { None, Lock, Unlock };

  explicit TouchLockChord(long unlock_hold_ms)
      : unlock_hold_ms_(unlock_hold_ms < 0 ? 0 : unlock_hold_ms) {}

  // Both chord buttons held / not held. Locks immediately; arms the unlock hold.
  Action on_chord(bool both_down, long now_ms);
  // Call from the poll loop; matures a pending unlock. A held chord emits no further evdev events.
  Action on_tick(long now_ms);

  bool locked() const { return locked_; }
  // Reconcile with reality when the EVIOCGRAB itself failed: the machine must never claim a lock
  // the kernel refused.
  void set_locked(bool v) { locked_ = v; }

  bool pending() const { return waiting_; }  // an unlock hold is counting down
  long deadline_ms() const { return hold_start_ms_ + unlock_hold_ms_; }

 private:
  long unlock_hold_ms_;
  bool locked_ = false;
  bool engaged_ = false;  // the chord is down and has already been adjudicated
  bool waiting_ = false;  // ...and it is an unlock hold that has not matured
  long hold_start_ms_ = 0;
};

}  // namespace deckback

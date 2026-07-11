#pragma once
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "devtools.hpp"
#include "haptic.hpp"
#include "layers.hpp"
#include "onboarding.hpp"
#include "touch.hpp"
#include "voice.hpp"

namespace deckback {

// ---- pure helpers (no evdev, no CDP) — unit-tested in tests/input_test.cpp
// -----------------------

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

// Runtime touchscreen-lock policy for the input layer (findings input-ux §4). Sourced from Config.
struct TouchConfig {
  bool lock_enabled = false;    // whether the chord toggles the touchscreen lock
  std::string chord = "l3+r3";  // the toggling controller chord
  bool block_initial = false;   // start with the touchscreen already locked
  long unlock_hold_ms = 800;    // hold the chord this long to unlock (locking is immediate)
  bool toast = true;            // announce lock/unlock with an on-screen toast
  bool haptic = true;           // ...and a controller rumble pulse
};

// Phase 3 input (S0.6 mechanism = key injection). Reads the gamepad directly from evdev
// (/dev/input/event*, raw <linux/input.h> — no libevdev dependency) and translates it to the DOM
// key events Leanback navigates with, dispatched over CDP (Input.dispatchKeyEvent, trusted). Under
// Steam Input the readable pad is the virtual "Microsoft X-Box 360 pad"; in desktop mode it's the
// physical controller — we open every gamepad-capable device and merge, so both work.
//
// Mapping is driven by `config/app.json:keymap` (hot-swappable, doc §6). D-pad + left stick always
// produce Arrow keys with auto-repeat + acceleration; face/shoulder buttons and the analog triggers
// come from the keymap. An empty keymap falls back to the built-in defaults (A -> Enter, B ->
// Escape). Uses its own DevToolsClient; content_shell (M114) allows multiple concurrent CDP clients
// per target.
class GamepadInput {
 public:
  // `keymap` carries every layer; an empty `base` selects the built-in defaults. `touch` enables
  // the runtime touchscreen lock (default-constructed = feature off). `layers` is the context
  // source, written by PlayerController's poll thread; nullptr pins the input to the Browse layer.
  // `voice` (nullable, not owned) enables hold-to-talk on whichever control the keymap binds to
  // `voice_search`. nullptr leaves that control unbound and reported at startup, which is the
  // shipped default until the V0 spike proves a soft-mic button exists.
  GamepadInput(std::string host, int port, KeymapConfig keymap = {}, TouchConfig touch = {},
               const LayerState* layers = nullptr, VoiceController* voice = nullptr,
               long voice_hold_ms = 250, FastScrollConfig fast_scroll = {},
               OnboardingController* onboarding = nullptr);
  ~GamepadInput();

  GamepadInput(const GamepadInput&) = delete;
  GamepadInput& operator=(const GamepadInput&) = delete;

  void start();  // launch the input thread
  void stop();   // signal + join (idempotent; also called by the destructor)

 private:
  void loop();
  bool stopping();
  void rescan_devices();  // close everything and (re)open gamepad evdev fds
  // Open gamepad devices that appeared since the last scan, leaving already-open fds (and any held
  // direction) untouched. Without this, a pad plugged in while another is open is never seen.
  void discover_new_devices();
  void close_devices();
  void handle_event(int type, int code, int value);
  void set_direction(
      const char* key);       // arm/replace the auto-repeating directional key (nullptr=clear)
  void dispatch_direction();  // resolve dir_key_ under the current layer/modifiers and send
  void dispatch_key(
      const char* base_arrow);  // resolve one arrow under the layer/modifiers and send
  void update_fast_scroll();    // re-evaluate the right stick after an axis change
  bool handle_chord(int code,
                    int value);  // track/toggle the touch-lock chord; true = event consumed
  // Press/hold/release for hold-to-talk; true = the event was the voice button and is consumed.
  bool handle_voice(int code, int value);
  // While the controls card is up it is modal: it swallows every event, and a button press
  // dismisses it. Returns true when the event was consumed.
  bool handle_onboarding(int type, int code, int value);
  void apply_touch_lock(TouchLockChord::Action a);  // engage/release the grab and report it
  void announce_touch_lock(bool locked);            // toast + rumble; never fails the lock
  Layer layer() const;                              // Browse when no LayerState is attached

  DevToolsClient client_;
  std::vector<int> fds_;
  std::vector<std::string> paths_;  // parallel to fds_; lets discover_new_devices() skip open nodes

  // Runtime touchscreen lock (findings input-ux §4).
  TouchGuard touch_;
  bool block_initial_ = false;
  int chord_a_code_ = -1, chord_b_code_ = -1;  // the two chord buttons (<0 = chord disabled)
  bool chord_a_down_ = false, chord_b_down_ = false;
  TouchLockChord chord_;
  std::string chord_label_;  // as written in config, so the toast can name the real chord
  bool touch_toast_ = true;
  bool touch_haptic_ = true;
  // Rumble needs its own O_RDWR fd on the pad; the input fds are O_RDONLY. Attached lazily on the
  // first pulse, because at construction no device has been discovered yet.
  Haptic haptic_;
  bool haptic_tried_ = false;

  // Resolved bindings (built once from the config keymap at construction).
  Keymaps maps_;
  const LayerState* layers_ = nullptr;  // context source; nullptr = always Browse
  std::string lt_key_, rt_key_;  // analog triggers; empty = unbound. Ignored while that trigger is
                                 // a modifier (i.e. its modifier layer is non-empty).
  bool lt_down_ = false, rt_down_ = false;

  // Hold-to-talk (findings input-ux §13). voice_ is not owned and may be null.
  VoiceController* voice_ = nullptr;
  int voice_code_ = -1;
  HoldToTalk hold_;

  // First-run controls card (findings input-ux §17). Not owned; may be null.
  OnboardingController* onboarding_ = nullptr;
  int help_code_ = -1;  // the control bound to `show_controls`, or -1

  // Directional auto-repeat state. dir_key_ is the *base* arrow (a stable module constant compared
  // by identity); the key actually dispatched is resolved per-repeat, so releasing a modifier or
  // entering the player mid-hold takes effect on the next repeat without re-pressing.
  const char* dir_key_ = nullptr;  // currently-held base arrow, or nullptr
  long next_repeat_ms_ = 0;
  int repeat_count_ = 0;
  // Axis state feeding the resolved direction.
  int hat_x_ = 0, hat_y_ = 0;      // D-pad (-1/0/1)
  int stick_x_ = 0, stick_y_ = 0;  // left stick, quantized to -1/0/1 via deadzone

  // Right stick: fast list traversal (input-ux §7). Kept RAW, unlike the left stick — the
  // deflection *is* the repeat rate, so quantizing it to -1/0/1 would throw away the whole feature.
  // Suppressed while the D-pad/left stick hold a direction, so one physical intent never emits two
  // key streams.
  FastScrollConfig fast_cfg_;
  int rstick_x_ = 0, rstick_y_ = 0;  // raw axis values
  const char* fast_key_ = nullptr;   // currently-scrolling base arrow, or nullptr
  long fast_next_ms_ = 0;

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool started_ = false;
};

}  // namespace deckback

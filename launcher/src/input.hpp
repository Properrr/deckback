#pragma once
#include <string>
#include <vector>

#include "devtools.hpp"
#include "haptic.hpp"
#include "keymap.hpp"
#include "layers.hpp"
#include "onboarding.hpp"
#include "touch.hpp"
#include "voice.hpp"
#include "worker.hpp"

namespace deckback {

class UpdatePromptController;

// Runtime touchscreen-lock policy for the input layer (findings input-ux §4). Sourced from Config.
struct TouchConfig {
  bool lock_enabled = false;    // whether the chord toggles the touchscreen lock
  std::string chord = "l3+r3";  // the toggling controller chord
  bool block_initial = false;   // start with the touchscreen already locked
  long unlock_hold_ms = 800;    // hold the chord this long to unlock (locking is immediate)
  bool toast = true;            // announce lock/unlock with an on-screen toast
  bool haptic = true;           // ...and a controller rumble pulse
};

// Everything GamepadInput is wired to. Bundled so the call site in main() labels what it passes.
//
//   keymap       every layer; an empty `base` selects the built-in defaults (a -> select, b ->
//                back).
//   touch        the runtime touchscreen lock (default-constructed = feature off).
//   fast_scroll  right-stick fast list traversal (input-ux §7).
//   layers       context source (browse/player/osk), written by PlayerController's poll thread;
//                nullptr pins the input to the Browse layer. Not owned.
//   voice        enables hold-to-talk on whichever control the keymap binds to `voice_search`.
//                nullptr leaves that control unbound and reported at startup, which is the shipped
//                default until the V0 spike proves a soft-mic button exists. Not owned.
//   onboarding   the first-run controls card (input-ux §17); nullptr = feature off. Not owned.
struct GamepadOptions {
  KeymapConfig keymap;
  TouchConfig touch;
  FastScrollConfig fast_scroll;
  int skip_seconds = 10;  // ± jump for a trigger bound to skip_back/skip_fwd (input-ux §18)
  const LayerState* layers = nullptr;
  VoiceController* voice = nullptr;
  OnboardingController* onboarding = nullptr;
  UpdatePromptController* update_prompt = nullptr;  // self-update notify UI; null = feature off
};

// Phase 3 input (S0.6 mechanism = key injection). Reads the gamepad directly from evdev
// (/dev/input/event*, raw <linux/input.h> — no libevdev dependency) and translates it to the DOM
// key events Leanback navigates with, dispatched over CDP (Input.dispatchKeyEvent, trusted). Under
// Steam Input the readable pad is the virtual "Microsoft X-Box 360 pad"; in desktop mode it's the
// physical controller — we open every gamepad-capable device and merge, so both work.
//
// Mapping is driven by `config/app.json:keymap` (hot-swappable, doc §6). D-pad + left stick always
// produce Arrow keys with auto-repeat + acceleration; face/shoulder buttons and the analog triggers
// come from the keymap. Uses its own DevToolsClient; content_shell (M114) allows multiple
// concurrent CDP clients per target.
class GamepadInput {
 public:
  GamepadInput(std::string host, int port, GamepadOptions opts = {});
  ~GamepadInput();

  GamepadInput(const GamepadInput&) = delete;
  GamepadInput& operator=(const GamepadInput&) = delete;

  void start();  // launch the input thread
  void stop();   // signal + join (idempotent; also called by the destructor)

 private:
  void loop();
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
  // While the update card is up it is modal: A confirms the update, B closes it, Y ignores the
  // version; every other event is swallowed. Returns true when the event was consumed.
  bool handle_update_card(int type, int code, int value);
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
  // A trigger bound to a seek action (skip_back/skip_fwd or chapter_back/chapter_fwd) carries a
  // prebuilt eval expression instead of a DOM key (input-ux §18): skip.js for a fixed jump,
  // chapter_seek.js for chapter-boundary navigation. Non-empty takes precedence over
  // lt_key_/rt_key_ on the press edge.
  std::string lt_skip_js_, rt_skip_js_;
  bool lt_down_ = false, rt_down_ = false;

  // Hold-to-talk (findings input-ux §13). voice_ is not owned and may be null.
  VoiceController* voice_ = nullptr;
  int voice_code_ = -1;
  HoldToTalk hold_;

  // First-run controls card (findings input-ux §17). Not owned; may be null.
  OnboardingController* onboarding_ = nullptr;
  int help_code_ = -1;  // the control bound to `show_controls`, or -1

  // Self-update notify UI (findings durable/self-update.md). Not owned; may be null. The card is
  // modal with fixed buttons (A confirm, B later, Y ignore) independent of the user's keymap.
  UpdatePromptController* update_prompt_ = nullptr;
  int upd_a_ = -1, upd_b_ = -1, upd_y_ = -1;

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

  WorkerThread worker_;
};

}  // namespace deckback

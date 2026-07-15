#pragma once
#include <atomic>

namespace deckback {

// Which context the user is in. The canonical keymap in findings input-ux §2 is *contextual* — the
// same physical button means different things while browsing, while the player is up, and inside
// the on-screen keyboard — but the input layer dispatched context-free until this landed.
//
// This is deliberately a small, closed enum rather than a free-form string: an unknown context must
// be impossible to express, so a stale config can never select a layer the launcher does not know.
enum class Layer {
  Browse = 0,  // rails/menus — the default, and what we fall back to when the engine is unreachable
  Player,      // the watch view is up
  Osk,         // a text field has focus (Leanback's grid keyboard, search)
};

const char* layer_name(Layer l);

// Resolve the active layer from the two signals the player poll can observe.
//
// The OSK wins over the player: text entry can be open *over* a playing video, and while typing,
// keys must mean characters and edits — never transport controls. When neither signal is set we
// browse. Never guesses: an unreachable engine yields both signals false, i.e. Browse, which is the
// layer whose bindings are the plain verified ones.
Layer resolve_layer(bool player_open, bool text_input_focused);

// The current layer, written by the player-poll thread and read by the input thread. A relaxed
// atomic is the whole synchronization story: a layer read that loses a race by one poll tick
// dispatches one keystroke under the previous layer, which is recoverable, whereas taking a lock on
// the input hot path is not worth it.
class LayerState {
 public:
  Layer get() const { return layer_.load(std::memory_order_relaxed); }

  // Returns true when the layer actually changed (callers log on the edge, not every tick).
  bool set(Layer l) { return layer_.exchange(l, std::memory_order_relaxed) != l; }

  // The raw watch-view signal, independent of which layer owns the keys: the OSK outranks the
  // player in resolve_layer() while a video can still be playing underneath, so overlays that must
  // stay off playback (the update pill/card) key off this, never off `get() == Layer::Player`.
  bool video_up() const { return video_up_.load(std::memory_order_relaxed); }
  void set_video_up(bool v) { video_up_.store(v, std::memory_order_relaxed); }

 private:
  std::atomic<Layer> layer_{Layer::Browse};
  std::atomic<bool> video_up_{false};
};

}  // namespace deckback

#pragma once
#include <atomic>

namespace deckback {

// Which context the user is in; selects the keymap context layer. A closed enum, so a config can
// never select a layer the launcher does not know.
enum class Layer {
  Browse = 0,  // rails/menus — the default, and what we fall back to when the engine is unreachable
  Player,      // the watch view is up
  Osk,         // a text field has focus (Leanback's grid keyboard, search)
};

const char* layer_name(Layer l);

// Resolve the active layer from the two signals the player poll can observe. The OSK wins over the
// player (text entry can be open over a playing video); neither set means Browse.
Layer resolve_layer(bool player_open, bool text_input_focused);

// The current layer, written by the player-poll thread and read by the input thread. Relaxed
// atomics: a read that loses a race dispatches at most one keystroke under the previous layer.
class LayerState {
 public:
  Layer get() const { return layer_.load(std::memory_order_relaxed); }

  // Returns true when the layer actually changed (callers log on the edge, not every tick).
  bool set(Layer l) { return layer_.exchange(l, std::memory_order_relaxed) != l; }

  // The raw watch-view signal, independent of which layer owns the keys (the OSK outranks the
  // player while a video plays underneath). Overlays that must stay off playback key off this,
  // never off `get() == Layer::Player`.
  bool video_up() const { return video_up_.load(std::memory_order_relaxed); }
  void set_video_up(bool v) { video_up_.store(v, std::memory_order_relaxed); }

 private:
  std::atomic<Layer> layer_{Layer::Browse};
  std::atomic<bool> video_up_{false};
};

}  // namespace deckback

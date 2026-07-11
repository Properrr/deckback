#include "layers.hpp"

namespace deckback {

const char* layer_name(Layer l) {
  switch (l) {
    case Layer::Browse:
      return "browse";
    case Layer::Player:
      return "player";
    case Layer::Osk:
      return "osk";
  }
  return "browse";
}

Layer resolve_layer(bool player_open, bool text_input_focused) {
  if (text_input_focused) return Layer::Osk;  // typing outranks transport, even over a live video
  if (player_open) return Layer::Player;
  return Layer::Browse;
}

}  // namespace deckback

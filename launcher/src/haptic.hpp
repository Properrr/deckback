#pragma once
#include <linux/input.h>

#include <cstdint>
#include <string>

namespace deckback {

// Controller rumble over evdev force-feedback (`EVIOCSFF` + an `EV_FF` play event). Requires the
// device opened O_RDWR (the input layer's fds are O_RDONLY, so this attaches its own); under
// Flatpak it needs `--device=input`.

// Pure: build an FF_RUMBLE effect. `id` is -1 to request a new effect slot from the kernel (the
// driver writes the assigned id back). Magnitudes are 0..0xFFFF.
ff_effect make_rumble_effect(int16_t id, uint16_t strong, uint16_t weak, uint16_t duration_ms);

class Haptic {
 public:
  Haptic() = default;
  ~Haptic();
  Haptic(const Haptic&) = delete;
  Haptic& operator=(const Haptic&) = delete;

  // Open `path` O_RDWR and verify it advertises EV_FF/FF_RUMBLE. Returns false (quietly) when the
  // device cannot rumble or cannot be opened for writing.
  bool attach(const std::string& path);
  bool attached() const { return fd_ >= 0; }
  void detach();

  // Upload + play one effect. Best-effort: failures are swallowed.
  void rumble(uint16_t strong, uint16_t weak, uint16_t duration_ms);

 private:
  int fd_ = -1;
  int16_t effect_id_ = -1;  // reused across calls; the kernel slots are finite
};

}  // namespace deckback

#pragma once
#include <linux/input.h>

#include <cstdint>
#include <string>

namespace deckback {

// Controller rumble over evdev force-feedback (`EVIOCSFF` + an `EV_FF` play event).
//
// Chosen over SDL2 deliberately: the launcher's dependency budget is libsystemd/libevdev/libcurl
// and nothing Boost-class (doc §13.2). FF is a dozen lines of ioctl against the pad we already
// open, and under Steam Input the rumble-capable node is the same virtual "Microsoft X-Box 360 pad"
// the input layer reads.
//
// Requires the device opened **O_RDWR** — the input layer's fds are O_RDONLY, so this attaches its
// own. Under Flatpak it needs `--device=input`, which `just install` grants.

// Pure: build an FF_RUMBLE effect. `id` is -1 to request a new effect slot from the kernel (the
// driver writes the assigned id back). Magnitudes are 0..0xFFFF. Exposed for unit testing, because
// the struct layout is easy to fill wrong and a wrong effect fails silently on hardware.
ff_effect make_rumble_effect(int16_t id, uint16_t strong, uint16_t weak, uint16_t duration_ms);

class Haptic {
 public:
  Haptic() = default;
  ~Haptic();
  Haptic(const Haptic&) = delete;
  Haptic& operator=(const Haptic&) = delete;

  // Open `path` O_RDWR and verify it advertises EV_FF/FF_RUMBLE. Returns false (quietly) when the
  // device cannot rumble or cannot be opened for writing — a pad without FF is normal, not an
  // error.
  bool attach(const std::string& path);
  bool attached() const { return fd_ >= 0; }
  void detach();

  // Upload + play one effect. Best-effort: a failure here must never break the action it
  // accompanies.
  void rumble(uint16_t strong, uint16_t weak, uint16_t duration_ms);

 private:
  int fd_ = -1;
  int16_t effect_id_ = -1;  // reused across calls; the kernel slots are finite
};

}  // namespace deckback

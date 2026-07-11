#include "haptic.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>

#include "log.hpp"

namespace deckback {
namespace {

bool test_bit(int bit, const unsigned long* arr) {
  return (arr[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

bool can_rumble(int fd) {
  unsigned long ev[(EV_MAX / (8 * sizeof(long))) + 1] = {};
  if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0) return false;
  if (!test_bit(EV_FF, ev)) return false;
  unsigned long ff[(FF_MAX / (8 * sizeof(long))) + 1] = {};
  if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff) < 0) return false;
  return test_bit(FF_RUMBLE, ff);
}

}  // namespace

ff_effect make_rumble_effect(int16_t id, uint16_t strong, uint16_t weak, uint16_t duration_ms) {
  ff_effect e{};
  e.type = FF_RUMBLE;
  e.id = id;  // -1 asks the kernel to allocate a slot and write the id back
  e.replay.length = duration_ms;
  e.replay.delay = 0;
  e.u.rumble.strong_magnitude = strong;
  e.u.rumble.weak_magnitude = weak;
  return e;
}

Haptic::~Haptic() { detach(); }

bool Haptic::attach(const std::string& path) {
  detach();
  // O_RDWR, unlike the input layer's O_RDONLY fds: uploading an effect is a write to the device.
  // Failing here is ordinary — a pad without FF, or a node we may read but not write — so it is not
  // an error, just no rumble.
  int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) return false;
  if (!can_rumble(fd)) {
    close(fd);
    return false;
  }
  fd_ = fd;
  effect_id_ = -1;
  return true;
}

void Haptic::detach() {
  if (fd_ < 0) return;
  if (effect_id_ >= 0) ioctl(fd_, EVIOCRMFF, static_cast<int>(effect_id_));
  close(fd_);
  fd_ = -1;
  effect_id_ = -1;
}

void Haptic::rumble(uint16_t strong, uint16_t weak, uint16_t duration_ms) {
  if (fd_ < 0) return;
  // Re-upload into the same slot each time (the kernel keeps the id when we pass one back), so a
  // long session cannot exhaust the device's finite effect table.
  ff_effect e = make_rumble_effect(effect_id_, strong, weak, duration_ms);
  if (ioctl(fd_, EVIOCSFF, &e) < 0) {
    // A stale slot after a hotplug/reconnect: retry once as a fresh upload before giving up.
    if (effect_id_ < 0) return;
    effect_id_ = -1;
    e = make_rumble_effect(-1, strong, weak, duration_ms);
    if (ioctl(fd_, EVIOCSFF, &e) < 0) return;
  }
  effect_id_ = e.id;

  input_event play{};
  play.type = EV_FF;
  play.code = static_cast<uint16_t>(e.id);
  play.value = 1;  // play once
  if (write(fd_, &play, sizeof(play)) != static_cast<ssize_t>(sizeof(play))) {
    // Best-effort by contract: the caller is locking a touchscreen, not driving a rumble motor.
  }
}

}  // namespace deckback

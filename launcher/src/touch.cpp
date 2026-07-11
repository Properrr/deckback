#include "touch.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>
#include <format>

#include "log.hpp"

namespace deckback {
namespace {

constexpr uint16_t kVendorFocaltech = 0x2808;
constexpr uint16_t kProductFts3528 = 0x1015;

bool test_bit(int bit, const unsigned long* arr) {
  return (arr[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

// A node is our touchscreen if it either matches the FTS3528 USB id, or is a genuine multitouch
// absolute device whose name looks like the Deck panel. The id match is authoritative; the
// capability check is the fallback for firmware/name drift across SteamOS updates.
bool is_touchscreen(int fd, std::string& name_out) {
  input_id id{};
  const bool id_match = ioctl(fd, EVIOCGID, &id) == 0 && id.vendor == kVendorFocaltech &&
                        id.product == kProductFts3528;

  unsigned long abs[(ABS_MAX / (8 * sizeof(long))) + 1] = {};
  const bool multitouch = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) >= 0 &&
                          test_bit(ABS_MT_POSITION_X, abs) && test_bit(ABS_MT_SLOT, abs);

  char name[256] = {};
  if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) name_out = name;

  if (id_match) return true;
  if (!multitouch) return false;
  // Name-based fallback: accept a multitouch node advertising the Focaltech panel / "touchscreen".
  std::string lower = name_out;
  for (char& ch : lower)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  return lower.find("fts3528") != std::string::npos ||
         lower.find("touchscreen") != std::string::npos;
}

}  // namespace

TouchGuard::TouchGuard() {
  int fd = resolve_and_open();
  if (fd >= 0) {
    available_ = true;
    info(std::format("touch: touchscreen found ({})", node_name_));
    close(fd);
  } else {
    warn("touch: no touchscreen node found (block/unblock will be a no-op until one appears)");
  }
}

TouchGuard::~TouchGuard() { release(); }

int TouchGuard::resolve_and_open() {
  DIR* d = opendir("/dev/input");
  if (!d) return -1;
  int found = -1;
  for (dirent* e; (e = readdir(d)) != nullptr;) {
    if (std::strncmp(e->d_name, "event", 5) != 0) continue;
    const std::string path = std::string("/dev/input/") + e->d_name;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    std::string name;
    if (is_touchscreen(fd, name)) {
      node_name_ = name.empty() ? path : name;
      found = fd;
      break;
    }
    close(fd);
  }
  closedir(d);
  return found;
}

bool TouchGuard::set_blocked(bool block) {
  if (block) {
    if (fd_ >= 0) return true;  // already grabbed
    int fd = resolve_and_open();
    if (fd < 0) {
      warn("touch: cannot block — no touchscreen node found");
      return false;
    }
    if (ioctl(fd, EVIOCGRAB, reinterpret_cast<void*>(1)) != 0) {
      warn("touch: EVIOCGRAB failed (need --device=input?) — touch not blocked");
      close(fd);
      return false;
    }
    fd_ = fd;
    available_ = true;
    info(std::format("touch: blocked ({})", node_name_));
    return true;
  }
  if (fd_ < 0) return true;  // already released
  release();
  info("touch: unblocked");
  return true;
}

void TouchGuard::release() {
  if (fd_ < 0) return;
  ioctl(fd_, EVIOCGRAB, reinterpret_cast<void*>(0));
  close(fd_);
  fd_ = -1;
}

}  // namespace deckback

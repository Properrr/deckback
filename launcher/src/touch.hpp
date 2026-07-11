#pragma once
#include <string>

namespace deckback {

// Runtime touchscreen block/unblock for Game Mode (findings input-ux §4, the explicit requirement).
//
// Mechanism: open the Steam Deck touchscreen evdev node and ioctl(fd, EVIOCGRAB, 1). gamescope /
// libinput read the touchscreen *without* an exclusive grab, so our grab starves them of touch
// input system-wide; EVIOCGRAB(0) / close(fd) restores it instantly, and the kernel auto-releases
// the grab if we crash (fail-safe). This sits below libinput/Xwayland, so it beats xinput / udev /
// gamescope flags. The controller is never touched — only the touchscreen is grabbed.
//
// The node is resolved by identity (Focaltech FTS3528, USB id 2808:1015, multitouch-capable), never
// by a fixed eventN — the number changes across boots/resume. We only hold the fd open while
// blocked, so each block() re-resolves the node, which also covers the post-resume renumbering.
class TouchGuard {
 public:
  TouchGuard();
  ~TouchGuard();

  TouchGuard(const TouchGuard&) = delete;
  TouchGuard& operator=(const TouchGuard&) = delete;

  // True if a touchscreen node was found at least once (probed at construction). block() re-probes,
  // so a device that appears later can still be grabbed even if this returned false at startup.
  bool available() const { return available_; }

  // Grab (block=true) or release (block=false) the touchscreen. Idempotent. Returns whether the
  // requested state was achieved; logs on failure. Re-resolves the node on every grab.
  bool set_blocked(bool block);

  bool blocked() const { return fd_ >= 0; }

 private:
  int resolve_and_open();  // find + open the FTS3528 node (O_RDONLY|CLOEXEC); -1 if not found
  void release();          // ungrab + close if held

  bool available_ = false;
  std::string node_name_;  // human-readable name of the resolved node (for logs)
  int fd_ = -1;            // held open (and grabbed) only while blocked
};

}  // namespace deckback

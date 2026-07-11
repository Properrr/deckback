#pragma once
#include <functional>
#include <memory>

namespace deckback {

// D-Bus / logind integration (doc §6, Phase 6). Backed by libsystemd sd-bus when available;
// compiles to no-op stubs otherwise so the launcher builds on a bare toolchain. The backend runs
// its own event-loop thread — callbacks fire on that thread, so keep them cheap and thread-aware.
class Platform {
 public:
  static std::unique_ptr<Platform> create();
  virtual ~Platform() = default;

  // logind PrepareForSleep: on_suspend fires before sleep, while a delay inhibitor is held (pause /
  // checkpoint here); on_resume fires after wake (rebuild audio/network, nudge player). Set these
  // before real suspend activity begins.
  virtual void on_suspend(std::function<void()> cb) = 0;
  virtual void on_resume(std::function<void()> cb) = 0;

  // Hold/release a logind "idle" block inhibitor while the player is playing (never dim mid-video).
  // Safe to call from any thread; the change is reconciled on the backend thread.
  virtual void set_idle_inhibited(bool inhibited) = 0;

  // True if a live logind connection was established.
  virtual bool backend_live() const = 0;

  // Live self-test (needs a reachable system bus): confirm the connection + inhibitor path work.
  // Returns 0 on success. Used by `deckback-launcher --selftest-dbus`.
  virtual int selftest() = 0;

  static bool backend_available();
};

}  // namespace deckback

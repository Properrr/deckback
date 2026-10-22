#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>

#include "devtools.hpp"
#include "scripts.hpp"

namespace deckback {

// The lifecycle every CDP-injected overlay shares: draw a node into the live Leanback document,
// believe it is there, and notice when the page takes it away. Leanback navigates, reloads, and
// rebuilds `documentElement` underneath us, silently deleting whatever we appended, so the painted
// belief must be re-checked with lost().

// Rate-limits a periodic check (e.g. a CDP health probe from the input loop).
class Throttle {
 public:
  explicit Throttle(int period_ms) : period_(period_ms) {}

  // True at most once per period, re-arming as it returns.
  bool due() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_ < period_) return false;
    last_ = now;
    return true;
  }

 private:
  std::chrono::milliseconds period_;
  std::chrono::steady_clock::time_point last_{};
};

// How often to ask the page whether our node is still there.
inline constexpr int kOverlayReconcileMs = 750;

class PageOverlay {
 public:
  // `element_id` is the DOM id the show script assigns, and the handle used to detect removal.
  PageOverlay(DevToolsClient& client, std::string element_id,
              int reconcile_ms = kOverlayReconcileMs)
      : client_(client), element_id_(std::move(element_id)), probe_(reconcile_ms) {}

  PageOverlay(const PageOverlay&) = delete;
  PageOverlay& operator=(const PageOverlay&) = delete;

  // Render `script_name` and inject it. Returns whether the engine accepted the injection; only
  // then is the overlay recorded as painted.
  bool draw(std::string_view script_name, const ScriptParams& params = {});

  // As draw(), for JS the caller already rendered.
  bool draw_js(std::string_view js);

  // Inject `hide_script_name` and clear the belief. Idempotent: no round trip when already hidden.
  void hide(std::string_view hide_script_name);

  // Our own belief. Written by whichever thread draws, read lock-free by the input thread on every
  // event; a one-tick race is recoverable.
  bool painted() const { return painted_.load(std::memory_order_relaxed); }

  // Set the belief without touching the page (e.g. on a reload signal). Returns whether it changed.
  bool set_painted(bool v) { return painted_.exchange(v, std::memory_order_relaxed) != v; }

  // Throttled `getElementById` probe. True when the page removed our node while we believed it
  // painted; the belief is then cleared. An unreachable engine changes nothing.
  bool lost();

 private:
  DevToolsClient& client_;
  std::string element_id_;
  Throttle probe_;
  std::atomic<bool> painted_{false};
};

}  // namespace deckback

#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>

#include "devtools.hpp"
#include "scripts.hpp"

namespace deckback {

// The lifecycle every CDP-injected overlay shares: draw a node into the live Leanback document,
// believe it is there, and notice when the page takes it away.
//
// The noticing is the part worth sharing. We do not own the document — Leanback navigates, reloads,
// and rebuilds `documentElement` underneath us, and every such event silently deletes whatever we
// appended. An overlay that only tracks its own draw/hide calls will therefore believe it is on
// screen when it is not, and anything gated on that belief (modal input capture, "already drawn, no
// need to redraw") is then wrong until something else disturbs it.
//
// Only the OSD implemented this. The first-run controls card did not, so a reload while the card
// was up left `visible()` true with nothing painted: the input layer went on swallowing every
// event and pinning the D-pad, and the user's next button press was eaten dismissing a card that
// had already vanished. Same document, same failure mode, one implementation now.

// Rate-limits a periodic check. A health probe costs a CDP round trip, and the input loop can tick
// every few milliseconds while a direction auto-repeats.
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

// How often to ask the page whether our node is still there. Fast enough that a stale belief is
// corrected within a beat of a reload, slow enough not to be a poll loop.
inline constexpr int kOverlayReconcileMs = 750;

class PageOverlay {
 public:
  // `element_id` is the DOM id the show script assigns, and the handle used to detect removal.
  PageOverlay(DevToolsClient& client, std::string element_id,
              int reconcile_ms = kOverlayReconcileMs)
      : client_(client), element_id_(std::move(element_id)), probe_(reconcile_ms) {}

  PageOverlay(const PageOverlay&) = delete;
  PageOverlay& operator=(const PageOverlay&) = delete;

  // Render `script_name` and inject it. Returns whether the engine accepted the injection, and only
  // then records the overlay as painted: caching a FAILED draw would leave a startup or reload
  // transport blip looking like a successful paint forever after.
  bool draw(std::string_view script_name, const ScriptParams& params = {});

  // As draw(), for a caller that already rendered its JS through a pure, separately-tested helper
  // (overlay_js et al.) rather than naming a script here.
  bool draw_js(std::string_view js);

  // Inject `hide_script_name` and clear the belief. Idempotent — no round trip when already hidden,
  // which matters because this is called per button press while a modal overlay is up.
  void hide(std::string_view hide_script_name);

  // Our own belief. Written by whichever thread draws, read by the input thread every event, so it
  // is atomic for the same reason LayerState is: losing the race by one tick is recoverable, taking
  // a lock on the input hot path is not.
  bool painted() const { return painted_.load(std::memory_order_relaxed); }

  // Set the belief without touching the page. For a caller that learns from elsewhere (the
  // navigator's reload signal) that the document was replaced.
  bool set_painted(bool v) { return painted_.exchange(v, std::memory_order_relaxed) != v; }

  // Throttled `getElementById` probe. True when the page has taken our node away while we believed
  // it painted — the belief is cleared as this returns. An UNREACHABLE engine is a transport blip,
  // not a lost overlay, so it changes nothing: only a live reply saying "absent" counts.
  bool lost();

 private:
  DevToolsClient& client_;
  std::string element_id_;
  Throttle probe_;
  std::atomic<bool> painted_{false};
};

}  // namespace deckback

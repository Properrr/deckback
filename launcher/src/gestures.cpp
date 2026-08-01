#include "gestures.hpp"

#include <format>

#include "json.hpp"
#include "log.hpp"
#include "scripts.hpp"

namespace deckback {
namespace {

// The page's drain entry point. Guarded because a reload can land between our poll and the
// document-start injection, and calling through a missing object would throw in the page and come
// back as an evaluation failure indistinguishable from a dead engine.
constexpr const char* kDrainExpr =
    "(function(){var g=window.__deckbackGestures;"
    "return g?JSON.stringify(g.drain()):'';})()";

std::string configure_expr(const GestureRouterConfig& c) {
  return std::format(
      "(function(){{var g=window.__deckbackGestures;if(!g)return false;"
      "g.configure({{stepPx:{},maxSteps:{},edgePx:{},longPressMs:{},doubleTapMs:{}}});"
      "return true;}})()",
      c.step_px, c.max_steps, c.edge_px, c.long_press_ms, c.double_tap_ms);
}

std::string set_enabled_expr(bool on) {
  return std::format(
      "(function(){{var g=window.__deckbackGestures;"
      "return g?g.setEnabled({}):false;}})()",
      on ? "true" : "false");
}

const json::Value* member(const std::vector<json::Member>& obj, std::string_view key) {
  for (const json::Member& kv : obj)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

}  // namespace

std::string gesture_key(std::string_view kind, std::string_view dir) {
  if (kind == "arrow") {
    if (dir == "up") return "ArrowUp";
    if (dir == "down") return "ArrowDown";
    if (dir == "left") return "ArrowLeft";
    if (dir == "right") return "ArrowRight";
    return {};
  }
  // No touch Back path exists otherwise (input-ux §7); the left-edge swipe is the only one.
  if (kind == "back") return "Escape";
  // A tap activates the focused tile / toggles the player chrome, exactly as the A button does.
  if (kind == "tap") return "Enter";
  return {};
}

int gesture_seek_sign(std::string_view kind, int dir) {
  if (kind != "seek") return 0;
  return dir > 0 ? 1 : (dir < 0 ? -1 : 0);
}

int gesture_hold_phase(std::string_view kind, bool on) {
  if (kind != "hold") return 0;
  return on ? 1 : -1;
}

double hold_rate_for_zone(std::string_view zone, double fast, double slow) {
  return zone == "left" ? slow : fast;  // centre, or a script that sent no zone, speeds up
}

std::string seek_hud_text(int seconds) {
  if (seconds == 0) return {};
  return (seconds > 0 ? "+" : "-") + std::to_string(seconds < 0 ? -seconds : seconds) + " s";
}

std::string hold_hud_text(double rate) {
  // Trailing zeros read badly at 34px: "0.5x", not "0.500000x".
  return std::format("{:g}x", rate);
}

GestureBatch parse_drain(std::string_view json_text) {
  GestureBatch out;
  if (json_text.empty()) return out;  // no router in the page yet — not an empty queue
  json::ParseResult pr = json::parse(json_text);
  if (!pr.ok()) return out;
  const std::vector<json::Member>* obj = pr.value->as_object();
  if (!obj) return out;

  out.ok = true;
  if (const json::Value* v = member(*obj, "configured"))
    out.configured = v->as_bool().value_or(false);
  if (const json::Value* v = member(*obj, "enabled")) out.enabled = v->as_bool().value_or(true);

  const json::Value* q = member(*obj, "q");
  const std::vector<json::Value>* arr = q ? q->as_array() : nullptr;
  if (!arr) return out;
  for (const json::Value& e : *arr) {
    const std::vector<json::Member>* eo = e.as_object();
    if (!eo) continue;
    Gesture g;
    if (const json::Value* v = member(*eo, "g"))
      if (const std::string* s = v->as_string()) g.kind = *s;
    if (g.kind.empty()) continue;
    if (const json::Value* v = member(*eo, "dir")) {
      if (const std::string* s = v->as_string()) g.dir = *s;
      // `dir` is a string for arrows and a signed number for seeks. Same key, two types, because
      // that is what reads naturally in the page; decoding both here keeps that from leaking.
      else if (auto n = v->as_number())
        g.seek_dir = *n > 0 ? 1 : (*n < 0 ? -1 : 0);
    }
    if (const json::Value* v = member(*eo, "zone"))
      if (const std::string* z = v->as_string()) g.zone = *z;
    if (const json::Value* v = member(*eo, "n"))
      if (auto n = v->as_number()) g.n = *n >= 1 ? static_cast<int>(*n) : 1;
    if (const json::Value* v = member(*eo, "on")) g.on = v->as_bool().value_or(false);
    out.gestures.push_back(std::move(g));
  }
  return out;
}

GestureRouter::GestureRouter(GestureRouterConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.poll_ms <= 0) cfg_.poll_ms = 50;
}

GestureRouter::~GestureRouter() { stop(); }

void GestureRouter::start() {
  worker_.start([this] { loop(); });
}

void GestureRouter::stop() { worker_.stop(); }

bool GestureRouter::set_enabled(bool on) {
  enabled_.store(on, std::memory_order_release);
  push_enabled_.store(true, std::memory_order_release);
  return on;
}

void GestureRouter::show_hud(DevToolsClient& client, std::string_view text, std::string_view side,
                             int ms) {
  ScriptParams p;
  p.set("text", text);
  p.set("side", side);
  p.set("ms", static_cast<long>(ms));
  ScriptLibrary::instance().invoke(client, "gesture_hud", p);
}

void GestureRouter::configure_page(DevToolsClient& client) {
  client.eval_void(configure_expr(cfg_));
}

void GestureRouter::act(DevToolsClient& client, const Gesture& g) {
  if (const std::string key = gesture_key(g.kind, g.dir); !key.empty()) {
    client.dispatch_key(key);
    return;
  }
  if (int sign = gesture_seek_sign(g.kind, g.seek_dir); sign != 0) {
    // Mobile's accumulating seek. Each double-tap jumps a CONSTANT skip_seconds; what accumulates
    // is the indicator, which shows the running total for the whole burst (10, 20, 30, 40) so the
    // user reads where they have got to rather than a "+10 s" that says nothing about the four
    // taps before it. Seeking the total instead would jump 10+20+30+40 = 100s for four taps.
    const int steps = g.n < 1 ? 1 : g.n;
    ScriptParams p;
    // The same player call the trigger bindings use, so touch and the controller cannot drift into
    // two different seek behaviours (input-ux §12 is about exactly that kind of split).
    p.set("delta", static_cast<long>(sign * cfg_.skip_seconds));
    ScriptLibrary::instance().invoke(client, "skip", p);
    show_hud(client, seek_hud_text(sign * cfg_.skip_seconds * steps), sign > 0 ? "right" : "left",
             900);
    return;
  }
  if (int phase = gesture_hold_phase(g.kind, g.on); phase != 0) {
    // Mobile's hold-to-scrub. Unblocked by P12.0b, which measured <video>.playbackRate holding
    // through real playback and the player advertising [0.25 .. 2].
    const double rate = hold_rate_for_zone(g.zone, cfg_.hold_rate, cfg_.hold_slow_rate);
    ScriptParams p;
    if (phase > 0) {
      p.set("rate", rate);
      p.set("stop", false);
    } else {
      p.set("stop", true);
    }
    ScriptLibrary::instance().invoke(client, "hold_scrub", p);
    // ms 0 pins the indicator for the whole hold; the release hides it with an empty text.
    if (phase > 0)
      show_hud(client, hold_hud_text(rate), g.zone.empty() ? "center" : g.zone, 0);
    else
      show_hud(client, "", "center", 0);
    return;
  }
  // A script newer than this binary can emit a gesture we have no action for. Ignoring it is the
  // designed outcome (scripts are hot-swappable, doc §6); misfiring on it would not be.
  static bool warned = false;
  if (!warned) {
    warn(std::format(
        "gestures: ignoring unknown gesture '{}' (page script newer than the launcher?)", g.kind));
    warned = true;
  }
}

void GestureRouter::loop() {
  DevToolsClient client(cfg_.cdp_host, cfg_.cdp_port);
  bool announced = false;
  for (;;) {
    if (push_enabled_.exchange(false, std::memory_order_acq_rel)) {
      const bool on = enabled_.load(std::memory_order_acquire);
      client.eval_void(set_enabled_expr(on));
      if (!on) {
        // Turning the router off DISCARDS its queue, so a hold in progress would never deliver its
        // release and the video would stay at 2x (or 0.5x) for good -- pressing Y to stop touch
        // doing things would leave the most visible thing touch had done. Stop unconditionally: the
        // script is a no-op when nothing is held, and this path must not depend on the queue it
        // just threw away.
        ScriptParams stop;
        stop.set("stop", true);
        ScriptLibrary::instance().invoke(client, "hold_scrub", stop);
        show_hud(client, "", "center", 0);
      }
    }

    std::optional<std::string> raw = client.eval_string(kDrainExpr);
    if (raw && !raw->empty()) {
      GestureBatch batch = parse_drain(*raw);
      if (batch.ok) {
        if (!announced) {
          info("gestures: touch gesture router connected to the page");
          announced = true;
        }
        // A reload reinstalls the script with its built-in defaults, and there is no reload signal
        // the poll thread can trust (the same reason PageOverlay re-probes rather than remembers).
        // The answer rides along with every drain instead.
        if (!batch.configured) {
          configure_page(client);
          if (!enabled_.load(std::memory_order_acquire)) client.eval_void(set_enabled_expr(false));
        }
        for (const Gesture& g : batch.gestures) act(client, g);
      }
    }
    if (worker_.wait_or_stop(cfg_.poll_ms)) break;
  }
}

}  // namespace deckback

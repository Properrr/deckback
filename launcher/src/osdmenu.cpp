#include "osdmenu.hpp"

#include <chrono>
#include <format>
#include <optional>

#include "caption_settings.hpp"
#include "log.hpp"

namespace deckback {

std::string osd_status_line(std::string_view local_version, std::string_view available_version,
                            bool has_update) {
  if (!available_version.empty())
    return std::format("v{} is available. You have v{}.", available_version, local_version);
  // An update is known (the portal saw a newer commit) but its version/changelog hasn't arrived yet
  // — the GitHub fetch is independent of availability and can lag or fail. Never claim "latest"
  // while the buttons still offer "Update now".
  if (has_update) return std::format("A newer version is available. You have v{}.", local_version);
  // UpdateMonitor publishes only positive availability. A false bit means it has not observed an
  // update, not that it has completed a "you're current" check, so it cannot honestly say latest.
  return "Update status is not available.";
}

std::vector<std::pair<std::string, std::string>> osd_update_buttons(bool has_update) {
  if (!has_update) return {};
  return {{"update.confirm", "Update now"}, {"update.ignore", "Ignore this version"}};
}

OsdVerdict parse_verdict(std::string_view v) {
  if (v == "close") return {OsdVerdict::Kind::Close, {}};
  if (v == "gone" || v.empty()) return {OsdVerdict::Kind::Gone, {}};
  constexpr std::string_view kAction = "action:";
  if (v.substr(0, kAction.size()) == kAction)
    return {OsdVerdict::Kind::Action, std::string(v.substr(kAction.size()))};
  constexpr std::string_view kApply = "apply:";
  if (v.substr(0, kApply.size()) == kApply)
    return {OsdVerdict::Kind::Apply, std::string(v.substr(kApply.size()))};
  return {OsdVerdict::Kind::Consumed, {}};
}

OsdMenuController::OsdMenuController(OsdMenuConfig cfg)
    : cfg_(std::move(cfg)), client_(cfg_.cdp_host, cfg_.cdp_port) {}

std::string OsdMenuController::eval_op(DevToolsClient& client, const ScriptParams& params) {
  std::string js = ScriptLibrary::instance().render("osd", params);
  if (js.empty()) return "gone";
  return client.eval_string(js).value_or("gone");
}

bool OsdMenuController::inject_open(DevToolsClient& client) {
  std::vector<std::pair<std::string, std::string>> keys;
  for (const ControlRow& r : controls_overlay_rows(cfg_.overlay))
    keys.push_back({r.control, r.action});

  bool has_update;
  std::string status, notes;
  {
    std::lock_guard lk(model_mu_);
    has_update = has_update_;
    status = status_;
    notes = notes_;
  }
  if (status.empty()) status = "Update status is not available.";

  const AboutInfo& ab = cfg_.about;
  std::vector<std::pair<std::string, std::string>> links;
  if (!ab.homepage.empty()) links.push_back({"Project", ab.homepage});
  if (!ab.help.empty()) links.push_back({"Support", ab.help});

  ScriptParams pm;
  pm.set("op", std::string_view("open")).set("tab", std::string_view("settings")).set("keys", keys);
  if (cfg_.captions)
    pm.set_raw("cc", cfg_.captions->osd_model_json());
  else
    pm.set_raw("cc", std::string_view("null"));
  pm.set("upd_has", has_update)
      .set("upd_status", status)
      .set("upd_notes", notes)
      .set("upd_buttons", osd_update_buttons(has_update))
      .set("about_name", ab.name.empty() ? std::string("Deckback") : ab.name)
      .set("about_summary", ab.summary)
      .set("about_desc", ab.description)
      .set("about_author", ab.developer)
      .set("about_version", cfg_.local_version)
      .set("about_features", ab.features)
      .set("about_links", links);
  return eval_op(client, pm) == "ok";
}

bool OsdMenuController::open_menu() {
  if (!inject_open(client_)) {
    warn("osd: could not open the menu (engine unreachable)");
    return false;
  }
  open_.store(true, std::memory_order_release);
  info("osd: menu opened");
  return true;
}

void OsdMenuController::close_menu() {
  eval_op(client_, ScriptParams().set("op", std::string_view("close")));
  open_.store(false, std::memory_order_release);
}

std::string OsdMenuController::exec(std::string_view cmd) {
  const std::string v =
      eval_op(client_, ScriptParams().set("op", std::string_view("cmd")).set("cmd", cmd));
  const OsdVerdict pv = parse_verdict(v);
  switch (pv.kind) {
    case OsdVerdict::Kind::Gone:
      open_.store(false, std::memory_order_release);
      break;
    case OsdVerdict::Kind::Close:
      close_menu();
      break;
    case OsdVerdict::Kind::Action:
      close_menu();
      if (pv.action == "update.confirm" && cfg_.on_update_confirm)
        cfg_.on_update_confirm();
      else if (pv.action == "update.ignore" && cfg_.on_update_ignore)
        cfg_.on_update_ignore();
      break;
    case OsdVerdict::Kind::Apply:
      if (cfg_.captions) cfg_.captions->apply_action(pv.action);
      break;
    case OsdVerdict::Kind::Consumed:
      break;
  }
  return v;
}

void OsdMenuController::tick(bool on_watch) {
  const bool just_reloaded = reloaded_.exchange(false, std::memory_order_acquire);
  if (just_reloaded) button_shown_ = false;

  if (open_.load(std::memory_order_acquire)) {
    if (on_watch)
      close_menu();  // a video came up under the open menu
    else
      reconcile_open();  // enforce capture <=> paint: the menu we capture for must still be painted
  } else if (just_reloaded) {
    // Not captured, but a reload can leave a keep-alive'd node painted with no owner — keys would
    // pass through a visible menu. Sweep it (no-op if nothing is there).
    eval_op(client_, ScriptParams().set("op", std::string_view("close")));
  }

  const bool want = !on_watch;
  if (want) reconcile_button();
  if (want && (!button_shown_ || badge_dirty_))
    draw_button();
  else if (!want && button_shown_)
    hide_button();
}

void OsdMenuController::reconcile_button() {
  if (!button_shown_) return;
  // Navigator only announces a transition from a non-app URL to the TV app. A same-URL reload
  // (including the common wake path) leaves it announced, but has still wiped documentElement and
  // the Settings button. Probe the actual paint periodically so our local flag cannot pin it gone.
  const auto now = std::chrono::steady_clock::now();
  if (now - last_button_reconcile_ < std::chrono::milliseconds(750)) return;
  last_button_reconcile_ = now;
  const std::optional<bool> present =
      client_.eval_bool("/*osd-button-state*/!!document.getElementById('__deckback_settings_btn')");
  if (present && !*present) button_shown_ = false;
}

void OsdMenuController::reconcile_open() {
  // Throttled: the input loop can tick every few ms while a direction auto-repeats, but this is a
  // health check, not per-frame work.
  const auto now = std::chrono::steady_clock::now();
  if (now - last_reconcile_ < std::chrono::milliseconds(750)) return;
  last_reconcile_ = now;

  const std::string js =
      ScriptLibrary::instance().render("osd", ScriptParams().set("op", std::string_view("state")));
  if (js.empty()) return;
  const std::optional<std::string> st = client_.eval_string(js);
  if (!st) return;  // engine unreachable: a transport blip, not a lost menu — retry later
  if (*st != "gone") return;  // a live state string, or 'detached' (keep-alive re-appends): ours
  // The page was reloaded out from under the captured menu (JS context wiped, so no keep-alive can
  // bring it back). Release capture so input is never trapped behind an absent menu. The paint is
  // already gone with the old document, so this restores capture <=> paint.
  open_.store(false, std::memory_order_release);
}

void OsdMenuController::draw_button() {
  bool has_update;
  {
    std::lock_guard lk(model_mu_);
    has_update = has_update_;
  }
  const bool drawn = ScriptLibrary::instance().invoke(
      client_, "osd_button",
      ScriptParams().set("label", std::string_view("Settings")).set("badge", has_update));
  // Do not cache a failed draw: a startup/reload transport blip must retry on the next input tick.
  button_shown_ = drawn;
  if (drawn) badge_dirty_ = false;
}

void OsdMenuController::hide_button() {
  ScriptLibrary::instance().invoke(client_, "osd_button_hide");
  button_shown_ = false;
}

void OsdMenuController::set_update_model(bool has_update, std::string_view status,
                                         std::string_view notes) {
  std::lock_guard lk(model_mu_);
  has_update_ = has_update;
  status_ = std::string(status);
  notes_ = std::string(notes);
  badge_dirty_ = true;
}

bool OsdMenuController::update_available() const {
  std::lock_guard lk(model_mu_);
  return has_update_;
}

void OsdMenuController::on_page_reloaded() {
  // Just flag it; the input-thread tick() is the single owner of open_ and the OSD's CDP client and
  // reconciles capture <=> paint from there. The old code re-injected here on a second CDP client
  // (racing the input thread) AND only ran when the navigator saw a not-app -> app transition, so a
  // same-URL reload — the common case — silently left the menu captured with nothing painted.
  reloaded_.store(true, std::memory_order_release);
}

}  // namespace deckback

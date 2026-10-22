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
  // Update known, version not fetched yet: never claim "latest" while offering "Update now".
  if (has_update) return std::format("A newer version is available. You have v{}.", local_version);
  // A false bit means "no update observed", not "checked and current".
  return "Update status is not available.";
}

std::vector<std::pair<std::string, std::string>> osd_update_buttons(bool has_update, bool can_check,
                                                                    bool can_quit) {
  if (can_quit) return {{"update.quit", "Close now"}};
  if (has_update)
    return {{"update.confirm", "Update now"}, {"update.ignore", "Ignore this version"}};
  if (can_check) return {{"update.check", "Check for updates"}};
  return {};
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
  if (v == "hold") return {OsdVerdict::Kind::Hold, {}};
  if (v == kOsdUnreachable) return {OsdVerdict::Kind::Unreachable, {}};
  return {OsdVerdict::Kind::Consumed, {}};
}

OsdMenuController::OsdMenuController(OsdMenuConfig cfg)
    : cfg_(std::move(cfg)),
      client_(cfg_.cdp_host, cfg_.cdp_port),
      button_(client_, kOsdButtonElementId) {}

std::optional<std::string> OsdMenuController::eval_op(DevToolsClient& client,
                                                      const ScriptParams& params) {
  std::string js = ScriptLibrary::instance().render("osd", params);
  if (js.empty()) return "gone";  // no osd script: no menu can be on screen
  return client.eval_string(js);
}

bool OsdMenuController::inject_open(DevToolsClient& client) {
  std::vector<std::pair<std::string, std::string>> keys;
  for (const ControlRow& r : controls_overlay_rows(cfg_.overlay))
    keys.push_back({r.control, r.action});

  bool has_update, can_check, can_quit;
  std::string status, notes;
  {
    std::lock_guard lk(model_mu_);
    has_update = has_update_;
    can_check = can_check_;
    can_quit = can_quit_;
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
      .set("upd_buttons", osd_update_buttons(has_update, can_check, can_quit))
      .set("about_name", ab.name.empty() ? std::string("Deckback") : ab.name)
      .set("about_summary", ab.summary)
      .set("about_desc", ab.description)
      .set("about_author", ab.developer)
      .set("about_version", cfg_.local_version)
      .set("about_features", ab.features)
      .set("about_links", links)
      .set("exit_enabled", cfg_.exit_enabled && static_cast<bool>(cfg_.on_exit))
      .set("hold_ms", cfg_.exit_hold_ms);
  return eval_op(client, pm).value_or("") == "ok";
}

void OsdMenuController::fire_exit() {
  if (!cfg_.on_exit) return;
  info("osd: exit confirmed (hold complete)");
  cfg_.on_exit();
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
  const std::optional<std::string> reply =
      eval_op(client_, ScriptParams().set("op", std::string_view("cmd")).set("cmd", cmd));
  // No answer says nothing about the menu, which is most likely still painted: keep capture and
  // let reconcile_open() ask the page.
  if (!reply) return std::string(kOsdUnreachable);
  const std::string& v = *reply;
  const OsdVerdict pv = parse_verdict(v);
  switch (pv.kind) {
    case OsdVerdict::Kind::Gone:
      open_.store(false, std::memory_order_release);
      break;
    case OsdVerdict::Kind::Close:
      close_menu();
      break;
    case OsdVerdict::Kind::Action:
      // "Check for updates" is a query: the menu stays up and the answer patches it in place.
      if (pv.action == "update.check") {
        if (cfg_.on_update_check) cfg_.on_update_check();
        break;
      }
      close_menu();
      if (pv.action == "update.confirm" && cfg_.on_update_confirm)
        cfg_.on_update_confirm();
      else if (pv.action == "update.ignore" && cfg_.on_update_ignore)
        cfg_.on_update_ignore();
      else if (pv.action == "update.quit" && cfg_.on_update_quit)
        cfg_.on_update_quit();
      break;
    case OsdVerdict::Kind::Apply:
      if (cfg_.captions) cfg_.captions->apply_action(pv.action);
      break;
    case OsdVerdict::Kind::Hold:
      // The input layer owns the hold deadline (it sees the release edge); nothing to do here.
      break;
    case OsdVerdict::Kind::Unreachable:  // a page string cannot spell it; only the !reply path
    case OsdVerdict::Kind::Consumed:
      break;
  }
  return v;
}

void OsdMenuController::tick(bool on_watch) {
  const bool just_reloaded = reloaded_.exchange(false, std::memory_order_acquire);
  if (just_reloaded) button_.set_painted(false);

  if (open_.load(std::memory_order_acquire) &&
      model_dirty_.exchange(false, std::memory_order_acq_rel)) {
    push_update_model();
  }

  if (open_.load(std::memory_order_acquire)) {
    // The menu may stay up over playback; a video appearing underneath does not close it.
    reconcile_open();  // enforce capture <=> paint: the menu we capture for must still be painted
  } else if (just_reloaded) {
    // Not captured, but a reload can leave a keep-alive'd node painted with no owner — keys would
    // pass through a visible menu. Sweep it (no-op if nothing is there).
    eval_op(client_, ScriptParams().set("op", std::string_view("close")));
  }

  const bool want = !on_watch;
  // lost() catches a same-URL reload, which wipes the button without any Navigator signal.
  if (want) button_.lost();
  if (want && (!button_.painted() || badge_dirty_))
    draw_button();
  else if (!want && button_.painted())
    hide_button();
}

void OsdMenuController::reconcile_open() {
  // Throttled: the input loop can tick every few ms during auto-repeat.
  if (!menu_probe_.due()) return;

  const std::string js =
      ScriptLibrary::instance().render("osd", ScriptParams().set("op", std::string_view("state")));
  if (js.empty()) return;
  const std::optional<std::string> st = client_.eval_string(js);
  if (!st) return;            // engine unreachable: a transport blip, not a lost menu — retry later
  if (*st != "gone") return;  // a live state string, or 'detached' (keep-alive re-appends): ours
  // The page reloaded under the captured menu: release capture so input is not trapped.
  open_.store(false, std::memory_order_release);
}

void OsdMenuController::draw_button() {
  bool has_update;
  {
    std::lock_guard lk(model_mu_);
    has_update = has_update_;
  }
  // A failed draw is not cached, so it retries on the next tick.
  if (button_.draw(
          "osd_button",
          ScriptParams().set("label", std::string_view("Settings")).set("badge", has_update)))
    badge_dirty_ = false;
}

void OsdMenuController::hide_button() { button_.hide("osd_button_hide"); }

void OsdMenuController::set_update_model(bool has_update, bool can_check, bool can_quit,
                                         std::string_view status, std::string_view notes) {
  {
    std::lock_guard lk(model_mu_);
    has_update_ = has_update;
    can_check_ = can_check;
    can_quit_ = can_quit;
    status_ = std::string(status);
    notes_ = std::string(notes);
    badge_dirty_ = true;
  }
  model_dirty_.store(true, std::memory_order_release);
}

// Patch an already-open menu with the current model (input thread).
void OsdMenuController::push_update_model() {
  bool has_update, can_check, can_quit;
  std::string status;
  {
    std::lock_guard lk(model_mu_);
    has_update = has_update_;
    can_check = can_check_;
    can_quit = can_quit_;
    status = status_;
  }
  ScriptParams pm;
  pm.set("op", std::string_view("upd"))
      .set("upd_has", has_update)
      .set("upd_status", status)
      .set("upd_buttons", osd_update_buttons(has_update, can_check, can_quit));
  const std::optional<std::string> r = eval_op(client_, pm);
  if (!r)
    model_dirty_.store(true, std::memory_order_release);  // unanswered: push it again next tick
  else if (*r == "gone")
    open_.store(false, std::memory_order_release);
}

bool OsdMenuController::update_available() const {
  std::lock_guard lk(model_mu_);
  return has_update_;
}

void OsdMenuController::on_page_reloaded() {
  // Only flag it: the input-thread tick() owns open_ and the CDP client.
  reloaded_.store(true, std::memory_order_release);
}

}  // namespace deckback

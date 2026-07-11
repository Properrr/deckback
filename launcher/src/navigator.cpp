#include "navigator.hpp"

#include <chrono>
#include <ctime>
#include <format>

#include "av1_steering_js.hpp"  // GENERATED from config/av1_steering.js
#include "errorpage.hpp"
#include "log.hpp"
#include "no_pointer_js.hpp"  // GENERATED from config/no_pointer.js

namespace deckback {
namespace {

long now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1'000'000L;
}

// scheme://host of a URL (the security origin), for Browser.grantPermissions. "" if unparseable.
std::string origin_of(const std::string& url) {
  auto s = url.find("://");
  if (s == std::string::npos) return {};
  auto slash = url.find('/', s + 3);
  return slash == std::string::npos ? url : url.substr(0, slash);
}

}  // namespace

Navigator::Navigator(std::string host, int port, std::string user_agent, std::string url,
                     int poll_ms, NavPolicy policy)
    : client_(std::move(host), port),
      user_agent_(std::move(user_agent)),
      url_(std::move(url)),
      poll_ms_(poll_ms < 250 ? 250 : poll_ms),
      policy_(policy) {}

Navigator::~Navigator() { stop(); }

void Navigator::start() {
  {
    std::lock_guard lk(mu_);
    if (started_) return;
    started_ = true;
    stop_ = false;
  }
  thread_ = std::thread([this] { loop(); });
}

void Navigator::stop() {
  {
    std::lock_guard lk(mu_);
    if (!started_) return;
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard lk(mu_);
  started_ = false;
}

bool Navigator::wait_or_stop(int ms) {
  std::unique_lock lk(mu_);
  return cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stop_; });
}

void Navigator::loop() {
  // "Already on the app" needle: the configured URL's host+path, minus scheme and any
  // query/fragment. e.g. https://www.youtube.com/tv -> www.youtube.com/tv, which the loaded
  // ".../tv#/" contains but a desktop redirect (".../?app=desktop") or about:blank does not.
  std::string needle = url_;
  if (auto s = needle.find("://"); s != std::string::npos) needle.erase(0, s + 3);
  if (auto q = needle.find_first_of("?#"); q != std::string::npos) needle.erase(q);
  while (!needle.empty() && needle.back() == '/') needle.pop_back();

  // Prime the sticky TV UA before the first navigation so YouTube sees it on the initial load.
  if (!user_agent_.empty()) {
    while (!client_.set_user_agent_override(user_agent_)) {
      if (wait_or_stop(poll_ms_)) return;
    }
    info("navigator: TV UA override armed");
  }

  // Startup CDP policy, installed before the first navigation so it takes effect on the initial
  // load. Both are sticky in DevToolsClient (re-armed across Leanback's target teardown).
  if (policy_.steer_av1) {
    if (client_.add_script_on_new_document(kAv1SteeringScript))
      info("navigator: AV1 codec steering installed (AV1 -> unsupported; VP9/H.264 unaffected)");
  }
  if (policy_.disable_pointer) {
    // Option A of disable_touch: swallow every pointer/mouse/touch event in the page so a finger
    // (delivered as synthetic mouse events under gamescope) cannot navigate. Sticky across
    // Leanback's target teardown, same as the steering script. Option B (gamescope hover) is in
    // main.cpp.
    if (client_.add_script_on_new_document(kNoPointerScript))
      info("navigator: touch disabled — page pointer/mouse/touch events swallowed");
  }
  if (policy_.mic_autogrant) {
    const std::string origin = origin_of(url_);
    if (!origin.empty() && client_.grant_permissions(origin, "audioCapture"))
      info(std::format("navigator: mic auto-granted for {}", origin));
  }

  bool announced = false;
  do {
    if (showing_error_) {
      // The user asked, or the backoff expired. A retry the user requested resets the backoff: they
      // presumably just fixed the Wi-Fi, and making them wait 30 s to find out is hostile.
      if (take_retry_request(client_)) {
        info("navigator: retry requested from the error page");
        fail_attempt_ = 0;
        if (try_navigate()) announced = false;
      } else if (now_ms() >= next_retry_ms_) {
        if (try_navigate()) announced = false;
      } else if (client_.eval_bool(kIsErrorPageExpr) == false) {
        // The engine was restarted (watchdog) or navigated out from under us, so the page we were
        // waiting on no longer exists — and with it the retry button the user is looking for.
        // `== false` and not `!`: an unreachable engine answers nullopt, and must not count as
        // gone.
        info("navigator: error page vanished — re-navigating");
        try_navigate();  // succeeds onto the app, or re-draws the error page
      }
      continue;
    }

    auto here = client_.eval_string("document.location.href");
    if (here) {
      const bool on_app = !needle.empty() && here->find(needle) != std::string::npos;
      if (!on_app) {
        info(std::format("navigator: engine at '{}' — navigating to {}", *here, url_));
        if (try_navigate()) announced = false;
      } else if (!announced) {
        info("navigator: TV app loaded");
        announced = true;
        fail_attempt_ = 0;
        if (on_app_loaded_) on_app_loaded_();
      }
    }
  } while (!wait_or_stop(poll_ms_));
}

bool Navigator::try_navigate() {
  const DevToolsClient::NavStatus st = client_.navigate_checked(url_);
  if (!st.sent)
    return false;  // engine unreachable: transport, not a page failure. Retry next poll.

  if (st.error_text.empty()) {
    if (showing_error_) info("navigator: recovered — back on the TV app");
    showing_error_ = false;
    fail_attempt_ = 0;
    return true;
  }

  // A failed navigation. Without this branch the loop would find `document.location.href` still set
  // to the URL we asked for, conclude the TV app had loaded, and leave the user on Chromium's
  // desktop error interstitial forever — a page with no focusable control in an app with no
  // keyboard.
  warn(std::format("navigator: navigation to {} failed: {}", url_, st.error_text));

  const long delay = retry_backoff_ms(fail_attempt_, policy_.retry_min_ms, policy_.retry_max_ms);
  next_retry_ms_ = now_ms() + delay;
  ++fail_attempt_;

  if (policy_.error_page) {
    ErrorPageInfo info_page;
    if (!policy_.error_title.empty()) info_page.title = policy_.error_title;
    info_page.detail = st.error_text;
    info_page.hint =
        policy_.error_hint.empty() ? classify_net_error(st.error_text) : policy_.error_hint;
    info_page.url = url_;
    if (show_error_page(client_, info_page))
      showing_error_ = true;
    else
      warn("navigator: could not draw the error page — engine unreachable");
  }
  info(std::format("navigator: retrying in {} ms", delay));
  return false;
}

}  // namespace deckback

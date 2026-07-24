#include "updateprompt.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>

#include "fileio.hpp"
#include "http.hpp"
#include "json.hpp"
#include "log.hpp"
#include "osdmenu.hpp"
#include "overlay.hpp"
#include "scripts.hpp"
#include "updater.hpp"

namespace deckback {
namespace {

constexpr std::size_t kMaxNotesLen = 4000;
constexpr long kReleasesTimeoutSec = 6;
constexpr int kDeployToastMs = 9000;
constexpr std::string_view kIgnoredStatus =
    "This available version is ignored. A newer version will appear here.";
constexpr std::string_view kReleaseNotesFallback =
    "Release notes: github.com/properrr/deckback/releases";
constexpr std::string_view kOffStatus = "Updates are off.";

// Deploy-phase copy. Every one of these is only ever shown after the portal actually said so — the
// bug this replaces was announcing success at the moment the button was pressed, which is before
// anything is known.
constexpr std::string_view kRequestedStatus = "Updating\xE2\x80\xA6 this can take a minute.";
constexpr std::string_view kDoneStatus = "Update installed. Restart Deckback to start using it.";
constexpr std::string_view kEmptyStatus = "You are on the latest version.";
// The refusal a user cannot retry their way out of, so the text is the workaround rather than an
// apology. The command is the same one-liner the README documents for installing; run from the host
// it updates through flatpak directly, which is not subject to the portal's permission rule.
constexpr std::string_view kPermissionBlockedStatus =
    "This version needs new system permissions, so Deckback cannot install it by itself. In "
    "Desktop "
    "Mode, run:  curl -fsSL https://properrr.github.io/deckback/install.sh | bash";
constexpr std::string_view kPermissionBlockedToast =
    "Update needs new permissions \xE2\x80\x94 install it from Desktop Mode (see Settings "
    "\xE2\x96\xB8 Updates)";
constexpr std::string_view kFailedToast =
    "Update failed \xE2\x80\x94 see Settings \xE2\x96\xB8 Updates";

// Split "v0.0.10" into {0,0,10}. `valid` is false if any field is empty or non-numeric — a
// malformed string (e.g. a "-rc1" pre-release suffix) must never read as newer than a real release.
std::vector<long> parse_version_fields(std::string_view v, bool& valid) {
  valid = true;
  std::vector<long> out;
  if (!v.empty() && (v.front() == 'v' || v.front() == 'V')) v.remove_prefix(1);
  if (v.empty()) {
    valid = false;
    return out;
  }
  size_t i = 0;
  while (i <= v.size()) {
    const size_t dot = v.find('.', i);
    const size_t end = (dot == std::string_view::npos) ? v.size() : dot;
    const std::string_view field = v.substr(i, end - i);
    if (field.empty()) {
      valid = false;
      return out;
    }
    long n = 0;
    for (char c : field) {
      if (c < '0' || c > '9') {
        valid = false;
        return out;
      }
      n = n * 10 + (c - '0');
    }
    out.push_back(n);
    if (dot == std::string_view::npos) break;
    i = dot + 1;
  }
  return out;
}

// The string member `key` of a release object, or "" when absent or not a string. `\r` is dropped
// so notes are \n-terminated (GitHub serves CRLF bodies).
std::string release_string(const json::Value& obj, std::string_view key) {
  const json::Value* v = obj.find(key);
  if (!v) return {};
  const std::string* s = v->as_string();
  if (!s) return {};
  std::string out;
  out.reserve(s->size());
  for (char c : *s)
    if (c != '\r') out += c;
  return out;
}

std::string trim(std::string s) {
  const auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && ws(static_cast<unsigned char>(s[i]))) ++i;
  return s.substr(i);
}

void erase_all(std::string& s, std::string_view token) {
  for (size_t p = s.find(token); p != std::string::npos; p = s.find(token, p))
    s.erase(p, token.size());
}

// GET a GitHub API URL. Short timeout so a slow/absent network never stalls the menu.
std::optional<std::string> github_get(const std::string& url) {
  return http_get(HttpRequest{.url = url,
                              .timeout_seconds = kReleasesTimeoutSec,
                              .user_agent = "deckback-updater/1",  // GitHub requires a UA
                              .headers = {"Accept: application/vnd.github+json"}});
}

}  // namespace

// ---- pure helpers -------------------------------------------------------------------------------

int compare_versions(std::string_view a, std::string_view b) {
  bool va = false, vb = false;
  const std::vector<long> fa = parse_version_fields(a, va);
  const std::vector<long> fb = parse_version_fields(b, vb);
  if (!va && !vb) return 0;
  if (!va) return -1;  // malformed compares older than any real version
  if (!vb) return 1;
  const size_t n = std::max(fa.size(), fb.size());
  for (size_t i = 0; i < n; ++i) {
    const long x = i < fa.size() ? fa[i] : 0;
    const long y = i < fb.size() ? fb[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

std::vector<ReleaseNote> parse_github_releases(const std::string& text) {
  std::vector<ReleaseNote> out;
  const json::ParseResult parsed = json::parse(text);
  if (!parsed.ok()) {
    warn(std::format("update: GitHub releases JSON did not parse (line {}: {}) — no changelog",
                     parsed.error.line, parsed.error.message));
    return out;
  }
  const std::vector<json::Value>* arr = parsed.value->as_array();
  if (!arr) return out;  // the API returns an array; an object here is an error payload
  for (const json::Value& item : *arr) {
    if (!item.is_object()) continue;
    std::string version = release_string(item, "tag_name");
    if (version.empty()) continue;  // a release with no tag is unusable
    if (version.front() == 'v' || version.front() == 'V') version.erase(0, 1);
    ReleaseNote n;
    n.version = std::move(version);
    n.title = release_string(item, "name");
    n.body = trim(release_string(item, "body"));
    out.push_back(std::move(n));
  }
  return out;
}

std::string notes_to_plain(std::string_view markdown) {
  std::string out;
  int blank_run = 0;
  size_t i = 0;
  while (i <= markdown.size()) {
    const size_t eol = markdown.find('\n', i);
    const size_t end = (eol == std::string_view::npos) ? markdown.size() : eol;
    std::string line(markdown.substr(i, end - i));
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();

    // Leading '#'s of an ATX heading ("### Added" -> "Added").
    size_t h = 0;
    while (h < line.size() && line[h] == '#') ++h;
    if (h > 0 && h <= 6 && h < line.size() && line[h] == ' ') line = line.substr(h + 1);

    // List marker "- " / "* " / "+ " (any indent) -> "• ".
    size_t s = 0;
    while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
    if (s + 1 < line.size() && (line[s] == '-' || line[s] == '*' || line[s] == '+') &&
        line[s + 1] == ' ')
      line = line.substr(0, s) + "\xE2\x80\xA2" + line.substr(s + 1);

    erase_all(line, "**");  // bold
    erase_all(line, "__");  // bold / underline
    erase_all(line, "`");   // inline code fences

    if (line.find_first_not_of(" \t") == std::string::npos) {
      ++blank_run;  // collapse runs of blank lines into a single separator
    } else {
      if (!out.empty()) out += (blank_run > 0) ? "\n\n" : "\n";
      out += line;
      blank_run = 0;
    }
    if (eol == std::string_view::npos) break;
    i = end + 1;
  }
  return trim(std::move(out));
}

ChangelogView summarize_releases(const std::vector<ReleaseNote>& notes,
                                 std::string_view local_version, std::size_t max_len) {
  ChangelogView view;
  std::vector<const ReleaseNote*> newer;
  for (const ReleaseNote& n : notes)
    if (version_newer(n.version, local_version)) newer.push_back(&n);
  std::sort(newer.begin(), newer.end(), [](const ReleaseNote* a, const ReleaseNote* b) {
    return compare_versions(a->version, b->version) > 0;  // newest first
  });
  if (newer.empty()) return view;
  view.available_version = newer.front()->version;

  std::string acc;
  for (const ReleaseNote* n : newer) {
    const std::string head = n->title.empty() ? ("v" + n->version) : n->title;
    if (!acc.empty()) acc += "\n\n";
    acc += head;
    const std::string body = notes_to_plain(n->body);  // CHANGELOG markdown -> clean 10-foot text
    if (!body.empty()) {
      acc += "\n";
      acc += body;
    }
    if (acc.size() >= max_len) break;
  }
  if (acc.size() > max_len) {
    acc.resize(max_len);
    acc += "\n…";
  }
  view.notes = std::move(acc);
  return view;
}

std::string release_notes_for(const std::vector<ReleaseNote>& notes, std::string_view version,
                              std::size_t max_len) {
  for (const ReleaseNote& n : notes) {
    if (compare_versions(n.version, version) != 0) continue;
    std::string out = n.title.empty() ? ("v" + n.version) : n.title;
    const std::string body = notes_to_plain(n.body);
    if (!body.empty()) {
      out += "\n";
      out += body;
    }
    if (out.size() > max_len) {
      out.resize(max_len);
      out += "\n…";
    }
    return out;
  }
  return {};
}

UpdatesView updates_view(DeployPhase phase, std::string_view error_message, bool update_available,
                         std::string_view available_version, std::string_view local_version,
                         std::string_view idle_status, bool updater_present) {
  UpdatesView v;
  switch (phase) {
    case DeployPhase::Requested:
      // No buttons: the portal is mid-transaction and a second Update() would be ignored anyway.
      v.status = std::string(kRequestedStatus);
      return v;
    case DeployPhase::Done:
      // Nothing more to do here — the new commit binds on the next launch, not on another press.
      v.status = std::string(kDoneStatus);
      return v;
    case DeployPhase::Empty:
      // The portal really did compare against the remote, so this is the one place we may say
      // "latest" outright.
      v.status = std::string(kEmptyStatus);
      v.can_check = updater_present;
      return v;
    case DeployPhase::PermissionBlocked:
      // Deliberately offers nothing: every button here would fail identically, and an enabled
      // control that cannot work is worse than none.
      v.status = std::string(kPermissionBlockedStatus);
      return v;
    case DeployPhase::Failed:
      v.status =
          error_message.empty()
              ? "The update could not be installed. It will be retried later."
              : std::string("The update could not be installed: ") + std::string(error_message);
      v.can_check = updater_present;  // transient causes (network, remote) are worth retrying
      return v;
    case DeployPhase::Idle:
      break;
  }
  if (!updater_present) {
    v.status = std::string(kOffStatus);
    return v;
  }
  if (update_available) {
    v.status = osd_status_line(local_version, available_version, true);
    v.has_update = true;
    return v;
  }
  v.status = std::string(idle_status);
  v.can_check = true;
  return v;
}

std::string deploy_toast(DeployPhase phase) {
  switch (phase) {
    case DeployPhase::Done:
      return "Deckback update installed \xE2\x80\x94 restart to apply.";
    case DeployPhase::PermissionBlocked:
      return std::string(kPermissionBlockedToast);
    case DeployPhase::Failed:
      return std::string(kFailedToast);
    // Empty answers a question the user explicitly asked, so it needs an answer. It was silent at
    // first on the reasoning that the Updates tab would show it — but a check used to close the
    // menu, so the reply arrived with nothing on screen and the button looked inert. The menu now
    // stays open and this only fires when it is NOT (see the caller): the panel says it otherwise.
    case DeployPhase::Empty:
      return std::string(kEmptyStatus);
    // Never terminal, nothing to announce.
    case DeployPhase::Requested:
    case DeployPhase::Idle:
      return {};
  }
  return {};
}

NotifyDecision decide_notification(std::string_view remote_commit,
                                   std::string_view card_shown_commit,
                                   std::string_view dot_dismissed_commit) {
  NotifyDecision d;
  d.show_card = !remote_commit.empty() && remote_commit != card_shown_commit;
  d.show_dot = !remote_commit.empty() && remote_commit != dot_dismissed_commit;
  return d;
}

std::string update_card_marker_path(std::string_view state_dir) {
  return std::string(state_dir) + "/update_card_shown_v1";
}
std::string update_dot_marker_path(std::string_view state_dir) {
  return std::string(state_dir) + "/update_dot_dismissed_v1";
}

std::string read_update_marker(const std::string& path) { return read_marker(path); }

bool write_update_marker(const std::string& path, const std::string& value) {
  // A read-only state dir must not crash and must not nag every launch (the mild failure: the card
  // may auto-show / the dot may reappear again next boot).
  return write_marker(path, value, "the update prompt state was not saved");
}

// ---- controller ---------------------------------------------------------------------------------

UpdatePromptController::UpdatePromptController(UpdatePromptConfig cfg)
    : cfg_(std::move(cfg)), client_(cfg_.cdp_host, cfg_.cdp_port) {}

UpdatePromptController::~UpdatePromptController() {
  if (fetch_thread_.joinable()) fetch_thread_.join();
}

bool UpdatePromptController::update_available() const {
  return cfg_.state != nullptr && cfg_.state->available();
}

void UpdatePromptController::tick(bool on_watch) {
  (void)on_watch;
  if (!cfg_.osd) return;
  // Unconditional: the changelog is now "what's new", not just "what you'd be getting", so it must
  // be there whether or not an update is pending. One fetch per process (kick_changelog CASes).
  kick_changelog();

  DeployPhase phase = DeployPhase::Idle;
  std::string deploy_error;
  bool fresh = false;
  {
    std::lock_guard lk(deploy_mu_);
    phase = deploy_phase_;
    deploy_error = deploy_error_;
    fresh = std::exchange(deploy_fresh_, false);
  }
  // Exactly once per verdict, and only from here: this is the input thread, which owns client_.
  // A toast is the fallback surface: when the menu is open the Updates panel is patched in place
  // and a toast on top of it would just be the same sentence twice.
  if (fresh && !(cfg_.osd && cfg_.osd->open())) {
    if (const std::string t = deploy_toast(phase); !t.empty())
      show_toast(client_, t, kDeployToastMs);
  }

  if (cfg_.state && cfg_.state->available()) {
    const std::string commit = cfg_.state->commit();
    if (!commit.empty() && commit != last_commit_) {
      last_commit_ = commit;
      const NotifyDecision d =
          decide_notification(commit, read_update_marker(update_card_marker_path(cfg_.state_dir)),
                              read_update_marker(update_dot_marker_path(cfg_.state_dir)));
      notify_ = d.show_dot;  // an un-ignored update is available
      hidden_status_ = notify_ ? "" : std::string(kIgnoredStatus);
    }
  }

  const ChangelogView cv = current_changelog();
  const bool announce = notify_ && phase == DeployPhase::Idle;
  const std::string idle =
      (!hidden_status_.empty() && phase == DeployPhase::Idle) ? hidden_status_ : cfg_.idle_status;
  const UpdatesView v = updates_view(phase, deploy_error, announce, cv.available_version,
                                     cfg_.local_version, idle, cfg_.updater != nullptr);

  // Prefer the pending release's notes; fall back to the running version's, so the tab is never
  // empty. Only link out when the fetch produced nothing at all.
  std::string notes = cv.notes.empty() ? installed_notes_ : cv.notes;
  if (notes.empty() && fetch_done()) notes = std::string(kReleaseNotesFallback);
  feed(v.has_update, v.can_check, v.status, notes);
}

void UpdatePromptController::feed(bool has_update, bool can_check, const std::string& status,
                                  const std::string& notes) {
  if (fed_valid_ && has_update == fed_has_ && can_check == fed_check_ && status == fed_status_ &&
      notes == fed_notes_)
    return;
  fed_valid_ = true;
  fed_has_ = has_update;
  fed_check_ = can_check;
  fed_status_ = status;
  fed_notes_ = notes;
  if (cfg_.osd) cfg_.osd->set_update_model(has_update, can_check, status, notes);
}

void UpdatePromptController::kick_changelog() {
  int expected = 0;
  if (!fetch_state_.compare_exchange_strong(expected, 1)) return;  // already fetching or done
  if (!http_available()) {
    fetch_state_.store(2, std::memory_order_release);  // no libcurl: done, empty -> fallback link
    return;
  }
  fetch_thread_ = std::thread([this] {
    ChangelogView v;
    std::string installed;
    if (const auto json = github_get(cfg_.releases_url)) {
      const std::vector<ReleaseNote> notes = parse_github_releases(*json);
      v = summarize_releases(notes, cfg_.local_version, kMaxNotesLen);
      installed = release_notes_for(notes, cfg_.local_version, kMaxNotesLen);
    } else {
      warn("update: could not fetch release notes from GitHub — showing a link instead");
    }
    cached_ = std::move(v);
    installed_notes_ = std::move(installed);
    fetch_state_.store(2, std::memory_order_release);
  });
}

bool UpdatePromptController::fetch_done() const {
  return fetch_state_.load(std::memory_order_acquire) == 2;
}

ChangelogView UpdatePromptController::current_changelog() const {
  if (fetch_done()) return cached_;
  return {};  // not ready yet: the menu shows the releases-link fallback
}

void UpdatePromptController::request_deploy(const char* why) {
  {
    std::lock_guard lk(deploy_mu_);
    deploy_phase_ = DeployPhase::Requested;
    deploy_error_.clear();
    deploy_fresh_ = false;  // Requested never toasts; the tab shows it
  }
  notify_ = false;
  hidden_status_.clear();
  if (cfg_.updater) cfg_.updater->request_update();
  info(std::string("update: ") + why + " — deploy requested");
}

// Deliberately says nothing about the outcome. The portal has not answered yet, and claiming
// success here is exactly the bug a user hit on v0.0.7: a "will apply after restart" toast over a
// deploy the portal had already refused. The verdict arrives via on_deploy_result.
void UpdatePromptController::confirm_update() { request_deploy("user confirmed"); }

void UpdatePromptController::check_now() { request_deploy("user asked to check"); }

void UpdatePromptController::on_deploy_result(UpdateProgress progress,
                                              const std::string& error_name,
                                              const std::string& error_message) {
  DeployPhase phase = DeployPhase::Idle;
  switch (progress) {
    case UpdateProgress::Done:
      phase = DeployPhase::Done;
      break;
    case UpdateProgress::Empty:
      phase = DeployPhase::Empty;
      break;
    case UpdateProgress::Failed:
      phase = is_permission_change_failure(error_name, error_message)
                  ? DeployPhase::PermissionBlocked
                  : DeployPhase::Failed;
      break;
    case UpdateProgress::Running:
    case UpdateProgress::Unknown:
      return;  // not terminal
  }
  std::lock_guard lk(deploy_mu_);
  deploy_phase_ = phase;
  deploy_error_ = error_message;
  deploy_fresh_ = true;
}

void UpdatePromptController::ignore_version() {
  notify_ = false;
  hidden_status_ = std::string(kIgnoredStatus);
  if (cfg_.state) write_update_marker(update_dot_marker_path(cfg_.state_dir), cfg_.state->commit());
  info("update: user ignored this version — hidden until a newer release");
}

}  // namespace deckback

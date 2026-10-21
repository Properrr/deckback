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
#include "util.hpp"

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
constexpr std::string_view kCheckingStatus = "Checking for updates\xE2\x80\xA6";
constexpr std::string_view kRequestedStatus = "Updating\xE2\x80\xA6 this can take a minute.";
constexpr std::string_view kDoneStatus =
    "Update installed. Restart Deckback to start using it: choose Close now, then open it again "
    "from your Library.";
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
constexpr std::string_view kNoAnswerError = "the update service did not answer";
constexpr std::string_view kFailedToast =
    "Update failed \xE2\x80\x94 see Settings \xE2\x96\xB8 Updates";
constexpr std::size_t kOstreeChecksumLen = 64;

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

std::string parse_ref_commit(std::string_view ref_body) {
  const auto ws = [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
  while (!ref_body.empty() && ws(ref_body.front())) ref_body.remove_prefix(1);
  while (!ref_body.empty() && ws(ref_body.back())) ref_body.remove_suffix(1);
  if (ref_body.size() != kOstreeChecksumLen) return {};
  for (char c : ref_body)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
  return std::string(ref_body);
}

RefCheck decide_ref_check(std::string_view ref_body, std::string_view running_commit) {
  const std::string remote = parse_ref_commit(ref_body);
  if (remote.empty() || parse_ref_commit(running_commit).empty()) return RefCheck::Unknown;
  return remote == running_commit ? RefCheck::UpToDate : RefCheck::UpdateAvailable;
}

std::string deploy_progress_status(std::uint32_t op, std::uint32_t n_ops, std::uint32_t percent) {
  percent = std::min<std::uint32_t>(percent, 100);
  if (n_ops > 1)
    return std::format("Updating\xE2\x80\xA6 step {} of {} ({}%)", std::min(op + 1, n_ops), n_ops,
                       percent);
  return std::format("Updating\xE2\x80\xA6 {}%", percent);
}

UpdatesView updates_view(DeployPhase phase, std::string_view error_message, bool update_available,
                         std::string_view available_version, std::string_view local_version,
                         std::string_view idle_status, bool updater_present,
                         std::string_view progress_status) {
  UpdatesView v;
  switch (phase) {
    case DeployPhase::Checking:
      // A read of the published ref, a second or two; nothing to press until it answers.
      v.status = std::string(kCheckingStatus);
      return v;
    case DeployPhase::Requested:
      // No buttons: the portal is mid-transaction and a second Update() would be ignored anyway.
      v.status =
          progress_status.empty() ? std::string(kRequestedStatus) : std::string(progress_status);
      return v;
    case DeployPhase::Done:
      // The new commit binds on the next launch, not on another press, so the one useful action is
      // leaving. Deckback cannot start itself again from Game Mode; the Library tile does that.
      v.status = std::string(kDoneStatus);
      v.can_quit = true;
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
              ? "The update could not be installed. Check for updates to try again."
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
    case DeployPhase::Checking:
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
  if (probe_thread_.joinable()) probe_thread_.join();
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
  // One probe per launch, so a release published since the last session is announced now rather
  // than whenever the portal's shared timer next fires (0–30 min, never on a metered network).
  if (!launch_probed_ && probe_configured()) {
    launch_probed_ = true;
    kick_probe();
  }

  DeployPhase phase = DeployPhase::Idle;
  std::string deploy_error;
  std::string progress;
  bool fresh = false;
  if (probe_state_.load(std::memory_order_acquire) == 2) consume_probe();
  {
    std::lock_guard lk(deploy_mu_);
    phase = deploy_phase_;
    deploy_error = deploy_error_;
    progress = deploy_progress_;
    fresh = std::exchange(deploy_fresh_, false);
    if (phase == DeployPhase::Requested && mono_ms() - requested_at_ms_ >= cfg_.deploy_timeout_ms) {
      // No verdict ever came (see deploy_timeout_ms). Report it now, and toast it next tick.
      phase = deploy_phase_ = DeployPhase::Failed;
      deploy_error = deploy_error_ = std::string(kNoAnswerError);
      deploy_fresh_ = true;
      warn("update: no answer from the update service — giving up on this request");
    }
  }
  // Exactly once per verdict, and only from here: this is the input thread, which owns client_.
  // A toast is the fallback surface: when the menu is open the Updates panel is patched in place
  // and a toast on top of it would just be the same sentence twice.
  if (fresh && !(cfg_.osd && cfg_.osd->open())) {
    if (const std::string t = deploy_toast(phase); !t.empty())
      show_toast(client_, t, kDeployToastMs);
  }
  // The portal found nothing to install for the commit the probe announced: stop announcing it, or
  // every check would offer an update that cannot land.
  if (fresh && phase == DeployPhase::Empty && !probe_announced_.empty() && cfg_.state &&
      cfg_.state->commit() == probe_announced_) {
    probe_refused_ = probe_announced_;
    probe_refused_at_ms_ = mono_ms();
  }

  if (cfg_.state && cfg_.state->available()) {
    const std::string commit = cfg_.state->commit();
    if (!commit.empty() && commit != last_commit_) {
      last_commit_ = commit;
      // A newer commit makes an earlier "latest" / failed / blocked verdict stale (a later release
      // can be the very fix for a blocked one); a staged deploy or one in flight still stands.
      if (phase == DeployPhase::Empty || phase == DeployPhase::Failed ||
          phase == DeployPhase::PermissionBlocked) {
        std::lock_guard lk(deploy_mu_);
        if (deploy_phase_ == phase) deploy_phase_ = DeployPhase::Idle;
        phase = deploy_phase_;
        deploy_error = deploy_error_;
      }
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
                                     cfg_.local_version, idle, cfg_.updater != nullptr, progress);

  // Prefer the pending release's notes; fall back to the running version's, so the tab is never
  // empty. Only link out when the fetch produced nothing at all. installed_notes_ is written by the
  // fetch thread, so it is read only once the fetch has published.
  std::string notes = cv.notes;
  if (notes.empty() && fetch_done()) notes = installed_notes_;
  if (notes.empty() && fetch_done()) notes = std::string(kReleaseNotesFallback);
  feed(v, notes);
}

void UpdatePromptController::feed(const UpdatesView& view, const std::string& notes) {
  if (fed_valid_ && view.has_update == fed_has_ && view.can_check == fed_check_ &&
      view.can_quit == fed_quit_ && view.status == fed_status_ && notes == fed_notes_)
    return;
  fed_valid_ = true;
  fed_has_ = view.has_update;
  fed_check_ = view.can_check;
  fed_quit_ = view.can_quit;
  fed_status_ = view.status;
  fed_notes_ = notes;
  if (cfg_.osd)
    cfg_.osd->set_update_model(view.has_update, view.can_check, view.can_quit, view.status, notes);
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
    deploy_progress_.clear();
    deploy_fresh_ = false;  // Requested never toasts; the tab shows it
    requested_at_ms_ = mono_ms();
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

void UpdatePromptController::check_now() {
  if (!probe_configured()) {
    request_deploy("user asked to check");
    return;
  }
  {
    std::lock_guard lk(deploy_mu_);
    // A deploy in flight or staged already answers the question; the tab offers no Check there.
    if (deploy_phase_ == DeployPhase::Requested || deploy_phase_ == DeployPhase::Done) return;
    deploy_phase_ = DeployPhase::Checking;
    deploy_error_.clear();
    deploy_fresh_ = false;
  }
  // A launch probe still in flight answers this press too: it reads the same file.
  probe_for_user_ = true;
  kick_probe();
  info("update: user asked to check — reading the published ref");
}

bool UpdatePromptController::probe_configured() const {
  return !cfg_.running_commit.empty() && static_cast<bool>(cfg_.fetch_remote_ref);
}

void UpdatePromptController::kick_probe() {
  int expected = 0;
  if (!probe_state_.compare_exchange_strong(expected, 1)) return;  // already running or unconsumed
  if (probe_thread_.joinable()) probe_thread_.join();
  probe_thread_ = std::thread([this] {
    const std::optional<std::string> body = cfg_.fetch_remote_ref();
    probe_result_ = body ? decide_ref_check(*body, cfg_.running_commit) : RefCheck::Unknown;
    probe_commit_ = body ? short_commit(parse_ref_commit(*body)) : std::string();
    probe_state_.store(2, std::memory_order_release);
  });
}

// Input thread, once the probe has published. The probe only ever ADDS availability: the ref is
// cached by the CDN for up to 10 min, so the portal can know about a release before the probe does,
// and an "up to date" read must never hide what the portal announced.
void UpdatePromptController::consume_probe() {
  if (probe_thread_.joinable()) probe_thread_.join();
  RefCheck result = probe_result_;
  const std::string commit = probe_commit_;
  probe_state_.store(0, std::memory_order_relaxed);
  const bool for_user = std::exchange(probe_for_user_, false);
  if (result == RefCheck::UpdateAvailable && refused(commit)) result = RefCheck::UpToDate;
  if (result == RefCheck::UpdateAvailable) {
    info("update: the published repo has a newer commit (" + commit + ")");
    probe_announced_ = commit;
    if (cfg_.state) cfg_.state->set_available(true, commit);
    // Re-run the announcement logic even when this is the commit already announced: after a failed
    // or timed-out deploy nothing else re-offers it (the portal does not re-announce), and the
    // Failed text tells the user to check again. The markers still apply — an ignored commit stays
    // hidden and the one-time card is not shown twice.
    last_commit_.clear();
  }
  // An announcement of the commit the portal refused does not count: nothing can install it.
  const bool announced = cfg_.state && cfg_.state->available() && !refused(cfg_.state->commit());

  bool fall_back = false;
  {
    std::lock_guard lk(deploy_mu_);
    const bool checking = deploy_phase_ == DeployPhase::Checking;
    if (result == RefCheck::UpdateAvailable) {
      if (checking) deploy_phase_ = DeployPhase::Idle;  // the announcement is the answer
    } else if (result == RefCheck::UpToDate) {
      // "Latest" is earned only when nothing else has announced an update.
      if (checking || (deploy_phase_ == DeployPhase::Idle && !for_user)) {
        deploy_phase_ = announced ? DeployPhase::Idle : DeployPhase::Empty;
        deploy_fresh_ = checking && !announced;  // a press gets an answer; a launch stays quiet
      }
    } else {
      fall_back = checking;
    }
  }
  // The portal path is the one verified on hardware: when the ref cannot be read, let the portal
  // check by deploying, exactly as a check did before the probe existed.
  if (fall_back)
    request_deploy(
        "user asked to check; the published repo was unreadable, falling back to the portal");
  else if (result == RefCheck::Unknown)
    warn("update: could not read the published repo — the portal's own check still runs");
}

bool UpdatePromptController::refused(const std::string& commit) const {
  return !probe_refused_.empty() && commit == probe_refused_ &&
         mono_ms() - probe_refused_at_ms_ < cfg_.refusal_window_ms;
}

void UpdatePromptController::on_deploy_progress(std::uint32_t op, std::uint32_t n_ops,
                                                std::uint32_t percent) {
  std::lock_guard lk(deploy_mu_);
  if (deploy_phase_ != DeployPhase::Requested) return;
  deploy_progress_ = deploy_progress_status(op, n_ops, percent);
  requested_at_ms_ = mono_ms();  // still moving: restart the no-answer timeout
}

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

DeployPhase UpdatePromptController::deploy_phase() const {
  std::lock_guard lk(deploy_mu_);
  return deploy_phase_;
}

void UpdatePromptController::ignore_version() {
  notify_ = false;
  hidden_status_ = std::string(kIgnoredStatus);
  if (cfg_.state) write_update_marker(update_dot_marker_path(cfg_.state_dir), cfg_.state->commit());
  info("update: user ignored this version — hidden until a newer release");
}

}  // namespace deckback

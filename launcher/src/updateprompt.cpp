#include "updateprompt.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>

#include "log.hpp"
#include "overlay.hpp"
#include "scripts.hpp"
#include "updater.hpp"

#if __has_include(<curl/curl.h>)
#define DECKBACK_HAVE_CURL 1
#include <curl/curl.h>
#else
#define DECKBACK_HAVE_CURL 0
#endif

namespace deckback {
namespace {

constexpr std::size_t kMaxNotesLen = 4000;
constexpr long kReleasesTimeoutSec = 6;

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

void append_utf8(std::string& out, unsigned cp) {
  if (cp >= 0xD800 && cp <= 0xDFFF) {  // lone surrogate: emit a replacement, don't try to pair
    out += '?';
    return;
  }
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Decode the JSON string value of `key` from `s` (first match). Handles the common escapes and BMP
// \uXXXX. Not a full parser — sufficient for GitHub's release objects, like config.cpp's extractor.
std::optional<std::string> json_string_field(const std::string& s, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const size_t k = s.find(needle);
  if (k == std::string::npos) return std::nullopt;
  const size_t colon = s.find(':', k + needle.size());
  if (colon == std::string::npos) return std::nullopt;
  size_t p = colon + 1;
  while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
  if (p >= s.size() || s[p] != '"') return std::nullopt;
  std::string out;
  for (size_t i = p + 1; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '\\' && i + 1 < s.size()) {
      const char n = s[++i];
      switch (n) {
        case 'n':
          out += '\n';
          break;
        case 't':
          out += '\t';
          break;
        case 'r':
          break;  // drop \r so notes are \n-terminated
        case 'u': {
          if (i + 4 < s.size()) {
            unsigned cp = 0;
            bool ok = true;
            for (int h = 0; h < 4; ++h) {
              const char d = s[i + 1 + h];
              int val;
              if (d >= '0' && d <= '9')
                val = d - '0';
              else if (d >= 'a' && d <= 'f')
                val = d - 'a' + 10;
              else if (d >= 'A' && d <= 'F')
                val = d - 'A' + 10;
              else {
                ok = false;
                break;
              }
              cp = cp * 16 + static_cast<unsigned>(val);
            }
            if (ok) {
              i += 4;
              append_utf8(out, cp);
            } else {
              out += 'u';
            }
          } else {
            out += 'u';
          }
          break;
        }
        default:
          out += n;  // \" \\ \/ and any other escaped char: keep it verbatim
      }
    } else if (c == '"') {
      return out;
    } else {
      out += c;
    }
  }
  return std::nullopt;
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

#if DECKBACK_HAVE_CURL
size_t curl_sink(char* ptr, size_t size, size_t nmemb, void* user) {
  static_cast<std::string*>(user)->append(ptr, size * nmemb);
  return size * nmemb;
}

// GET a GitHub API URL into `out`. Short timeout so a slow/absent network never stalls the card.
bool github_get(const std::string& url, std::string& out) {
  CURL* c = curl_easy_init();
  if (!c) return false;
  curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Accept: application/vnd.github+json");
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_sink);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, kReleasesTimeoutSec);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "deckback-updater/1");  // GitHub requires a UA
  const CURLcode rc = curl_easy_perform(c);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);
  return rc == CURLE_OK && !out.empty();
}
#endif

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

std::vector<ReleaseNote> parse_github_releases(const std::string& json) {
  std::vector<ReleaseNote> out;
  // Slice each depth-1 object out of the array so a "name"/"body" cannot bleed across releases.
  int depth = 0;
  size_t obj_start = std::string::npos;
  bool in_string = false;
  for (size_t i = 0; i < json.size(); ++i) {
    const char c = json[i];
    if (in_string) {
      if (c == '\\')
        ++i;  // skip the escaped char
      else if (c == '"')
        in_string = false;
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      if (depth == 0) obj_start = i;
      ++depth;
    } else if (c == '}') {
      if (depth > 0) --depth;
      if (depth == 0 && obj_start != std::string::npos) {
        const std::string obj = json.substr(obj_start, i - obj_start + 1);
        obj_start = std::string::npos;
        auto tag = json_string_field(obj, "tag_name");
        if (!tag || tag->empty()) continue;  // a release with no tag is unusable
        std::string version = *tag;
        if (!version.empty() && (version.front() == 'v' || version.front() == 'V'))
          version.erase(0, 1);
        ReleaseNote n;
        n.version = version;
        n.title = json_string_field(obj, "name").value_or("");
        n.body = trim(json_string_field(obj, "body").value_or(""));
        out.push_back(std::move(n));
      }
    }
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

std::string read_update_marker(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  std::string line;
  std::getline(f, line);
  return trim(std::move(line));
}

bool write_update_marker(const std::string& path, const std::string& value) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    // A read-only state dir must not crash and must not nag every launch; warn and move on (the
    // mild failure: the card may auto-show / the dot may reappear again next boot).
    warn(std::format("update: cannot write {} — the update prompt state was not saved", path));
    return false;
  }
  std::fputs((value + "\n").c_str(), f);
  std::fclose(f);
  return true;
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
  if (!cfg_.state || !cfg_.state->available()) {
    return;
  }
  // A full reload wiped documentElement, so the pill is gone even if we thought it was shown.
  if (reloaded_.exchange(false, std::memory_order_acquire)) dot_shown_ = false;

  const std::string commit = cfg_.state->commit();
  if (!commit.empty() && commit != last_commit_) {
    last_commit_ = commit;
    const NotifyDecision d =
        decide_notification(commit, read_update_marker(update_card_marker_path(cfg_.state_dir)),
                            read_update_marker(update_dot_marker_path(cfg_.state_dir)));
    dot_desired_ = d.show_dot;
    if (d.show_card) {
      kick_changelog();  // fetch off-thread; the card shows once the notes are ready (below)
      want_card_ = true;
    }
  }
  // Reconcile the pill: shown when an un-dismissed update exists, hidden while a video is up so it
  // never sits over playback (durable/self-update.md). Only touches the DOM on a change; this is
  // the only place that draws or hides the pill.
  const bool want_dot = dot_desired_ && !on_watch;
  if (want_dot && !dot_shown_)
    draw_dot();
  else if (!want_dot && dot_shown_)
    hide_dot();

  // Deferred auto-card: show it once the async changelog is ready, without ever blocking this
  // thread — and never over a playing video. The card is modal (input.cpp swallows direction keys
  // while it is up), so it must respect the watch view even more than the pill does; want_card_
  // stays armed until the user leaves playback.
  if (want_card_ && !card_visible_ && !on_watch &&
      fetch_state_.load(std::memory_order_acquire) == 2) {
    want_card_ = false;
    show_card();
  }
}

void UpdatePromptController::kick_changelog() {
  int expected = 0;
  if (!fetch_state_.compare_exchange_strong(expected, 1)) return;  // already fetching or done
#if DECKBACK_HAVE_CURL
  fetch_thread_ = std::thread([this] {
    ChangelogView v;
    std::string json;
    if (github_get(cfg_.releases_url, json))
      v = summarize_releases(parse_github_releases(json), cfg_.local_version, kMaxNotesLen);
    else
      warn("update: could not fetch release notes from GitHub — showing a link instead");
    cached_ = std::move(v);
    fetch_state_.store(2, std::memory_order_release);
  });
#else
  fetch_state_.store(2, std::memory_order_release);  // no libcurl: done, empty -> fallback link
#endif
}

ChangelogView UpdatePromptController::current_changelog() const {
  if (fetch_state_.load(std::memory_order_acquire) == 2) return cached_;
  return {};  // not ready yet: the card falls back to the releases link
}

bool UpdatePromptController::draw_card(DevToolsClient& client) {
  const ChangelogView cv = current_changelog();
  const std::string version =
      cv.available_version.empty()
          ? std::format("A newer version is available. You have v{}.", cfg_.local_version)
          : std::format("v{} is available. You have v{}.", cv.available_version,
                        cfg_.local_version);
  const std::string notes =
      cv.notes.empty() ? "Release notes: github.com/properrr/deckback/releases" : cv.notes;

  return ScriptLibrary::instance().invoke(
      client, "update_card",
      ScriptParams()
          .set("heading", "Update available")
          .set("version", version)
          .set("notes", notes)
          // A/B/Y hotkeys as [key, label] pairs so the page can colour each glyph via its CSP-safe
          // stylesheet (an inline colour would be dropped; durable/self-update.md).
          .set("buttons", std::vector<std::pair<std::string, std::string>>{
                              {"A", "Update now"}, {"B", "Not now"}, {"Y", "Ignore version"}}));
}

void UpdatePromptController::show_card() {
  if (!draw_card(client_)) {
    warn("update: could not draw the update card (engine unreachable)");
    return;
  }
  card_visible_ = true;
  // Recorded on show (like the onboarding card): a crash while the card is up must not re-nag.
  if (cfg_.state)
    write_update_marker(update_card_marker_path(cfg_.state_dir), cfg_.state->commit());
  info("update: 'update available' card shown");
}

void UpdatePromptController::hide_card() {
  if (!card_visible_) return;
  ScriptLibrary::instance().invoke(client_, "update_card_hide");
  card_visible_ = false;
}

void UpdatePromptController::draw_dot() {
  ScriptLibrary::instance().invoke(client_, "update_badge", ScriptParams());
  dot_shown_ = true;
}

void UpdatePromptController::hide_dot() {
  ScriptLibrary::instance().invoke(client_, "update_badge_hide");
  dot_shown_ = false;
}

void UpdatePromptController::confirm_update() {
  hide_card();
  // The update is being applied; the pill has nothing left to offer. tick() — the sole owner of
  // the pill DOM — reconciles it away in this same input-loop iteration.
  dot_desired_ = false;
  if (cfg_.updater) cfg_.updater->request_update();
  show_toast(client_, "Updating\xE2\x80\xA6 it will apply the next time you open Deckback.", 6000);
  info("update: user confirmed — deploy requested");
}

void UpdatePromptController::dismiss_later() {
  hide_card();  // the dot stays (dot_desired_ unchanged); the Menu route remains reachable
  info("update: user chose 'Not now' — the dot stays");
}

void UpdatePromptController::ignore_version() {
  hide_card();
  dot_desired_ = false;  // tick() hides the pill; a newer commit re-arms it via the dot marker
  if (cfg_.state) {
    write_update_marker(update_dot_marker_path(cfg_.state_dir), cfg_.state->commit());
  }
  info("update: user ignored this version — dot hidden until a newer release");
}

bool UpdatePromptController::open_from_menu() {
  if (!update_available()) return false;
  kick_changelog();  // ensure the fetch is under way; the card shows the link if it isn't ready yet
  want_card_ = false;  // an explicit open satisfies any pending auto-show; don't re-pop on dismiss
  show_card();         // user-initiated: deliberately allowed even over playback
  return true;
}

void UpdatePromptController::on_page_reloaded() {
  // Dot: flag the wipe; the input thread's tick() redraws it (playback-aware), so it never
  // reappears over a video and only client_ ever touches the pill.
  reloaded_.store(true, std::memory_order_release);
  // Card: re-inject here if it was open. Navigator thread, so use an own client (not client_);
  // draw_card() has no state/marker side effects, so re-injecting into the reloaded page is
  // idempotent.
  if (!card_visible_.load(std::memory_order_acquire)) return;
  if (cfg_.cdp_port <= 0) return;
  DevToolsClient c(cfg_.cdp_host, cfg_.cdp_port);
  draw_card(c);
}

}  // namespace deckback

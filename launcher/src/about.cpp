#include "about.hpp"

#include <cstdlib>
#include <utility>

#include "fileio.hpp"

namespace deckback {
namespace {

std::string collapse_ws(std::string_view s) {
  std::string out;
  bool pending_space = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      pending_space = true;
      continue;
    }
    if (pending_space && !out.empty()) out += ' ';
    pending_space = false;
    out += c;
  }
  return out;
}

std::string decode_entities(std::string s) {
  const std::pair<std::string_view, std::string_view> table[] = {
      {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}};
  for (const auto& [ent, ch] : table) {
    for (size_t p = s.find(ent); p != std::string::npos; p = s.find(ent, p))
      s.replace(p, ent.size(), ch);
  }
  // &amp; last, so "&amp;lt;" decodes to "&lt;" rather than "<".
  for (size_t p = s.find("&amp;"); p != std::string::npos; p = s.find("&amp;", p))
    s.replace(p, 5, "&");
  return s;
}

std::string clean(std::string_view raw) { return decode_entities(collapse_ws(raw)); }

// Text strictly between the first `open` at/after `from` and the next `close`, plus the index just
// past `close` (for iterating). nullopt if either marker is missing.
std::optional<std::pair<std::string, size_t>> slice(std::string_view xml, std::string_view open,
                                                    std::string_view close, size_t from) {
  size_t a = xml.find(open, from);
  if (a == std::string_view::npos) return std::nullopt;
  a += open.size();
  size_t b = xml.find(close, a);
  if (b == std::string_view::npos) return std::nullopt;
  return std::pair{std::string(xml.substr(a, b - a)), b + close.size()};
}

std::string tag_text(std::string_view xml, std::string_view open, std::string_view close,
                     size_t from = 0) {
  auto s = slice(xml, open, close, from);
  return s ? clean(s->first) : std::string();
}

}  // namespace

AboutInfo parse_metainfo(std::string_view xml) {
  AboutInfo out;

  // <name> appears twice: the app name (first, before <developer>) and the developer's name (inside
  // <developer>). Split on the <developer> position so each takes the right one.
  const size_t dev = xml.find("<developer");
  if (auto s = slice(xml, "<name>", "</name>", 0);
      s && (dev == std::string_view::npos || s->second <= dev))
    out.name = clean(s->first);
  if (dev != std::string_view::npos)
    if (auto s = slice(xml, "<name>", "</name>", dev)) out.developer = clean(s->first);

  out.summary = tag_text(xml, "<summary>", "</summary>");
  out.homepage = tag_text(xml, "<url type=\"homepage\">", "</url>");
  out.help = tag_text(xml, "<url type=\"help\">", "</url>");

  // Description block: first <p> is the prose; every <li> is a feature. Scope both to the block so
  // a release note's <p> (further down the file) can never be mistaken for the description.
  if (auto desc = slice(xml, "<description>", "</description>", 0)) {
    const std::string& d = desc->first;
    out.description = tag_text(d, "<p>", "</p>");
    for (size_t pos = 0;;) {
      auto li = slice(d, "<li>", "</li>", pos);
      if (!li) break;
      out.features.push_back(clean(li->first));
      pos = li->second;
    }
  }
  return out;
}

std::optional<std::string> load_metainfo() {
  std::vector<std::string> candidates;
  if (const char* env = std::getenv("DECKBACK_METAINFO"); env && *env) candidates.emplace_back(env);
  candidates.emplace_back("/app/share/metainfo/io.github.properrr.deckback.metainfo.xml");
  candidates.emplace_back("flatpak/assets/io.github.properrr.deckback.metainfo.xml");
  for (const std::string& path : candidates)
    if (auto text = read_file(path)) return text;
  return std::nullopt;
}

}  // namespace deckback

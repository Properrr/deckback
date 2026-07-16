// Pure parse of the AppStream metainfo into the About tab's model (about.cpp). The About tab is
// single-sourced from this file, so the parser must pull the right fields out of the real shape.

#include "about.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using namespace deckback;

namespace {
// A trimmed copy of flatpak/assets/io.github.properrr.deckback.metainfo.xml with the shape that
// matters: two <name> tags (app + developer), a <description> whose first <p> is prose and whose
// <li>s are features, a release <p> that must NOT be read as the description, and typed <url>s.
constexpr const char* kXml = R"XML(<?xml version="1.0"?>
<component type="desktop-application">
  <id>io.github.properrr.deckback</id>
  <name>Deckback</name>
  <summary>Living-room video client for Steam Deck, Game Mode native</summary>
  <description>
    <p>
      Deckback is an unofficial native TV-interface video client for the Steam Deck.
    </p>
    <p>Built for the couch, not the desktop:</p>
    <ul>
      <li>Controller-native — every button is mapped at the launcher, no touchscreen required</li>
      <li>Hardware VP9 decode for cool, battery-friendly 720p60 playback</li>
      <li>Suspend &amp; resume with the power button</li>
    </ul>
  </description>
  <developer id="io.github.properrr">
    <name>properrr</name>
  </developer>
  <url type="homepage">https://github.com/properrr/deckback</url>
  <url type="help">https://github.com/properrr/deckback/blob/main/docs/SUPPORT.md</url>
  <releases>
    <release version="0.0.4"><description><p>See the changelog for details.</p></description></release>
  </releases>
</component>)XML";
}  // namespace

void test_parses_the_real_shape() {
  const AboutInfo a = parse_metainfo(kXml);
  assert(a.name == "Deckback");
  assert(a.developer == "properrr");
  assert(a.summary == "Living-room video client for Steam Deck, Game Mode native");
  // First <description> <p>, whitespace collapsed — NOT the release note further down.
  assert(a.description ==
         "Deckback is an unofficial native TV-interface video client for the Steam Deck.");
  assert(a.homepage == "https://github.com/properrr/deckback");
  assert(a.help == "https://github.com/properrr/deckback/blob/main/docs/SUPPORT.md");
  assert(a.features.size() == 3);
  assert(a.features[0] ==
         "Controller-native — every button is mapped at the launcher, no touchscreen required");
  assert(a.features[1] == "Hardware VP9 decode for cool, battery-friendly 720p60 playback");
  // &amp; must decode to a bare ampersand.
  assert(a.features[2] == "Suspend & resume with the power button");
}

void test_missing_fields_degrade_to_empty() {
  const AboutInfo a = parse_metainfo("<component><name>Only</name></component>");
  assert(a.name == "Only");
  assert(a.summary.empty());
  assert(a.developer.empty());  // no <developer> block
  assert(a.features.empty());
  assert(a.homepage.empty());

  const AboutInfo none = parse_metainfo("");
  assert(none.name.empty() && none.features.empty());
}

int main() {
  test_parses_the_real_shape();
  test_missing_fields_degrade_to_empty();
  std::puts("about_test: all passed");
  return 0;
}

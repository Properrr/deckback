#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace deckback {

// The "About" tab's content, extracted from the AppStream metainfo. Best-effort: a missing file or
// tag yields empty strings.
struct AboutInfo {
  std::string name;
  std::string summary;
  std::string description;  // the first <p> of the metainfo <description>
  std::string developer;
  std::string homepage;
  std::string help;
  std::vector<std::string> features;  // the <li> bullets
};

// Pure: extract AboutInfo from an AppStream metainfo XML string. A tolerant string scan, not an XML
// parser: only valid for our own lint-validated file with flat tags.
AboutInfo parse_metainfo(std::string_view xml);

// Read the installed metainfo. Tries $DECKBACK_METAINFO, then the flatpak install path, then the
// repo copy (dev runs). nullopt if none is readable.
std::optional<std::string> load_metainfo();

}  // namespace deckback

#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace deckback {

// The "About" tab's content, extracted from the AppStream metainfo — the single source of truth the
// Flathub listing already maintains, so the in-app tab can never drift from the store page. All
// fields are best-effort: a missing file or tag yields empty strings, and the tab degrades to
// whatever it has (at minimum the compiled-in version).
struct AboutInfo {
  std::string name;
  std::string summary;
  std::string description;  // the first <p> of the metainfo <description>
  std::string developer;
  std::string homepage;
  std::string help;
  std::vector<std::string> features;  // the <li> bullets
};

// Pure: extract AboutInfo from an AppStream metainfo XML string. Tolerant string-scan, not a real
// XML parser — the file is ours and lint-validated, and the fields are simple, flat tags.
AboutInfo parse_metainfo(std::string_view xml);

// Read the installed metainfo. Tries $DECKBACK_METAINFO, then the flatpak install path, then the
// repo copy (dev runs). nullopt if none is readable.
std::optional<std::string> load_metainfo();

}  // namespace deckback

#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace deckback {

// Small filesystem primitives. Each of these existed three to five times over — `read_file` alone
// was open-coded as `ifstream` + `ostringstream` + `rdbuf()` in config.cpp, config_store.cpp,
// about.cpp, scripts.cpp and updater.cpp, and the mkdir-then-write pair in onboarding.cpp,
// updateprompt.cpp and cdm_fetcher.cpp (the last with its own hand-rolled parent-directory loop).
// Copies of a five-line helper drift in what they do on failure, which is the only interesting
// part.

// Whole file as bytes. nullopt when it cannot be opened. Binary — no newline translation.
std::optional<std::string> read_file(const std::string& path);

// Create `path`'s parent directories, then write `bytes`, truncating. Returns whether the file is
// fully on disk. Never throws; a read-only or unwritable destination is a normal outcome here (the
// state dir may be read-only), so callers warn and carry on rather than treating it as fatal.
bool write_file(const std::string& path, std::string_view bytes);

// True when `path` exists and is a regular file (not a directory or a dangling symlink).
bool file_exists(const std::string& path);

// ---- state markers ------------------------------------------------------------------------------
//
// The launcher records three one-line facts across restarts: the first-run card was shown, the
// update card was shown, this update commit was ignored. They are separate files rather than one
// state blob so a partial write can only lose one of them, and each filename is versioned by its
// caller so bumping the version re-shows that UI once.

// The single trimmed line in `path`, or "" if absent/unreadable/empty.
std::string read_marker(const std::string& path);

// Write `value` plus a newline. On failure logs `what_breaks` (phrased as the user-visible
// consequence, e.g. "the controls card will show again") and returns false — a marker that cannot
// be written must never crash the launcher or nag on every launch.
bool write_marker(const std::string& path, std::string_view value, std::string_view what_breaks);

}  // namespace deckback

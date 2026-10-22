#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace deckback {

// Small filesystem primitives.

// Whole file as bytes. nullopt when it cannot be opened. Binary — no newline translation.
std::optional<std::string> read_file(const std::string& path);

// Create `path`'s parent directories, then write `bytes`, truncating. Returns whether the file is
// fully on disk. Never throws; an unwritable destination is a normal, non-fatal outcome.
bool write_file(const std::string& path, std::string_view bytes);

// write_file, but all-or-nothing: written to `path + ".part"`, fsync'd, then renamed over `path`.
// For files a reader trusts merely because they exist.
bool write_file_atomic(const std::string& path, std::string_view bytes);

// True when `path` exists and is a regular file (not a directory or a dangling symlink).
bool file_exists(const std::string& path);

// Milliseconds since `path` was last modified, or nullopt when it cannot be stat'ed. Compared
// against CLOCK_REALTIME, since the writer may be another process. A future mtime reads as 0.
std::optional<long> file_age_ms(const std::string& path);

// ---- state markers ------------------------------------------------------------------------------
//
// One-line facts persisted across restarts, one file each so a partial write loses only one.

// The single trimmed line in `path`, or "" if absent/unreadable/empty.
std::string read_marker(const std::string& path);

// Write `value` plus a newline. On failure logs `what_breaks` (the user-visible consequence, e.g.
// "the controls card will show again") and returns false.
bool write_marker(const std::string& path, std::string_view value, std::string_view what_breaks);

}  // namespace deckback

#pragma once
#include <string>
#include <string_view>

namespace deckback {

enum class Level { info, warn, error };

// Configure a size-rotating file sink (doc §2 P2: the launcher owns log rotation). Until this is
// called, logs go to stderr only — the default scaffolding behavior, and what `--version` /
// `--selftest-dbus` keep using. Idempotent: a second call reconfigures the sink.
//
//   file_path    absolute path of the active log file (rotated siblings get `.1`, `.2`, … suffixes)
//   max_bytes    rotate once the active file would exceed this; <=0 disables rotation (unbounded)
//   max_files    how many rotated files to keep (0 = truncate in place, keep no history)
//   also_stderr  keep mirroring every line to stderr so journald / Steam still capture output
//
// Returns false if the file could not be opened (logging falls back to stderr-only).
bool log_init(const std::string& file_path, long max_bytes, int max_files, bool also_stderr);

// Flush and close the file sink. Safe to call when no sink is open. After this, logging reverts to
// stderr-only until the next log_init.
void log_shutdown();

// Thread-safe: the logind backend thread logs from off the main thread.
void log_write(Level lvl, std::string_view msg);

inline void info(std::string_view m) { log_write(Level::info, m); }
inline void warn(std::string_view m) { log_write(Level::warn, m); }
inline void error(std::string_view m) { log_write(Level::error, m); }

}  // namespace deckback

#include "log.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

namespace deckback {
namespace {

struct Sink {
  std::mutex mu;
  std::string path;
  std::ofstream file;
  long max_bytes = 0;
  int max_files = 0;
  long cur_bytes = 0;
  bool to_stderr = true;
  bool file_open = false;
};

Sink& sink() {
  static Sink s;
  return s;
}

const char* tag_of(Level lvl) {
  switch (lvl) {
    case Level::error:
      return "ERROR";
    case Level::warn:
      return "WARN";
    case Level::info:
      break;
  }
  return "INFO";
}

std::string format_line(Level lvl, std::string_view msg) {
  const auto now = std::chrono::system_clock::now();
  return std::format("[{:%F %T}] deckback {}: {}\n", std::chrono::floor<std::chrono::seconds>(now),
                     tag_of(lvl), msg);
}

namespace fs = std::filesystem;

// Precondition: caller holds sink().mu and s.file_open is true. Closes the active file, shifts the
// numbered history (`.N` dropped, `.k`→`.k+1`, active→`.1`), and reopens a fresh truncated file.
void rotate_locked(Sink& s) {
  s.file.close();
  std::error_code ec;
  if (s.max_files <= 0) {
    s.file.open(s.path, std::ios::binary | std::ios::trunc);
    s.cur_bytes = 0;
    s.file_open = s.file.good();
    return;
  }
  fs::remove(s.path + "." + std::to_string(s.max_files), ec);
  for (int i = s.max_files - 1; i >= 1; --i) {
    fs::rename(s.path + "." + std::to_string(i), s.path + "." + std::to_string(i + 1), ec);
  }
  fs::rename(s.path, s.path + ".1", ec);
  s.file.open(s.path, std::ios::binary | std::ios::trunc);
  s.cur_bytes = 0;
  s.file_open = s.file.good();
}

}  // namespace

bool log_init(const std::string& file_path, long max_bytes, int max_files, bool also_stderr) {
  Sink& s = sink();
  std::lock_guard lk(s.mu);
  if (s.file.is_open()) s.file.close();

  std::error_code ec;
  const fs::path p(file_path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);

  s.path = file_path;
  s.max_bytes = max_bytes;
  s.max_files = max_files < 0 ? 0 : max_files;
  s.to_stderr = also_stderr;

  s.cur_bytes = static_cast<long>(fs::file_size(p, ec));
  if (ec) s.cur_bytes = 0;
  s.file.open(file_path, std::ios::binary | std::ios::app);
  s.file_open = s.file.good();
  if (!s.file_open) {
    std::fprintf(stderr, "deckback ERROR: log_init: cannot open %s (stderr-only)\n",
                 file_path.c_str());
  }
  return s.file_open;
}

void log_shutdown() {
  Sink& s = sink();
  std::lock_guard lk(s.mu);
  if (s.file.is_open()) {
    s.file.flush();
    s.file.close();
  }
  s.file_open = false;
}

void log_write(Level lvl, std::string_view msg) {
  const std::string line = format_line(lvl, msg);
  Sink& s = sink();
  std::lock_guard lk(s.mu);
  if (s.file_open) {
    const long len = static_cast<long>(line.size());
    if (s.max_bytes > 0 && s.cur_bytes > 0 && s.cur_bytes + len > s.max_bytes) rotate_locked(s);
    if (s.file_open) {
      s.file.write(line.data(), len);
      s.file.flush();
      s.cur_bytes += len;
    }
  }
  if (!s.file_open || s.to_stderr) {
    std::fwrite(line.data(), 1, line.size(), stderr);
  }
}

}  // namespace deckback

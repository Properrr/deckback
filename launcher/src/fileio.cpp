#include "fileio.hpp"

#include <sys/stat.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "log.hpp"

namespace deckback {

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool write_file(const std::string& path, std::string_view bytes) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  f.close();  // flush before judging: a full disk fails here, not at write()
  return f.good();
}

bool file_exists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string read_marker(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  std::string line;
  std::getline(f, line);
  const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!line.empty() && ws(line.back())) line.pop_back();
  size_t i = 0;
  while (i < line.size() && ws(line[i])) ++i;
  return line.substr(i);
}

bool write_marker(const std::string& path, std::string_view value, std::string_view what_breaks) {
  if (write_file(path, std::string(value) + "\n")) return true;
  warn(std::format("state: cannot write {} — {}", path, what_breaks));
  return false;
}

}  // namespace deckback

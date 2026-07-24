#include "log.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "harness.hpp"

namespace fs = std::filesystem;
using namespace deckback;

namespace {

fs::path g_root;

fs::path scratch(const std::string& name) {
  fs::path p = g_root / name;
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

std::vector<std::string> read_lines(const fs::path& file) {
  std::vector<std::string> out;
  std::ifstream f(file);
  std::string line;
  while (std::getline(f, line))
    if (!line.empty()) out.push_back(line);
  return out;
}

// Every log line embeds a zero-padded counter "#NNNNNN#"; pull it back out so we can reason about
// ordering and loss across rotated files.
int seq_of(const std::string& line) {
  size_t a = line.find('#');
  size_t b = line.find('#', a + 1);
  if (a == std::string::npos || b == std::string::npos) return -1;
  return std::stoi(line.substr(a + 1, b - a - 1));
}

std::string msg(int n) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "#%06d# payload", n);
  return buf;
}

// Rotation keeps a bounded history and never loses a line inside the retained window.
void test_rotation_bounded_and_lossless() {
  fs::path dir = scratch("rot");
  fs::path base = dir / "deckback.log";
  const int kMaxFiles = 3;
  // ~55-byte lines, 400-byte cap -> a handful of lines per file, forcing many rotations.
  assert(log_init(base.string(), 400, kMaxFiles, /*also_stderr=*/false));

  const int kLines = 500;
  for (int i = 0; i < kLines; ++i) info(msg(i));
  log_shutdown();

  // History is bounded: base + up to kMaxFiles siblings, and NOT one past that.
  assert(fs::exists(base));
  assert(!fs::exists(base.string() + "." + std::to_string(kMaxFiles + 1)));

  // Read oldest -> newest: .N, .N-1, ..., .1, base.
  std::vector<int> seqs;
  for (int i = kMaxFiles; i >= 1; --i) {
    fs::path p = base.string() + "." + std::to_string(i);
    if (fs::exists(p))
      for (auto& l : read_lines(p)) seqs.push_back(seq_of(l));
  }
  for (auto& l : read_lines(base)) seqs.push_back(seq_of(l));

  assert(!seqs.empty());
  // The very last line written must be the last retained line.
  assert(seqs.back() == kLines - 1);
  // Strictly increasing and contiguous within the retained window (no drops, no reorder).
  for (size_t i = 1; i < seqs.size(); ++i) assert(seqs[i] == seqs[i - 1] + 1);
  // We actually dropped old history (otherwise the test isn't exercising rotation).
  assert(seqs.front() > 0);
}

// max_files == 0: truncate in place, keep no numbered history.
void test_truncate_no_history() {
  fs::path dir = scratch("trunc");
  fs::path base = dir / "deckback.log";
  assert(log_init(base.string(), 300, 0, false));
  for (int i = 0; i < 300; ++i) info(msg(i));
  log_shutdown();

  assert(fs::exists(base));
  assert(!fs::exists(base.string() + ".1"));
  auto lines = read_lines(base);
  assert(!lines.empty());
  assert(seq_of(lines.back()) == 299);
}

// max_bytes <= 0: rotation disabled, single unbounded file retains everything.
void test_rotation_disabled() {
  fs::path dir = scratch("nomax");
  fs::path base = dir / "deckback.log";
  assert(log_init(base.string(), 0, 5, false));
  const int kLines = 200;
  for (int i = 0; i < kLines; ++i) info(msg(i));
  log_shutdown();

  assert(!fs::exists(base.string() + ".1"));
  auto lines = read_lines(base);
  assert(static_cast<int>(lines.size()) == kLines);
  assert(seq_of(lines.front()) == 0);
  assert(seq_of(lines.back()) == kLines - 1);
}

// A single line larger than the cap must still be written (no infinite-rotation / no drop).
void test_oversized_single_line() {
  fs::path dir = scratch("big");
  fs::path base = dir / "deckback.log";
  assert(log_init(base.string(), 50, 3, false));
  std::string huge(500, 'x');
  info(huge);
  log_shutdown();
  auto lines = read_lines(base);
  assert(lines.size() == 1);
  assert(lines[0].find(huge) != std::string::npos);
}

// Re-init after a full file continues appending, not truncating, when the path is unchanged.
void test_reinit_appends() {
  fs::path dir = scratch("reinit");
  fs::path base = dir / "deckback.log";
  assert(log_init(base.string(), 0, 0, false));
  info(msg(0));
  log_shutdown();
  assert(log_init(base.string(), 0, 0, false));
  info(msg(1));
  log_shutdown();
  auto lines = read_lines(base);
  assert(lines.size() == 2);
  assert(seq_of(lines[0]) == 0);
  assert(seq_of(lines[1]) == 1);
}

// Opening an impossible path fails cleanly and logging falls back to stderr (no crash).
void test_open_failure_is_graceful() {
  bool ok = log_init("/proc/nonexistent_dir/deckback.log", 100, 3, true);
  assert(!ok);
  info("this must not crash; goes to stderr");
  log_shutdown();
}

// Concurrent writers (mimics the logind backend thread) never corrupt a line: every line in the
// file is well-formed and carries a decodable sequence tag.
void test_thread_safety() {
  fs::path dir = scratch("threads");
  fs::path base = dir / "deckback.log";
  assert(log_init(base.string(), 0, 0, false));
  const int kThreads = 8, kEach = 500;
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t)
    ts.emplace_back([&, t] {
      for (int i = 0; i < kEach; ++i) info(msg(t * kEach + i));
    });
  for (auto& th : ts) th.join();
  log_shutdown();

  auto lines = read_lines(base);
  assert(static_cast<int>(lines.size()) == kThreads * kEach);
  std::set<int> seen;
  for (auto& l : lines) {
    int s = seq_of(l);
    assert(s >= 0 && s < kThreads * kEach);
    assert(l.find("payload") != std::string::npos);  // line not interleaved/torn
    seen.insert(s);
  }
  assert(static_cast<int>(seen.size()) == kThreads * kEach);  // every message present, none lost
}

}  // namespace

DECKBACK_TEST_MAIN(log) {
  g_root = fs::temp_directory_path() / "deckback_log_test";
  fs::remove_all(g_root);
  fs::create_directories(g_root);

  test_rotation_bounded_and_lossless();
  test_truncate_no_history();
  test_rotation_disabled();
  test_oversized_single_line();
  test_reinit_appends();
  test_open_failure_is_graceful();
  test_thread_safety();

  fs::remove_all(g_root);
  std::puts("log_test: ok");
  return 0;
}

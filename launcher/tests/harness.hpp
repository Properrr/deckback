#pragma once
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

// Links several test files into one group binary while keeping one CTest entry per case. Test files
// declare their entry point with DECKBACK_TEST_MAIN, a group file lists the cases, and CTest runs
// the group binary once per case name. One case per process keeps process-global state (log file,
// script library, CWD) from coupling tests.

#define DECKBACK_TEST_MAIN(name) int name##_test_main()
#define DECKBACK_TEST_DECL(name) int name##_test_main()

struct DeckbackTestCase {
  const char* name;
  int (*fn)();
};

// With a name, run that case; without one, run all in order. An unknown name is an error (exit 2).
inline int deckback_run_group(int argc, char** argv, const DeckbackTestCase* cases, size_t count) {
  if (argc > 1) {
    for (size_t i = 0; i < count; ++i) {
      if (std::strcmp(cases[i].name, argv[1]) == 0) return cases[i].fn();
    }
    std::fprintf(stderr, "unknown test case '%s'; known:", argv[1]);
    for (size_t i = 0; i < count; ++i) std::fprintf(stderr, " %s", cases[i].name);
    std::fprintf(stderr, "\n");
    return 2;
  }
  for (size_t i = 0; i < count; ++i) {
    std::printf("== %s\n", cases[i].name);
    if (const int rc = cases[i].fn(); rc != 0) return rc;
  }
  return 0;
}

#define DECKBACK_RUN_GROUP(cases) \
  deckback_run_group(argc, argv, cases, sizeof(cases) / sizeof((cases)[0]))

// A scratch directory private to this test process (mkdtemp under $TMPDIR), removed at exit, so
// concurrent test runs never share files. Heap-held so the atexit cleanup never sees a destroyed
// path; pid-checked so a forked child cannot delete its parent's dir.
inline const std::filesystem::path& deckback_test_tmpdir() {
  static const pid_t owner = ::getpid();
  static const std::filesystem::path* root = [] {
    std::string t = (std::filesystem::temp_directory_path() / "deckback-test-XXXXXX").string();
    if (!::mkdtemp(t.data())) {
      std::perror("mkdtemp");
      std::abort();
    }
    auto* p = new std::filesystem::path(t);
    std::atexit([] {
      if (::getpid() != owner) return;
      std::error_code ec;
      std::filesystem::remove_all(*root, ec);
    });
    return p;
  }();
  return *root;
}

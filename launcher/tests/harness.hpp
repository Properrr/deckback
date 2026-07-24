#pragma once
#include <cstdio>
#include <cstring>

// Lets several test files link into one binary without giving up per-test reporting.
//
// Every test here was its own executable, and each one statically linked the whole of
// deckback_core — 25 link steps of a multi-megabyte archive to run a few hundred assertions. The
// obvious fix, one big binary, would have cost the thing that made the split worth having: CTest
// naming each area separately, so a failure says *what* broke before you read a line of output.
//
// So the grouping keeps both. Test files declare their entry point with DECKBACK_TEST_MAIN instead
// of a bare `main`, a group file lists the cases, and CMake still registers one CTest entry per
// case — each invoking the group binary with that case's name. Reporting granularity is unchanged;
// only the number of link steps drops.
//
// One case per process is not just for reporting: these tests touch process-global state (the log
// file, the script library, the CWD), and running them in one process would couple them.

#define DECKBACK_TEST_MAIN(name) int name##_test_main()
#define DECKBACK_TEST_DECL(name) int name##_test_main()

struct DeckbackTestCase {
  const char* name;
  int (*fn)();
};

// With a name, run that case. Without one, run them all in order (handy locally; CTest always
// passes a name). An unknown name is an error, not a silent success — a typo in the CMake list
// must not read as a passing test.
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

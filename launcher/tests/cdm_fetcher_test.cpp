// L0: CdmFetcher without the network. The path rule is pure; the install/verify half writes
// stand-in bytes under the test's private temp dir. No CDM is ever involved.
//
// `usable_cdm_path` mirrors the rule enforced by the engine patch
// (patches/0001-...-widevine-cdm-registration.patch): `--widevine-cdm-path` must be absolute and
// free of parent references. The engine is authoritative; this check exists so a bad path is
// refused where the profile dir that produced it is still in scope.
//
// The interesting cases are the near-misses. ".." is only traversal when it is a whole path
// component: `/a/..b` and `/a/b..` are ordinary directory names, and rejecting them would refuse a
// perfectly good CDM for no reason.
#include "cdm_fetcher.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

#include "fileio.hpp"
#include "harness.hpp"

using deckback::CdmFetcher;
using Installed = deckback::CdmFetcher::Installed;

namespace {

constexpr const char* kAbcSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

// A truncated CDM used to read as installed forever: ensure_installed only asked whether the file
// existed. Now a pinned CDM is hashed on every launch, and an install never leaves a partial file.
void test_install_and_reverify() {
  const std::string profile = (deckback_test_tmpdir() / "cdm-profile").string();
  const std::string dest = CdmFetcher::installed_path(profile);
  assert(CdmFetcher::check_installed(dest, kAbcSha) == Installed::Missing);

  // Bytes that do not match the pin are refused and nothing is written.
  assert(!CdmFetcher::install(dest, "abd", kAbcSha));
  assert(!deckback::file_exists(dest));
  assert(CdmFetcher::install(dest, "abc", kAbcSha));  // creates the parent dirs
  assert(deckback::read_file(dest) == "abc");
  assert(!deckback::file_exists(dest + ".part"));
  assert(CdmFetcher::check_installed(dest, kAbcSha) == Installed::Verified);
  assert(CdmFetcher::check_installed(dest, "") == Installed::Unpinned);

  // A refused re-install leaves the working file whole.
  assert(!CdmFetcher::install(dest, "zzz", kAbcSha));
  assert(deckback::read_file(dest) == "abc");

  // Truncated, as an interrupted pre-atomic write left it: detected, not trusted.
  assert(deckback::write_file(dest, "ab"));
  assert(CdmFetcher::check_installed(dest, kAbcSha) == Installed::Mismatch);
  // With nowhere to fetch the pinned one from, the installed file is still used (a pin that moved
  // must not strand a CDM that worked); with no file at all, there is no CDM.
  assert(CdmFetcher::ensure_installed(profile, {"", kAbcSha}));
  std::filesystem::remove(dest);
  assert(!CdmFetcher::ensure_installed(profile, {"", kAbcSha}));
}

}  // namespace

DECKBACK_TEST_MAIN(cdm_fetcher) {
  test_install_and_reverify();

  // The path the launcher actually builds, from a normal profile dir.
  const std::string real = CdmFetcher::installed_path("/home/deck/.local/share/deckback/profile");
  assert(real ==
         "/home/deck/.local/share/deckback/profile/WidevineCdm/_platform_specific/"
         "linux_x64/libwidevinecdm.so");
  assert(CdmFetcher::usable_cdm_path(real));

  // Absolute and clean.
  assert(CdmFetcher::usable_cdm_path("/a/libwidevinecdm.so"));
  assert(CdmFetcher::usable_cdm_path("/"));

  // Relative: the engine refuses these, so we must notice first. A relative DECKBACK_PROFILE is the
  // way a user gets here.
  assert(!CdmFetcher::usable_cdm_path(""));
  assert(!CdmFetcher::usable_cdm_path("relative/libwidevinecdm.so"));
  assert(!CdmFetcher::usable_cdm_path("./libwidevinecdm.so"));
  assert(!CdmFetcher::usable_cdm_path("../libwidevinecdm.so"));

  // Parent references, as whole components, anywhere.
  assert(!CdmFetcher::usable_cdm_path("/a/../b/libwidevinecdm.so"));
  assert(!CdmFetcher::usable_cdm_path("/.."));
  assert(!CdmFetcher::usable_cdm_path("/a/.."));
  assert(!CdmFetcher::usable_cdm_path("/../a"));

  // ...but NOT as a substring. These are legal directory names and must be accepted.
  assert(CdmFetcher::usable_cdm_path("/a/..b/c"));
  assert(CdmFetcher::usable_cdm_path("/a/b../c"));
  assert(CdmFetcher::usable_cdm_path("/a/x..y/c"));
  assert(CdmFetcher::usable_cdm_path("/a/.../c"));
  assert(CdmFetcher::usable_cdm_path("/a/./c"));  // "." is not a parent reference

  std::puts("cdm_fetcher_test: ok");
  return 0;
}

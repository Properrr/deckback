// CdmFetcher without the network: install/verify with stand-in bytes, and `usable_cdm_path`, which
// mirrors the engine patch's rule (absolute, no ".." path component).
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

// A pinned CDM is hashed on every check, and an install never leaves a partial file.
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

  // A truncated file is a mismatch.
  assert(deckback::write_file(dest, "ab"));
  assert(CdmFetcher::check_installed(dest, kAbcSha) == Installed::Mismatch);
  // With no fetch URL, a mismatched file is still used; with no file, there is no CDM.
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

  // Relative (e.g. from a relative DECKBACK_PROFILE).
  assert(!CdmFetcher::usable_cdm_path(""));
  assert(!CdmFetcher::usable_cdm_path("relative/libwidevinecdm.so"));
  assert(!CdmFetcher::usable_cdm_path("./libwidevinecdm.so"));
  assert(!CdmFetcher::usable_cdm_path("../libwidevinecdm.so"));

  // Parent references, as whole components, anywhere.
  assert(!CdmFetcher::usable_cdm_path("/a/../b/libwidevinecdm.so"));
  assert(!CdmFetcher::usable_cdm_path("/.."));
  assert(!CdmFetcher::usable_cdm_path("/a/.."));
  assert(!CdmFetcher::usable_cdm_path("/../a"));

  // A ".." substring inside a name is fine.
  assert(CdmFetcher::usable_cdm_path("/a/..b/c"));
  assert(CdmFetcher::usable_cdm_path("/a/b../c"));
  assert(CdmFetcher::usable_cdm_path("/a/x..y/c"));
  assert(CdmFetcher::usable_cdm_path("/a/.../c"));
  assert(CdmFetcher::usable_cdm_path("/a/./c"));  // "." is not a parent reference

  std::puts("cdm_fetcher_test: ok");
  return 0;
}

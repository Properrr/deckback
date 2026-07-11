// L0: the pure parts of CdmFetcher. No network, no filesystem, no CDM.
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
#include <string>

using deckback::CdmFetcher;

int main() {
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

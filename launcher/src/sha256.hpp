#pragma once
#include <string>
#include <string_view>

namespace deckback {

// Self-contained SHA-256 (FIPS 180-4). Dependency-free on purpose: the toolchain is restricted to
// libsystemd/libevdev/libcurl (no OpenSSL), and the CDM fetcher needs a hash to verify a download
// against a pinned digest (Phase 7). Returns the lowercase hex digest (64 chars).
std::string sha256_hex(std::string_view data);

}  // namespace deckback

#pragma once
#include <string>
#include <string_view>

namespace deckback {

// Self-contained SHA-256 (FIPS 180-4); no OpenSSL dependency. Returns the lowercase hex digest
// (64 chars).
std::string sha256_hex(std::string_view data);

}  // namespace deckback

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rockid {

/* Seals plaintext to each 32-byte X25519 public key. Empty result means a key was missing. */
std::string seal_dialog(const std::string &plaintext,
                        const std::vector<std::pair<int64_t, std::string>> &keys);

}

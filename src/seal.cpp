#include "seal.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <stdexcept>

namespace rockid {
namespace {

std::string b64(const unsigned char *data, size_t n) {
  static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  unsigned value = 0;
  int bits = 0;
  for (size_t i = 0; i < n; ++i) {
    value = (value << 8) | data[i];
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out.push_back(alphabet[(value >> bits) & 0x3F]);
    }
  }
  if (bits > 0) out.push_back(alphabet[(value << (6 - bits)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

std::string seal_one(const std::string &plaintext, const std::string &public_key) {
  if (public_key.size() != 32) throw std::runtime_error("bad public key");
  EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                               reinterpret_cast<const unsigned char *>(public_key.data()), 32);
  EVP_PKEY_CTX *gen = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
  EVP_PKEY *mine = nullptr;
  if (!peer || !gen || EVP_PKEY_keygen_init(gen) != 1 || EVP_PKEY_keygen(gen, &mine) != 1) {
    EVP_PKEY_free(peer);
    EVP_PKEY_CTX_free(gen);
    throw std::runtime_error("x25519 keygen failed");
  }
  unsigned char epub[32];
  size_t epub_len = sizeof epub;
  EVP_PKEY_get_raw_public_key(mine, epub, &epub_len);

  EVP_PKEY_CTX *derive = EVP_PKEY_CTX_new(mine, nullptr);
  unsigned char shared[32];
  size_t shared_len = sizeof shared;
  if (!derive || EVP_PKEY_derive_init(derive) != 1 || EVP_PKEY_derive_set_peer(derive, peer) != 1 ||
      EVP_PKEY_derive(derive, shared, &shared_len) != 1) {
    EVP_PKEY_CTX_free(derive);
    EVP_PKEY_free(mine);
    EVP_PKEY_CTX_free(gen);
    EVP_PKEY_free(peer);
    throw std::runtime_error("x25519 derive failed");
  }
  unsigned char key[32];
  SHA256(shared, shared_len, key);
  unsigned char nonce[12];
  if (RAND_bytes(nonce, sizeof nonce) != 1) throw std::runtime_error("RAND_bytes failed");

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  std::string cipher(plaintext.size(), '\0');
  int out_len = 0;
  int final_len = 0;
  unsigned char tag[16];
  bool ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1 &&
            EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(cipher.data()), &out_len,
                              reinterpret_cast<const unsigned char *>(plaintext.data()),
                              static_cast<int>(plaintext.size())) == 1 &&
            EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(cipher.data()) + out_len, &final_len) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
  EVP_CIPHER_CTX_free(ctx);
  EVP_PKEY_CTX_free(derive);
  EVP_PKEY_free(mine);
  EVP_PKEY_CTX_free(gen);
  EVP_PKEY_free(peer);
  if (!ok) throw std::runtime_error("aes-gcm failed");
  cipher.resize(static_cast<size_t>(out_len + final_len));
  cipher.append(reinterpret_cast<char *>(tag), sizeof tag);
  return "rk1." + b64(epub, sizeof epub) + "." + b64(nonce, sizeof nonce) + "." +
         b64(reinterpret_cast<const unsigned char *>(cipher.data()), cipher.size());
}

}  // namespace

std::string seal_dialog(const std::string &plaintext,
                        const std::vector<std::pair<int64_t, std::string>> &keys) {
  if (keys.size() < 2) return {};
  std::string out = "{\"e\":1,\"seals\":[";
  bool first = true;
  for (const auto &key : keys) {
    if (key.second.size() != 32) return {};
    if (!first) out.push_back(',');
    first = false;
    out += "{\"user_id\":" + std::to_string(key.first) + ",\"box\":\"" + seal_one(plaintext, key.second) + "\"}";
  }
  out += "]}";
  return out;
}

}

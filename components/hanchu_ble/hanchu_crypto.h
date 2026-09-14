#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::hanchu_ble {

static constexpr size_t HANCHU_KEY_LENGTH = 16;
static constexpr size_t HANCHU_TOKEN_LENGTH = 6;

/// Minimal AES-128, encrypt direction only (all that CFB mode needs).
/// Self-contained so it doesn't depend on the mbedTLS API of the ESP-IDF version in use.
class Aes128 {
 public:
  void set_key(const uint8_t *key);
  void encrypt_block(const uint8_t *in, uint8_t *out) const;

 protected:
  uint8_t round_keys_[176];
};

/// AES-CFB8 without padding. The Hanchu codec restarts from the same IV for every message.
void cfb8_encrypt(const Aes128 &aes, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t length);
void cfb8_decrypt(const Aes128 &aes, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t length);

/// Session key: the static codec basis with six characters overwritten by the handshake token.
void derive_session_key(const char *token, uint8_t *key_out);

/// The fixed IV ("state vector") used for every message.
const uint8_t *hanchu_state_vector();

/// Known-answer tests: FIPS-197 block, SP 800-38A CFB8 and the Hanchu key derivation.
bool crypto_self_test();

}  // namespace esphome::hanchu_ble

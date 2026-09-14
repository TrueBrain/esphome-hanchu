#include "hanchu_crypto.h"

#include <cstring>

namespace esphome::hanchu_ble {

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,  //
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,  //
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,  //
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,  //
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,  //
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,  //
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,  //
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,  //
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,  //
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,  //
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,  //
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,  //
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,  //
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,  //
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,  //
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,  //
};

// Static values from the Hanchu app, as documented in upton68/hanchu-ess-ble protocol.py.
static const uint8_t CODEC_BASIS[HANCHU_KEY_LENGTH] = {'g', 'x', 'k', 'j', '@', '2', '0', '9',
                                                       '9', '@', '1', '9', '1', '4', 'z', 'y'};
static const uint8_t STATE_VECTOR[16] = {'9', 'z', '6', '4', 'Q', 'r', '8', 'm',
                                         'Z', 'H', '7', 'P', 'g', '8', 'd', '1'};

static inline uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00)); }

void Aes128::set_key(const uint8_t *key) {
  std::memcpy(this->round_keys_, key, 16);
  uint8_t rcon = 0x01;
  for (size_t i = 16; i < 176; i += 4) {
    uint8_t t0 = this->round_keys_[i - 4];
    uint8_t t1 = this->round_keys_[i - 3];
    uint8_t t2 = this->round_keys_[i - 2];
    uint8_t t3 = this->round_keys_[i - 1];
    if (i % 16 == 0) {
      uint8_t first = t0;
      t0 = SBOX[t1] ^ rcon;
      t1 = SBOX[t2];
      t2 = SBOX[t3];
      t3 = SBOX[first];
      rcon = xtime(rcon);
    }
    this->round_keys_[i] = this->round_keys_[i - 16] ^ t0;
    this->round_keys_[i + 1] = this->round_keys_[i - 15] ^ t1;
    this->round_keys_[i + 2] = this->round_keys_[i - 14] ^ t2;
    this->round_keys_[i + 3] = this->round_keys_[i - 13] ^ t3;
  }
}

void Aes128::encrypt_block(const uint8_t *in, uint8_t *out) const {
  // State is column-major: s[row + 4 * column]
  uint8_t s[16];
  for (size_t i = 0; i < 16; i++)
    s[i] = in[i] ^ this->round_keys_[i];

  for (size_t round = 1; round <= 10; round++) {
    for (auto &b : s)
      b = SBOX[b];

    uint8_t t = s[1];
    s[1] = s[5];
    s[5] = s[9];
    s[9] = s[13];
    s[13] = t;
    t = s[2];
    s[2] = s[10];
    s[10] = t;
    t = s[6];
    s[6] = s[14];
    s[14] = t;
    t = s[15];
    s[15] = s[11];
    s[11] = s[7];
    s[7] = s[3];
    s[3] = t;

    if (round != 10) {
      for (size_t c = 0; c < 16; c += 4) {
        uint8_t a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];
        uint8_t all = a0 ^ a1 ^ a2 ^ a3;
        s[c] = a0 ^ all ^ xtime(a0 ^ a1);
        s[c + 1] = a1 ^ all ^ xtime(a1 ^ a2);
        s[c + 2] = a2 ^ all ^ xtime(a2 ^ a3);
        s[c + 3] = a3 ^ all ^ xtime(a3 ^ a0);
      }
    }

    const uint8_t *round_key = this->round_keys_ + 16 * round;
    for (size_t i = 0; i < 16; i++)
      s[i] ^= round_key[i];
  }
  std::memcpy(out, s, 16);
}

void cfb8_encrypt(const Aes128 &aes, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t length) {
  uint8_t shift_register[16];
  uint8_t keystream[16];
  std::memcpy(shift_register, iv, 16);
  for (size_t i = 0; i < length; i++) {
    aes.encrypt_block(shift_register, keystream);
    uint8_t cipher = in[i] ^ keystream[0];
    out[i] = cipher;
    std::memmove(shift_register, shift_register + 1, 15);
    shift_register[15] = cipher;
  }
}

void cfb8_decrypt(const Aes128 &aes, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t length) {
  uint8_t shift_register[16];
  uint8_t keystream[16];
  std::memcpy(shift_register, iv, 16);
  for (size_t i = 0; i < length; i++) {
    aes.encrypt_block(shift_register, keystream);
    uint8_t cipher = in[i];  // read first: in and out may alias
    out[i] = cipher ^ keystream[0];
    std::memmove(shift_register, shift_register + 1, 15);
    shift_register[15] = cipher;
  }
}

void derive_session_key(const char *token, uint8_t *key_out) {
  std::memcpy(key_out, CODEC_BASIS, HANCHU_KEY_LENGTH);
  size_t start = static_cast<uint8_t>(token[HANCHU_TOKEN_LENGTH - 1]) % 10;
  for (size_t i = 0; i < HANCHU_TOKEN_LENGTH; i++)
    key_out[start + i] = static_cast<uint8_t>(token[i]);
}

const uint8_t *hanchu_state_vector() { return STATE_VECTOR; }

bool crypto_self_test() {
  Aes128 aes;

  // FIPS-197 appendix C.1
  static const uint8_t BLOCK_KEY[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
  static const uint8_t BLOCK_PLAIN[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                          0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  static const uint8_t BLOCK_CIPHER[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                           0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
  uint8_t block[16];
  aes.set_key(BLOCK_KEY);
  aes.encrypt_block(BLOCK_PLAIN, block);
  if (std::memcmp(block, BLOCK_CIPHER, 16) != 0)
    return false;

  // NIST SP 800-38A F.3.7 CFB8-AES128
  static const uint8_t CFB_KEY[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                      0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
  static const uint8_t CFB_IV[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                     0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
  static const uint8_t CFB_PLAIN[18] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9,
                                        0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a, 0xae, 0x2d};
  static const uint8_t CFB_CIPHER[18] = {0x3b, 0x79, 0x42, 0x4c, 0x9c, 0x0d, 0xd4, 0x36, 0xba,
                                         0xce, 0x9e, 0x0e, 0xd4, 0x58, 0x6a, 0x4f, 0x32, 0xb9};
  uint8_t stream[18];
  aes.set_key(CFB_KEY);
  cfb8_encrypt(aes, CFB_IV, CFB_PLAIN, stream, sizeof(stream));
  if (std::memcmp(stream, CFB_CIPHER, sizeof(stream)) != 0)
    return false;
  cfb8_decrypt(aes, CFB_IV, stream, stream, sizeof(stream));
  if (std::memcmp(stream, CFB_PLAIN, sizeof(stream)) != 0)
    return false;

  // Token "K7Q2M5": '5' is ASCII 53, 53 % 10 = 3, so characters 3..8 are replaced
  uint8_t session_key[HANCHU_KEY_LENGTH];
  derive_session_key("K7Q2M5", session_key);
  return std::memcmp(session_key, "gxkK7Q2M5@1914zy", HANCHU_KEY_LENGTH) == 0;
}

}  // namespace esphome::hanchu_ble

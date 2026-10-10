#pragma once

// AES-128 in CBC mode (FIPS 197, NIST SP 800-38A): what the Wii's discs are encrypted with (see wii_disc.hpp). Decryption
// is table driven, as fast as is useful for reading a disc; encryption is the plain byte-wise cipher (only the tests, which
// build a disc of their own, need it).

#include <array>
#include <cstddef>
#include <cstdint>

namespace ntr {

class Aes128 {
public:
    explicit Aes128(const std::array<std::uint8_t, 16>& key);

    // Decrypts `size` bytes (a multiple of 16) in CBC mode. `iv` is the previous ciphertext block (it is updated to the
    // last block of `in`, so a long stream can be decrypted in pieces). `in` and `out` may be the same buffer.
    void cbc_decrypt(std::array<std::uint8_t, 16>& iv, const std::uint8_t* in, std::uint8_t* out, std::size_t size) const;
    void cbc_encrypt(std::array<std::uint8_t, 16>& iv, const std::uint8_t* in, std::uint8_t* out, std::size_t size) const;

    void decrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const;
    void encrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const;

private:
    std::uint32_t enc_[44];  // the round keys, as big-endian words
    std::uint32_t dec_[44];  // for the equivalent inverse cipher
};

}  // namespace ntr

// Hashing and random bytes (OpenSSL libcrypto).
// SHA-1 for pieces/files, SHA-256 for passwords, tokens and state digests.
#pragma once
#include <cstddef>
#include <string>

namespace crypto {

std::string sha1_hex(const void* data, size_t len);
std::string sha256_hex(const std::string& data);

// incremental SHA-1, so big files are hashed piece by piece
class Sha1Stream {
public:
    Sha1Stream();
    ~Sha1Stream();
    Sha1Stream(const Sha1Stream&) = delete;
    Sha1Stream& operator=(const Sha1Stream&) = delete;
    void update(const void* data, size_t len);
    std::string final_hex();
private:
    void* ctx_;  // EVP_MD_CTX*, hidden to keep OpenSSL out of the header
};

// n random bytes (RAND_bytes), hex encoded
std::string random_hex(size_t n);

// hex(SHA-256(salt + password))
std::string hash_password(const std::string& salt_hex, const std::string& password);

// constant-time compare (CRYPTO_memcmp)
bool ct_equal(const std::string& a, const std::string& b);

}  // namespace crypto

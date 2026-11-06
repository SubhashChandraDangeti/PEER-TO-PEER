#include "crypto.h"
#include "util.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdexcept>
#include <vector>

namespace crypto {

std::string sha1_hex(const void* data, size_t len) {
    unsigned char md[SHA_DIGEST_LENGTH];
    SHA1(static_cast<const unsigned char*>(data), len, md);
    return util::to_hex(md, sizeof(md));
}

std::string sha256_hex(const std::string& data) {
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), md);
    return util::to_hex(md, sizeof(md));
}

Sha1Stream::Sha1Stream() : ctx_(EVP_MD_CTX_new()) {
    if (!ctx_ || EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(ctx_), EVP_sha1(), nullptr) != 1) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
        throw std::runtime_error("EVP init failed");
    }
}

Sha1Stream::~Sha1Stream() { EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_)); }

void Sha1Stream::update(const void* data, size_t len) {
    EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx_), data, len);
}

std::string Sha1Stream::final_hex() {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(ctx_), md, &n);
    return util::to_hex(md, n);
}

std::string random_hex(size_t n) {
    std::vector<unsigned char> buf(n);
    if (RAND_bytes(buf.data(), static_cast<int>(n)) != 1)
        throw std::runtime_error("RAND_bytes failed");
    return util::to_hex(buf.data(), n);
}

std::string hash_password(const std::string& salt_hex, const std::string& password) {
    return sha256_hex(salt_hex + password);
}

bool ct_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;  // sizes are public (fixed-length hex)
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace crypto

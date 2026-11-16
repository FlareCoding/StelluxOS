#ifndef MCUFW_DIGEST_HPP
#define MCUFW_DIGEST_HPP

// SHA-256 over bytes as they arrive, computed by libbearssl

#include <bearssl/bearssl_hash.h>

#include <string>
#include <string_view>

class sha256_digest {
public:
    sha256_digest();

    void update(std::string_view bytes);

    // The digest of every byte so far, as 64 lowercase hex digits
    std::string hex() const;

private:
    br_sha256_context m_context;
};

#endif // MCUFW_DIGEST_HPP

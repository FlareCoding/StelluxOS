#include "digest.hpp"

constexpr const char* HEX_DIGITS = "0123456789abcdef";

sha256_digest::sha256_digest() {
    br_sha256_init(&m_context);
}

void sha256_digest::update(std::string_view bytes) {
    br_sha256_update(&m_context, bytes.data(), bytes.size());
}

std::string sha256_digest::hex() const {
    unsigned char digest[br_sha256_SIZE];
    br_sha256_out(&m_context, digest);

    std::string text;
    for (unsigned char byte : digest) {
        text += HEX_DIGITS[byte >> 4];
        text += HEX_DIGITS[byte & 0xf];
    }

    return text;
}

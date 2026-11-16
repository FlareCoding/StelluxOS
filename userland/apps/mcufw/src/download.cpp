#include "download.hpp"

#include "cancel.hpp"
#include "digest.hpp"

#include <stlxhttp/http.h>

#include <vector>

constexpr size_t DOWNLOAD_CHUNK_BYTES = 64 * 1024;

static bool read_body(stlxhttp_request* request, const std::string& url, sha256_digest& digest,
                      const byte_consumer& consume, failure& failed) {
    std::vector<char> buffer(DOWNLOAD_CHUNK_BYTES);
    while (true) {
        int64_t count = stlxhttp_read(request, buffer.data(), buffer.size());
        if (cancel_requested()) {
            failed = { failure_kind::cancelled, "the download of " + url + " was cancelled" };
            return false;
        }

        if (count < 0) {
            failed = { failure_kind::network, "cannot download " + url + ", since " + stlxhttp_error_message(request) };
            return false;
        }

        if (count == 0) {
            return true;
        }

        std::string_view bytes(buffer.data(), static_cast<size_t>(count));
        digest.update(bytes);
        if (!consume(bytes, failed)) {
            return false;
        }
    }
}

bool download(const std::string& url, const std::string& sha256, const byte_consumer& consume, failure& failed) {
    stlxhttp_request* request = nullptr;
    if (stlxhttp_open(url.c_str(), 0, &request) != STLXHTTP_OK) {
        bool cancelled = cancel_requested();
        failed = { cancelled ? failure_kind::cancelled : failure_kind::network,
                   cancelled ? "the download of " + url + " was cancelled"
                             : "cannot download " + url + ", since " + stlxhttp_error_message(request) };
        stlxhttp_close(request);
        return false;
    }

    sha256_digest digest;
    bool read = read_body(request, url, digest, consume, failed);
    stlxhttp_close(request);
    if (!read) {
        return false;
    }

    if (!sha256.empty() && digest.hex() != sha256) {
        failed = { failure_kind::verify, url + " does not match the checksum its index states" };
        return false;
    }

    return true;
}

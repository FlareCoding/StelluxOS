#ifndef MCUFW_DOWNLOAD_HPP
#define MCUFW_DOWNLOAD_HPP

// Downloads with libstlxhttp, handing the body to a consumer as it arrives, so no archive is ever held whole

#include "failure.hpp"

#include <functional>
#include <string>
#include <string_view>

// Returning false stops the download with the failure it sets
using byte_consumer = std::function<bool(std::string_view bytes, failure& failed)>;

// Streams the body of `url` into `consume`, then, unless `sha256` is empty, checks the bytes against it, lowercase hex.
// Fails as network, verify or cancelled, or as the consumer's failure.
bool download(const std::string& url, const std::string& sha256, const byte_consumer& consume, failure& failed);

#endif // MCUFW_DOWNLOAD_HPP

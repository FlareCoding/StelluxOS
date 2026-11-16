#ifndef MCUFW_INFLATE_HPP
#define MCUFW_INFLATE_HPP

// Inflating a deflate stream that arrives in pieces, which the zip reader and the index reader share

#include <zlib.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

enum class inflate_failure { corrupt, after_end, consumer };

using inflated_consumer = std::function<bool(std::string_view bytes, std::string& error)>;

// Inflates all of `input`, handing `consume` each piece that fills at most `output`. Sets `ended` once the stream ends.
// Fails on corrupt data, on input after the end, or with the consumer's error, saying which in `why`.
bool inflate_input(z_stream& stream, std::string_view input, std::vector<unsigned char>& output, bool& ended,
                   const inflated_consumer& consume, inflate_failure& why, std::string& error);

#endif // MCUFW_INFLATE_HPP

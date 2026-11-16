#include "inflate.hpp"

bool inflate_input(z_stream& stream, std::string_view input, std::vector<unsigned char>& output, bool& ended,
                   const inflated_consumer& consume, inflate_failure& why, std::string& error) {
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    do {
        if (ended) {
            why = inflate_failure::after_end;
            return false;
        }

        stream.next_out = output.data();
        stream.avail_out = static_cast<uInt>(output.size());

        // Z_BUF_ERROR without input left means inflate can do nothing until the next bytes arrive
        int status = inflate(&stream, Z_NO_FLUSH);
        bool needs_input = status == Z_BUF_ERROR && stream.avail_in == 0;
        if (status != Z_OK && status != Z_STREAM_END && !needs_input) {
            why = inflate_failure::corrupt;
            return false;
        }

        ended = status == Z_STREAM_END;
        std::string_view produced(reinterpret_cast<const char*>(output.data()), output.size() - stream.avail_out);
        if (!produced.empty() && !consume(produced, error)) {
            why = inflate_failure::consumer;
            return false;
        }
    } while (stream.avail_in > 0 || (stream.avail_out == 0 && !ended));

    return true;
}

#ifndef SHEEN_WORKSPACE_EVENT_STREAM_H
#define SHEEN_WORKSPACE_EVENT_STREAM_H

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sheen {

enum class event_protocol {
    NONE,
    SPACE_STLXPKG, // stlxpkg --events: space-separated fields, reason in remainder
    TSV_MCUFW,     // mcufw --events: tab-separated fields
    LINES,         // plain line-delimited output, one field per non-empty line
};

/* Splits streamed stdout or stderr chunks into structured event lines for SPACE_STLXPKG, TSV_MCUFW, or LINES. */
class event_line_stream {
public:
    using event_callback = std::function<void(const std::vector<std::string>& fields)>;

    explicit event_line_stream(event_protocol protocol);

    void feed(std::string_view chunk, const event_callback& on_event);
    void flush(const event_callback& on_event);

private:
    void emit_line(std::string_view line, const event_callback& on_event) const;

    event_protocol m_protocol;
    std::string m_pending;
};

std::vector<std::string> split_event_line(event_protocol protocol, std::string_view line);

} // namespace sheen

#endif

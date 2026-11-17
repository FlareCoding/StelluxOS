#include "workspace/event_stream.h"

namespace sheen {

static constexpr size_t STLXPKG_FAILED_PREFIX_FIELDS = 3;

static std::vector<std::string> split_stlxpkg_line(std::string_view line) {
    std::vector<std::string> fields;
    size_t index = 0;

    while (index < line.size()) {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
            index++;
        }

        if (index >= line.size()) {
            break;
        }

        if (fields.size() == STLXPKG_FAILED_PREFIX_FIELDS && fields[0] == "failed") {
            fields.emplace_back(line.substr(index));
            break;
        }

        size_t start = index;
        while (index < line.size() && line[index] != ' ' && line[index] != '\t') {
            index++;
        }

        fields.emplace_back(line.substr(start, index - start));
    }

    return fields;
}

static std::vector<std::string> split_tsv_line(std::string_view line) {
    std::vector<std::string> fields;
    if (line.empty()) {
        return fields;
    }

    size_t start = 0;
    while (true) {
        size_t tab = line.find('\t', start);
        if (tab == std::string_view::npos) {
            fields.emplace_back(line.substr(start));
            break;
        }

        fields.emplace_back(line.substr(start, tab - start));
        start = tab + 1;
    }

    return fields;
}

std::vector<std::string> split_event_line(event_protocol protocol, std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }

    switch (protocol) {
        case event_protocol::SPACE_STLXPKG:
            return split_stlxpkg_line(line);
        case event_protocol::TSV_MCUFW:
            return split_tsv_line(line);
        case event_protocol::LINES:
            return line.empty() ? std::vector<std::string>{} : std::vector<std::string>{ std::string(line) };
        case event_protocol::NONE:
            break;
    }

    return {};
}

event_line_stream::event_line_stream(event_protocol protocol) : m_protocol(protocol) {
}

void event_line_stream::feed(std::string_view chunk, const event_callback& on_event) {
    if (m_protocol == event_protocol::NONE || chunk.empty()) {
        return;
    }

    m_pending.append(chunk.data(), chunk.size());

    size_t start = 0;
    while (true) {
        size_t newline = m_pending.find('\n', start);
        if (newline == std::string::npos) {
            break;
        }

        emit_line(std::string_view(m_pending).substr(start, newline - start), on_event);
        start = newline + 1;
    }

    if (start > 0) {
        m_pending.erase(0, start);
    }
}

void event_line_stream::flush(const event_callback& on_event) {
    if (m_protocol == event_protocol::NONE || m_pending.empty()) {
        m_pending.clear();
        return;
    }

    std::string tail = std::move(m_pending);
    m_pending.clear();
    emit_line(tail, on_event);
}

void event_line_stream::emit_line(std::string_view line, const event_callback& on_event) const {
    if (!on_event) {
        return;
    }

    std::vector<std::string> fields = split_event_line(m_protocol, line);
    if (!fields.empty()) {
        on_event(fields);
    }
}

} // namespace sheen

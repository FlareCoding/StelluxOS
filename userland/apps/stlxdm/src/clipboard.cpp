#include "clipboard.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

clipboard_item::clipboard_item(std::vector<clipboard_rep> reps, uint32_t flags)
    : m_reps(std::move(reps)), m_flags(flags) {
}

clipboard_item::~clipboard_item() {
    close_objects();
}

clipboard_item::clipboard_item(clipboard_item&& other) noexcept
    : m_reps(std::move(other.m_reps)), m_flags(other.m_flags) {
    other.m_reps.clear();
    other.m_flags = 0;
}

clipboard_item& clipboard_item::operator=(clipboard_item&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    close_objects();
    m_reps = std::move(other.m_reps);
    m_flags = other.m_flags;
    other.m_reps.clear();
    other.m_flags = 0;

    return *this;
}

uint64_t clipboard_item::size() const {
    uint64_t total = 0;
    for (const clipboard_rep& rep : m_reps) {
        total += rep.len;
    }

    return total;
}

uint64_t clipboard_item::held() const {
    uint64_t total = 0;
    for (const clipboard_rep& rep : m_reps) {
        total += std::max(rep.len, rep.held);
    }

    return total;
}

const clipboard_rep* clipboard_item::find(std::string_view mime_type) const {
    for (const clipboard_rep& rep : m_reps) {
        if (rep.mime_type == mime_type) {
            return &rep;
        }
    }

    return nullptr;
}

void clipboard_item::close_objects() {
    for (clipboard_rep& rep : m_reps) {
        if (rep.object >= 0) {
            close(rep.object);
            rep.object = -1;
        }
    }
}

/* Compares two objects' first `len` bytes through read-only mappings, a refused mapping counting as different */
static bool same_bytes(int a, int b, uint64_t len) {
    void* first = mmap(nullptr, len, PROT_READ, MAP_SHARED, a, 0);
    if (first == MAP_FAILED) {
        return false;
    }

    void* second = mmap(nullptr, len, PROT_READ, MAP_SHARED, b, 0);
    bool same = second != MAP_FAILED && memcmp(first, second, len) == 0;
    if (second != MAP_FAILED) {
        munmap(second, len);
    }

    munmap(first, len);
    return same;
}

bool same_content(const clipboard_item& a, const clipboard_item& b) {
    if (a.reps().size() != b.reps().size()) {
        return false;
    }

    for (size_t i = 0; i < a.reps().size(); i++) {
        const clipboard_rep& x = a.reps()[i];
        const clipboard_rep& y = b.reps()[i];
        if (x.mime_type != y.mime_type || x.len != y.len) {
            return false;
        }

        if (x.len > 0 && !same_bytes(x.object, y.object, x.len)) {
            return false;
        }
    }

    return true;
}

clipboard_history::clipboard_history(clipboard_matcher matches)
    : m_matches(std::move(matches)) {
}

void clipboard_history::set_limits(uint32_t items, uint64_t bytes) {
    m_item_limit = items;
    m_byte_limit = bytes;
    evict();
}

void clipboard_history::copy(clipboard_item item, std::string label, int64_t now_ns) {
    retire_current();
    if (item.empty()) {
        evict();
        return;
    }

    if (item.flags() & SWP_CLIPBOARD_PRIVATE) {
        m_private = std::move(item);
        evict();
        return;
    }

    /* The same content copied again moves its entry up with its id and pin, so whatever names the entry still finds
     * it, and the new copy's objects close */
    size_t i = 0;
    while (i < m_entries.size() && !m_matches(m_entries[i].item, item)) {
        i++;
    }

    clipboard_entry entry;
    if (i < m_entries.size()) {
        entry = std::move(m_entries[i]);
        m_entries.erase(m_entries.begin() + static_cast<ptrdiff_t>(i));
    } else {
        entry.id = m_next_id++;
        entry.item = std::move(item);
    }

    entry.label = std::move(label);
    entry.copied_ns = now_ns;
    m_entries.insert(m_entries.begin(), std::move(entry));
    m_front_is_current = true;
    evict();
}

const clipboard_item& clipboard_history::current() const {
    return m_front_is_current ? m_entries.front().item : m_private;
}

std::vector<const clipboard_entry*> clipboard_history::entries() const {
    std::vector<const clipboard_entry*> listed;
    for (bool pinned : { true, false }) {
        for (const clipboard_entry& entry : m_entries) {
            if (entry.pinned == pinned) {
                listed.push_back(&entry);
            }
        }
    }

    return listed;
}

const clipboard_entry* clipboard_history::find(uint64_t id) const {
    size_t i = index_of(id);
    return i < m_entries.size() ? &m_entries[i] : nullptr;
}

bool clipboard_history::is_current(uint64_t id) const {
    return m_front_is_current && m_entries.front().id == id;
}

void clipboard_history::make_current(uint64_t id, int64_t now_ns) {
    size_t i = index_of(id);
    if (i == m_entries.size() || is_current(id)) {
        return;
    }

    clipboard_entry entry = std::move(m_entries[i]);
    m_entries.erase(m_entries.begin() + static_cast<ptrdiff_t>(i));
    retire_current();

    entry.copied_ns = now_ns;
    m_entries.insert(m_entries.begin(), std::move(entry));
    m_front_is_current = true;
    evict();
}

bool clipboard_history::can_pin(uint64_t id) const {
    size_t i = index_of(id);
    if (i == m_entries.size()) {
        return false;
    }

    if (m_entries[i].pinned) {
        return true;
    }

    uint64_t size = m_entries[i].item.held();
    bool fits_count = pinned_count() + 1 <= m_item_limit;
    bool fits_bytes = size <= m_byte_limit && pinned_bytes() <= m_byte_limit - size;
    return fits_count && fits_bytes;
}

bool clipboard_history::set_pinned(uint64_t id, bool pinned) {
    size_t i = index_of(id);
    if (i == m_entries.size() || (pinned && !can_pin(id))) {
        return false;
    }

    m_entries[i].pinned = pinned;
    evict();
    return true;
}

void clipboard_history::remove(uint64_t id) {
    size_t i = index_of(id);
    if (i == m_entries.size()) {
        return;
    }

    if (is_current(id)) {
        m_front_is_current = false;
    }

    m_entries.erase(m_entries.begin() + static_cast<ptrdiff_t>(i));
}

size_t clipboard_history::clear_unpinned() {
    if (m_front_is_current && !m_entries.front().pinned) {
        m_front_is_current = false;
    }

    size_t before = m_entries.size();
    std::erase_if(m_entries, [](const clipboard_entry& entry) {
        return !entry.pinned;
    });

    return before - m_entries.size();
}

void clipboard_history::clear() {
    m_entries.clear();
    m_front_is_current = false;
    m_private = clipboard_item();
}

size_t clipboard_history::kept_count() const {
    size_t count = 0;
    for (size_t i = m_front_is_current ? 1 : 0; i < m_entries.size(); i++) {
        count += m_entries[i].pinned ? 0 : 1;
    }

    return count;
}

uint64_t clipboard_history::kept_bytes() const {
    uint64_t total = 0;
    for (size_t i = m_front_is_current ? 1 : 0; i < m_entries.size(); i++) {
        total += m_entries[i].pinned ? 0 : m_entries[i].item.held();
    }

    return total;
}

size_t clipboard_history::pinned_count() const {
    return static_cast<size_t>(std::count_if(m_entries.begin(), m_entries.end(), [](const clipboard_entry& entry) {
        return entry.pinned;
    }));
}

uint64_t clipboard_history::pinned_bytes() const {
    uint64_t total = 0;
    for (const clipboard_entry& entry : m_entries) {
        total += entry.pinned ? entry.item.held() : 0;
    }

    return total;
}

/* The current entry becomes an ordinary one, unless it was private or passes the whole budget alone */
void clipboard_history::retire_current() {
    m_private = clipboard_item();
    if (!m_front_is_current) {
        return;
    }

    m_front_is_current = false;
    if (m_entries.front().item.held() > m_byte_limit && !m_entries.front().pinned) {
        m_entries.erase(m_entries.begin());
    }
}

/* The oldest unpinned entries leave first, and the current one never does */
void clipboard_history::evict() {
    size_t first_kept = m_front_is_current ? 1 : 0;
    while (kept_count() > m_item_limit || kept_bytes() > m_byte_limit) {
        size_t oldest = m_entries.size();
        for (size_t i = m_entries.size(); i-- > first_kept;) {
            if (!m_entries[i].pinned) {
                oldest = i;
                break;
            }
        }

        if (oldest == m_entries.size()) {
            return;
        }

        m_entries.erase(m_entries.begin() + static_cast<ptrdiff_t>(oldest));
    }
}

size_t clipboard_history::index_of(uint64_t id) const {
    for (size_t i = 0; i < m_entries.size(); i++) {
        if (m_entries[i].id == id) {
            return i;
        }
    }

    return m_entries.size();
}

/* A MIME type is printable ASCII ended by a NUL inside its field */
static bool is_mime_type(const char* mime, size_t field) {
    size_t len = strnlen(mime, field);
    if (len == 0 || len == field) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = static_cast<unsigned char>(mime[i]);
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }

    return true;
}

bool read_offer(const swp_offer& offer, std::vector<clipboard_rep>* out) {
    if (offer.count > SWP_OFFER_TYPES_MAX) {
        return false;
    }

    std::vector<clipboard_rep> reps;
    for (uint32_t i = 0; i < offer.count; i++) {
        const swp_offer_type& type = offer.types[i];
        if (!is_mime_type(type.mime, sizeof(type.mime)) || type.flags != 0 || type.reserved != 0) {
            return false;
        }

        reps.push_back({ type.mime, type.len, -1 });
    }

    *out = std::move(reps);
    return true;
}

swp_clipboard_types describe_types(const clipboard_item& item) {
    swp_clipboard_types m = {};
    for (const clipboard_rep& rep : item.reps()) {
        if (m.count == SWP_OFFER_TYPES_MAX) {
            break;
        }

        snprintf(m.types[m.count++], SWP_MIME_MAX, "%s", rep.mime_type.c_str());
    }

    return m;
}

/* Bytes outside a URI's unreserved characters are escaped, slashes kept, and each line ends in CR LF */
std::string file_uri_list(std::string_view path) {
    static const char HEX[] = "0123456789ABCDEF";

    std::string list = "file://";
    for (unsigned char byte : path) {
        bool is_kept = isalnum(byte) || byte == '/' || byte == '-' || byte == '.' || byte == '_' || byte == '~';
        if (is_kept) {
            list += static_cast<char>(byte);
        } else {
            list += '%';
            list += HEX[byte >> 4];
            list += HEX[byte & 0xF];
        }
    }

    list += "\r\n";
    return list;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

std::vector<std::string> uri_list_paths(std::string_view list) {
    std::vector<std::string> paths;
    for (size_t at = 0; at < list.size();) {
        size_t end = std::min(list.find('\n', at), list.size());
        std::string_view line = list.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (!line.starts_with("file://")) {
            continue;
        }

        std::string path;
        std::string_view encoded = line.substr(strlen("file://"));
        for (size_t i = 0; i < encoded.size(); i++) {
            bool is_escape = encoded[i] == '%' && i + 2 < encoded.size() && hex_value(encoded[i + 1]) >= 0 &&
                             hex_value(encoded[i + 2]) >= 0;
            if (is_escape) {
                path += static_cast<char>(hex_value(encoded[i + 1]) * 16 + hex_value(encoded[i + 2]));
                i += 2;
            } else {
                path += encoded[i];
            }
        }

        if (path.starts_with('/')) {
            paths.push_back(std::move(path));
        }
    }

    return paths;
}

bool png_size(std::string_view header, uint32_t* width, uint32_t* height) {
    static const char SIGNATURE[] = "\x89PNG\r\n\x1a\n";
    if (header.size() < 24 || header.substr(0, 8) != std::string_view(SIGNATURE, 8) ||
        header.substr(12, 4) != "IHDR") {
        return false;
    }

    auto big_endian = [&header](size_t at) {
        uint32_t value = 0;
        for (size_t i = 0; i < 4; i++) {
            value = value << 8 | static_cast<uint8_t>(header[at + i]);
        }

        return value;
    };

    *width = big_endian(16);
    *height = big_endian(20);
    return true;
}

std::string read_bytes(const clipboard_rep& rep, uint64_t max) {
    uint64_t len = std::min(rep.len, max);
    if (len == 0 || rep.object < 0) {
        return {};
    }

    void* shared = mmap(nullptr, len, PROT_READ, MAP_SHARED, rep.object, 0);
    if (shared == MAP_FAILED) {
        return {};
    }

    std::string bytes(static_cast<const char*>(shared), len);
    munmap(shared, len);

    return bytes;
}

static char folded(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

size_t find_folded(std::string_view text, std::string_view query, size_t from) {
    if (query.empty()) {
        return std::string_view::npos;
    }

    for (size_t at = from; at + query.size() <= text.size(); at++) {
        size_t matched = 0;
        while (matched < query.size() && folded(text[at + matched]) == folded(query[matched])) {
            matched++;
        }

        if (matched == query.size()) {
            return at;
        }
    }

    return std::string_view::npos;
}

#include "clipboard.hpp"

#include <cstring>
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

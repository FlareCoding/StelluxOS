#ifndef STLXDM_CLIPBOARD_HPP
#define STLXDM_CLIPBOARD_HPP

#include <stlxwin/proto.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/* One representation of a clipboard item: its MIME type and the shared memory object holding its bytes, -1 for none */
struct clipboard_rep {
    std::string mime_type;
    uint64_t    len = 0;
    int         object = -1;
};

/* One copy, the same content in up to SWP_OFFER_TYPES_MAX MIME types, closing its objects when it goes */
class clipboard_item {
public:
    clipboard_item() = default;
    clipboard_item(std::vector<clipboard_rep> reps, uint32_t flags);
    ~clipboard_item();

    clipboard_item(clipboard_item&& other) noexcept;
    clipboard_item& operator=(clipboard_item&& other) noexcept;
    clipboard_item(const clipboard_item&) = delete;
    clipboard_item& operator=(const clipboard_item&) = delete;

    /* The representation of exactly `mime_type`, or null */
    const clipboard_rep* find(std::string_view mime_type) const;

    const std::vector<clipboard_rep>& reps() const { return m_reps; }
    uint32_t flags() const { return m_flags; }

private:
    void close_objects();

    std::vector<clipboard_rep> m_reps;
    uint32_t                   m_flags = 0;
};

/* Reads an offer's representations, their objects not yet taken. False when the count passes SWP_OFFER_TYPES_MAX, or a
 * type is not printable ASCII or sets its flags or reserved field. */
bool read_offer(const swp_offer& offer, std::vector<clipboard_rep>* out);

#endif

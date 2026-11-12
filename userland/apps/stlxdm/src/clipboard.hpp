#ifndef STLXDM_CLIPBOARD_HPP
#define STLXDM_CLIPBOARD_HPP

#include <stlxwin/proto.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

/* One representation of a clipboard item: its MIME type and the shared memory object holding its bytes, -1 for none */
struct clipboard_rep {
    std::string mime_type;
    uint64_t    len = 0;
    int         object = -1;
    uint64_t    held = 0;  /* the bytes its object held when it arrived, 0 when only its length is known */
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
    bool empty() const { return m_reps.empty(); }

    /* The bytes its representations hold together */
    uint64_t size() const;

    /* The bytes its objects hold together, which the budgets count, so a length shorter than its object counts the
     * object */
    uint64_t held() const;

private:
    void close_objects();

    std::vector<clipboard_rep> m_reps;
    uint32_t                   m_flags = 0;
};

/* One copy the history keeps, the current clipboard or one it replaced */
struct clipboard_entry {
    uint64_t       id = 0;
    clipboard_item item;
    std::string    label;          /* the copier as the bar names it, or stlxdm's own name for its copies */
    int64_t        copied_ns = 0;  /* CLOCK_MONOTONIC */
    bool           pinned = false;
};

/* Whether two items hold the same content, which the server reads from their objects */
using clipboard_matcher = std::function<bool(const clipboard_item& a, const clipboard_item& b)>;

/* The server's matcher: the same types and lengths in order, and the same bytes read through read-only mappings, a
 * refused mapping counting as different */
bool same_content(const clipboard_item& a, const clipboard_item& b);

/* The clipboard and the copies it replaced, newest first. Unpinned entries besides the current one stay within a budget
 * of entries and bytes, and pinned entries stay within a budget of the same size of their own, so pins never starve the
 * history. A private copy stays out, and the same content copied again moves its entry up under its id. */
class clipboard_history {
public:
    explicit clipboard_history(clipboard_matcher matches);

    void set_limits(uint32_t items, uint64_t bytes);
    uint32_t item_limit() const { return m_item_limit; }
    uint64_t byte_limit() const { return m_byte_limit; }

    /* Makes `item` current, keeping the entry it replaces unless that one was private or alone passes the budget */
    void copy(clipboard_item item, std::string label, int64_t now_ns);

    /* What the clipboard holds, empty while nothing is copied */
    const clipboard_item& current() const;

    /* The listed entries, pinned ones first, each part newest first */
    std::vector<const clipboard_entry*> entries() const;

    const clipboard_entry* find(uint64_t id) const;
    bool is_current(uint64_t id) const;

    /* Makes a kept entry current again as a fresh copy */
    void make_current(uint64_t id, int64_t now_ns);

    /* Whether pinning entry `id` keeps the pins within their budget, and the pin or unpin, false when it is refused */
    bool can_pin(uint64_t id) const;
    bool set_pinned(uint64_t id, bool pinned);

    /* Removing the current entry empties the clipboard, so a deleted copy leaves it too */
    void remove(uint64_t id);
    size_t clear_unpinned();
    void clear();

    /* What the history's budget counts: every unpinned entry but the current one */
    size_t kept_count() const;
    uint64_t kept_bytes() const;

    /* What the pins' budget counts: every pinned entry, the current one included */
    size_t pinned_count() const;
    uint64_t pinned_bytes() const;

private:
    void retire_current();
    void evict();
    size_t index_of(uint64_t id) const;

    clipboard_matcher            m_matches;
    std::vector<clipboard_entry> m_entries;  /* newest first, the current one first while m_front_is_current */
    bool                         m_front_is_current = false;
    clipboard_item               m_private;  /* the current item while it is private */
    uint64_t                     m_next_id = 1;
    uint32_t                     m_item_limit = 0;
    uint64_t                     m_byte_limit = 0;
};

/* Reads an offer's representations, their objects not yet taken. False when the count passes SWP_OFFER_TYPES_MAX, or a
 * type is not printable ASCII or sets its flags or reserved field. */
bool read_offer(const swp_offer& offer, std::vector<clipboard_rep>* out);

/* The CLIPBOARD_TYPES that tells a watching client what `item` holds */
swp_clipboard_types describe_types(const clipboard_item& item);

/* The `text/uri-list` naming one file by its absolute path, which File Manager's copies carry */
std::string file_uri_list(std::string_view path);

/* The paths of the local files a `text/uri-list` names, its comments and other schemes left out */
std::vector<std::string> uri_list_paths(std::string_view list);

/* A PNG's width and height from its first 24 bytes. False when they hold no PNG header. */
bool png_size(std::string_view header, uint32_t* width, uint32_t* height);

/* Whether a picture of `width` by `height` pixels may be decoded for a preview: at most 16 megapixels, enough for a 5K
 * display's capture, and 16384 pixels either way */
bool fits_preview(uint32_t width, uint32_t height);

/* A representation's first `max` bytes, empty when the kernel refuses to map its object */
std::string read_bytes(const clipboard_rep& rep, uint64_t max);

/* Where `query` first occurs in `text` at or after `from`, ignoring ASCII case, npos when it does not or is empty */
size_t find_folded(std::string_view text, std::string_view query, size_t from = 0);

#endif

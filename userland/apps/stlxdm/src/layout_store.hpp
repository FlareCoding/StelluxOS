#ifndef STLXDM_LAYOUT_STORE_HPP
#define STLXDM_LAYOUT_STORE_HPP

#include "layout.hpp"

#include <cstddef>
#include <cstdint>

/* The layouts a person chose, one section per set of monitors, newest
 * first, read once at start and written back whole at every applied
 * request. A section matches the members when its lines and the members
 * pair one to one by monitor, and by output index too among members whose
 * monitors are equal, so a monitor keeps its place on another connector. */
class layout_store {
public:
    static constexpr uint32_t LAYOUTS_KEPT = 16;

    /* The monitor GFXFB_GET_OUTPUT names, empty or 0 where it names none. */
    struct identity {
        char     maker[4] = {};
        char     name[16] = {};
        uint32_t product = 0;
        uint32_t serial = 0;

        bool operator==(const identity&) const = default;
    };

    struct member {
        uint32_t output = 0;
        identity monitor;
    };

    struct line {
        uint32_t output = 0;
        identity monitor;
        int32_t  x = 0;
        int32_t  y = 0;
        int32_t  width = 0;
        int32_t  height = 0;
    };

    struct section {
        uint32_t main = 0;
        uint32_t count = 0;
        line     lines[layout::MEMBERS_MAX];
    };

    /* Keeps the first `LAYOUTS_KEPT` well-formed sections of `text`,
     * logging each malformed one it skips. Keeps none and returns false,
     * logged, when a line other than a comment or a blank one comes before
     * the first section. */
    bool parse(const char* text, size_t length);

    /* Parses the file at `path`. A missing file holds no sections, and one
     * that cannot be read or holds 32 KiB or more is ignored, logged. */
    void read(const char* path);

    /* Fills `out` from the newest section matching `members`, naming each
     * place and the main output by its paired member's output, and
     * returns whether one matched. */
    bool match(const member* members, uint32_t count, layout::arrangement& out) const;

    /* Makes `places` the newest section for `members`, which must be its
     * members in order, dropping every other section matching them and any
     * past the last kept. */
    void remember(const member* members, uint32_t count, const layout& places);

    /* Writes the file's text into `buffer` and returns its length, or the
     * length it needs when that is `size` or more. */
    size_t format(char* buffer, size_t size) const;

    /* Writes the file at `path` through a temporary file renamed over it,
     * making its directory when missing. Returns 0, `ENOSPC` when the
     * text needs 32 KiB or more, `ENAMETOOLONG` when `path` with ".tmp"
     * needs 256 bytes or more, else the errno of the step that failed,
     * `EIO` for a write, close or rename that set none. */
    int write(const char* path) const;

    uint32_t count() const { return m_count; }
    const section& at(uint32_t index) const { return m_sections[index]; }

private:
    bool pair(const section& s, const member* members, uint32_t count, uint32_t* line_of) const;

    section  m_sections[LAYOUTS_KEPT];
    uint32_t m_count = 0;
};

#endif

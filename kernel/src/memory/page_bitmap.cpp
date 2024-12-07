#include <memory/page_bitmap.h>
#include <memory/memory.h>
#include <memory/paging.h>

namespace paging {
__PRIVILEGED_CODE
uint64_t page_frame_bitmap::calculate_required_size(uint64_t system_memory) {
    return PAGE_ALIGN((system_memory / PAGE_SIZE / 8) + 1);
}

__PRIVILEGED_CODE
void page_frame_bitmap::init(uint64_t size, uint8_t* buffer, bool initial_used_value) {
    m_size = size;
    m_buffer = buffer;
    m_next_free_index = 0;

    // Initially mark everything as used
    memset(buffer, initial_used_value ? 0xff : 0x00, size);
}

__PRIVILEGED_CODE
uint64_t page_frame_bitmap::get_size() const {
    return m_size;
}

__PRIVILEGED_CODE
uint64_t page_frame_bitmap::get_next_free_index() const {
    return m_next_free_index;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::mark_page_free(void* paddr) {
    bool result = _set_page_value(paddr, false);
    if (result) {
        uint64_t index = _get_addr_index(paddr);
        if (index < m_next_free_index) {
            m_next_free_index = index; // Update next free index
        }
    }
    return result;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::mark_page_used(void* paddr) {
    bool result = _set_page_value(paddr, true);
    if (result) {
        uint64_t index = _get_addr_index(paddr);
        if (index == m_next_free_index) {
            m_next_free_index = index + 1; // Move to the next index
        }
    }
    return result;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::mark_pages_free(void* paddr, size_t count) {
    uint64_t start_index = _get_addr_index(paddr);

    // Check that we don't go beyond the bitmap buffer
    if ((start_index + count) > (m_size * 8))
        return false;

    for (size_t i = 0; i < count; ++i) {
        void* addr = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(paddr) + i * PAGE_SIZE);
        if (!_set_page_value(addr, false))
            return false;
    }

    if (start_index < m_next_free_index) {
        m_next_free_index = start_index; // Update next free index
    }

    return true;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::mark_pages_used(void* paddr, size_t count) {
    uint64_t start_index = _get_addr_index(paddr);

    // Check that we don't go beyond the bitmap buffer
    if ((start_index + count) > (m_size * 8))
        return false;

    for (size_t i = 0; i < count; ++i) {
        void* addr = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(paddr) + i * PAGE_SIZE);
        if (!_set_page_value(addr, true))
            return false;
    }

    if (start_index <= m_next_free_index) {
        m_next_free_index = start_index + count; // Update next free index
    }

    return true;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::is_page_free(void* paddr) {
    return (_get_page_value(paddr) == false);
}

__PRIVILEGED_CODE
bool page_frame_bitmap::is_page_used(void* paddr) {
    return (_get_page_value(paddr) == true);
}

__PRIVILEGED_CODE
bool page_frame_bitmap::_set_page_value(void* paddr, bool value) {
    uint64_t index = _get_addr_index(paddr);

    // Preventing bitmap buffer overflow
    if (index > (m_size * 8))
        return false;

    uint64_t byte_idx = index / 8;
    uint8_t bit_idx = index % 8;
    uint8_t mask = 0b00000001 << bit_idx;

    // First disable the bit
    m_buffer[byte_idx] &= ~mask;

    // Now enable the bit if needed
    if (value) {
        m_buffer[byte_idx] |= mask;
    }

    return true;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::_get_page_value(void* paddr) {
    uint64_t index = _get_addr_index(paddr);
    uint64_t byte_idx = index / 8;
    uint8_t bit_idx = index % 8;
    uint8_t mask = 0b00000001 << bit_idx;

    return (m_buffer[byte_idx] & mask) > 0;
}

__PRIVILEGED_CODE
uint64_t page_frame_bitmap::_get_addr_index(void* paddr) {
    return reinterpret_cast<uint64_t>(paddr) / PAGE_SIZE;
}
} // namespace paging

#include <memory/page_bitmap.h>
#include <memory/memory.h>
#include <memory/paging.h>

namespace paging {
__PRIVILEGED_CODE page_frame_bitmap::page_frame_bitmap() {}

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
bool page_frame_bitmap::mark_page_free(void* addr) {
    bool result = _set_page_value(addr, false);
    if (result) {
        uint64_t index = _get_addr_index(addr);
        if (index < m_next_free_index) {
            m_next_free_index = index; // Update next free index
        }
    }
    return result;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::mark_page_used(void* addr) {
    bool result = _set_page_value(addr, true);
    if (result) {
        uint64_t index = _get_addr_index(addr);
        if (index == m_next_free_index) {
            m_next_free_index = index + 1; // Move to the next index
        }
    }
    return result;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::mark_pages_free(void* addr, size_t count) {
    uint64_t start_index = _get_addr_index(addr);

    // Check that we don't go beyond the bitmap buffer
    if ((start_index + count) > (m_size * 8))
        return false;

    for (size_t i = 0; i < count; ++i) {
        void* page_addr = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(addr) + i * PAGE_SIZE);
        if (!_set_page_value(page_addr, false))
            return false;
    }

    if (start_index < m_next_free_index) {
        m_next_free_index = start_index; // Update next free index
    }

    return true;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::mark_pages_used(void* addr, size_t count) {
    uint64_t start_index = _get_addr_index(addr);

    // Check that we don't go beyond the bitmap buffer
    if ((start_index + count) > (m_size * 8))
        return false;

    for (size_t i = 0; i < count; ++i) {
        void* page_addr = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(addr) + i * PAGE_SIZE);
        if (!_set_page_value(page_addr, true))
            return false;
    }

    if (start_index <= m_next_free_index) {
        m_next_free_index = start_index + count; // Update next free index
    }

    return true;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::is_page_free(void* addr) {
    return (_get_page_value(addr) == false);
}

__PRIVILEGED_CODE
bool page_frame_bitmap::is_page_used(void* addr) {
    return (_get_page_value(addr) == true);
}

__PRIVILEGED_CODE
void page_frame_bitmap::mark_buffer_address_as_physical() {
    m_is_physical_buffer_address = true;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::_set_page_value(void* addr, bool value) {
    uint64_t index = _get_addr_index(addr);

    // Preventing bitmap buffer overflow
    if (index > (m_size * 8))
        return false;

    uint64_t byte_idx = index / 8;
    uint8_t bit_idx = index % 8;
    uint8_t mask = 0b00000001 << bit_idx;

    // Get the mapped virtual address for the buffer
    uint8_t* vbuffer = m_buffer;
    if (m_is_physical_buffer_address) {
        vbuffer = reinterpret_cast<uint8_t*>(phys_to_virt_linear(m_buffer));
    }

    // First disable the bit
    vbuffer[byte_idx] &= ~mask;

    // Now enable the bit if needed
    if (value) {
        vbuffer[byte_idx] |= mask;
    }

    return true;
}

__PRIVILEGED_CODE
bool page_frame_bitmap::_get_page_value(void* addr) {
    uint64_t index = _get_addr_index(addr);
    uint64_t byte_idx = index / 8;
    uint8_t bit_idx = index % 8;
    uint8_t mask = 0b00000001 << bit_idx;

    // Get the mapped virtual address for the buffer
    uint8_t* vbuffer = m_buffer;
    if (m_is_physical_buffer_address) {
        vbuffer = reinterpret_cast<uint8_t*>(phys_to_virt_linear(m_buffer));
    }

    return (vbuffer[byte_idx] & mask) > 0;
}

__PRIVILEGED_CODE
uint64_t page_frame_bitmap::_get_addr_index(void* addr) {
    return reinterpret_cast<uint64_t>(addr) / PAGE_SIZE;
}
} // namespace paging

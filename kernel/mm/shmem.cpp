#include "mm/shmem.h"

#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"
#include "mm/paging.h"
#include "mm/pmm.h"

namespace mm {

namespace {

constexpr size_t INITIAL_PAGE_CAPACITY = 4;

bool ensure_capacity(shmem* s, size_t needed) {
    if (needed <= s->m_capacity) {
        return true;
    }

    size_t new_cap = s->m_capacity ? s->m_capacity : INITIAL_PAGE_CAPACITY;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    auto* new_pages = static_cast<pmm::phys_addr_t*>(
        heap::kzalloc(new_cap * sizeof(pmm::phys_addr_t)));
    if (!new_pages) {
        return false;
    }

    if (s->m_pages && s->m_capacity > 0) {
        string::memcpy(new_pages, s->m_pages,
                       s->m_capacity * sizeof(pmm::phys_addr_t));
        heap::kfree(s->m_pages);
    }

    s->m_pages = new_pages;
    s->m_capacity = new_cap;
    return true;
}

} // namespace

void shmem::ref_destroy(shmem* self) {
    if (!self) {
        return;
    }

    RUN_ELEVATED({
        if (self->m_pages) {
            for (size_t i = 0; i < self->m_capacity; i++) {
                if (self->m_pages[i] != 0) {
                    pmm::free_page(self->m_pages[i]);
                }
            }
            heap::kfree(self->m_pages);
        }

        heap::kfree_delete(self);
    });
}

shmem* shmem_create(size_t initial_size) {
    shmem* s = nullptr;
    RUN_ELEVATED({
        s = heap::kalloc_new<shmem>();
    });
    if (!s) {
        return nullptr;
    }

    RUN_ELEVATED({
        s->m_pages = nullptr;
        s->m_page_count = 0;
        s->m_capacity = 0;
        s->m_size = 0;
        s->lock.init();
    });

    if (initial_size > 0) {
        int32_t rc = SHMEM_OK;
        RUN_ELEVATED({
            sync::mutex_lock(s->lock);
            rc = shmem_resize_locked(s, initial_size);
            sync::mutex_unlock(s->lock);
        });
        if (rc != SHMEM_OK) {
            shmem::ref_destroy(s);
            return nullptr;
        }
    }

    return s;
}

int32_t shmem_resize_locked(shmem* s, size_t new_size) {
    if (!s) {
        return SHMEM_ERR_INVAL;
    }

    size_t max_alignable = ~(pmm::PAGE_SIZE - 1);
    if (new_size > max_alignable) {
        return SHMEM_ERR_INVAL;
    }

    int32_t result = SHMEM_OK;
    RUN_ELEVATED({
        size_t new_page_count = pmm::page_align_up(new_size) / pmm::PAGE_SIZE;
        size_t old_page_count = s->m_page_count;

        if (new_page_count > old_page_count) {
            if (!ensure_capacity(s, new_page_count)) {
                result = SHMEM_ERR_NO_MEM;
            } else {
                for (size_t i = old_page_count; i < new_page_count; i++) {
                    if (s->m_pages[i] != 0) {
                        string::memset(paging::phys_to_virt(s->m_pages[i]), 0, pmm::PAGE_SIZE);
                        continue;
                    }
                    pmm::phys_addr_t phys = pmm::alloc_page();
                    if (phys == 0) {
                        if (i > old_page_count) {
                            s->m_page_count = i;
                            s->m_size = i * pmm::PAGE_SIZE;
                        }
                        result = SHMEM_ERR_NO_MEM;
                        break;
                    }
                    string::memset(paging::phys_to_virt(phys), 0, pmm::PAGE_SIZE);
                    s->m_pages[i] = phys;
                }
            }
        }

        if (result == SHMEM_OK) {
            if (new_size < s->m_size && new_page_count > 0) {
                size_t tail_off = new_size % pmm::PAGE_SIZE;
                if (tail_off != 0 && s->m_pages[new_page_count - 1] != 0) {
                    auto* page = static_cast<uint8_t*>(
                        paging::phys_to_virt(s->m_pages[new_page_count - 1]));
                    string::memset(page + tail_off, 0, pmm::PAGE_SIZE - tail_off);
                }
            }
            s->m_page_count = new_page_count;
            s->m_size = new_size;
        }
    });
    return result;
}

pmm::phys_addr_t shmem_get_page_locked(shmem* s, size_t page_index) {
    pmm::phys_addr_t phys = 0;
    RUN_ELEVATED({
        if (s && page_index < s->m_page_count) {
            phys = s->m_pages[page_index];
        }
    });
    return phys;
}

ssize_t shmem_read(shmem* s, size_t offset, void* dst, size_t count) {
    if (!s || !dst) {
        return SHMEM_ERR_INVAL;
    }

    ssize_t result = 0;
    RUN_ELEVATED({
        sync::mutex_lock(s->lock);

        if (offset >= s->m_size) {
            result = 0;
        } else {
            if (offset + count > s->m_size) {
                count = s->m_size - offset;
            }

            auto* out = static_cast<uint8_t*>(dst);
            size_t remaining = count;
            size_t pos = offset;

            while (remaining > 0) {
                size_t page_idx = pos / pmm::PAGE_SIZE;
                size_t page_off = pos % pmm::PAGE_SIZE;
                size_t chunk = pmm::PAGE_SIZE - page_off;
                if (chunk > remaining) {
                    chunk = remaining;
                }

                if (page_idx < s->m_page_count && s->m_pages[page_idx] != 0) {
                    auto* src_page = static_cast<uint8_t*>(
                        paging::phys_to_virt(s->m_pages[page_idx]));
                    string::memcpy(out, src_page + page_off, chunk);
                } else {
                    string::memset(out, 0, chunk);
                }

                out += chunk;
                pos += chunk;
                remaining -= chunk;
            }

            result = static_cast<ssize_t>(count);
        }

        sync::mutex_unlock(s->lock);
    });
    return result;
}

ssize_t shmem_write(shmem* s, size_t offset, const void* src, size_t count) {
    if (!s || !src) {
        return SHMEM_ERR_INVAL;
    }

    ssize_t result = 0;
    RUN_ELEVATED({
        sync::mutex_lock(s->lock);

        if (offset >= s->m_size) {
            result = 0;
        } else {
            if (offset + count > s->m_size) {
                count = s->m_size - offset;
            }

            auto* in = static_cast<const uint8_t*>(src);
            size_t remaining = count;
            size_t pos = offset;

            while (remaining > 0) {
                size_t page_idx = pos / pmm::PAGE_SIZE;
                size_t page_off = pos % pmm::PAGE_SIZE;
                size_t chunk = pmm::PAGE_SIZE - page_off;
                if (chunk > remaining) {
                    chunk = remaining;
                }

                if (page_idx >= s->m_page_count || s->m_pages[page_idx] == 0) {
                    break;
                }

                auto* dst_page = static_cast<uint8_t*>(
                    paging::phys_to_virt(s->m_pages[page_idx]));
                string::memcpy(dst_page + page_off, in, chunk);

                in += chunk;
                pos += chunk;
                remaining -= chunk;
            }

            result = static_cast<ssize_t>(count - remaining);
        }

        sync::mutex_unlock(s->lock);
    });
    return result;
}

} // namespace mm

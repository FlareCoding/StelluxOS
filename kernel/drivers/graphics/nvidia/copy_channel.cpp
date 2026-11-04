#include "drivers/graphics/nvidia/copy_channel.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "hw/mmio.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"

namespace drivers::nvidia {

// Where the channel's GPFIFO, push buffer and semaphore map, above the [4 KiB, 2^39) range a context leaves to its user
constexpr uint64_t GPFIFO_ADDRESS      = 1ULL << 39;
constexpr uint64_t PUSH_BUFFER_ADDRESS = GPFIFO_ADDRESS + GPFIFO_BYTES;
constexpr uint64_t SEMAPHORE_ADDRESS   = PUSH_BUFFER_ADDRESS + pmm::PAGE_SIZE;

// The channel's system memory: the push buffer's page, then the semaphore's
constexpr size_t   MEMORY_PAGES     = 2;
constexpr uint64_t SEMAPHORE_OFFSET = pmm::PAGE_SIZE;

// A slot of the push buffer, room for one fill's method words and its release sequence
constexpr uint32_t SLOT_WORDS = 64;
constexpr uint64_t SLOT_BYTES = SLOT_WORDS * sizeof(uint32_t);

static_assert(FILL_WORDS + RELEASE_WORDS <= SLOT_WORDS, "a fill and its release sequence share one slot");
static_assert(COPY_SLOTS * SLOT_BYTES == pmm::PAGE_SIZE, "the push buffer is one page");

copy_channel::copy_channel(
    firmware& fw,
    const gpu_objects& objects,
    video_memory& memory,
    registers bar0,
    failed_mark& failed
)
    : m_space(fw, objects, memory, failed)
    , m_channel(fw, objects, memory, bar0) {
    m_waiters.init();
}

int32_t copy_channel::create(
    const dma::device_profile& card,
    const chip_info& chip,
    const engine_info& engine,
    uint32_t id
) {
    if (m_is_created) {
        return ERR_BAD_STATE;
    }

    int32_t rc = m_space.create();
    if (rc != OK) {
        return rc;
    }

    int32_t dma_rc = dma::OK;
    RUN_ELEVATED({
        dma_rc = dma::alloc_pages(card, MEMORY_PAGES, dma::caching::cached_when_coherent, paging::PAGE_USER, m_memory);
    });

    if (dma_rc != dma::OK) {
        log::error("nvidia: allocating the copy channel's push buffer and semaphore failed: %d", dma_rc);
        rc = ERR_NO_MEMORY;
    }

    if (rc == OK) {
        memory_run push_buffer = {.address = m_memory.phys, .bytes = pmm::PAGE_SIZE};
        page_mapping read_only = {.aperture = page_aperture::system_memory, .kind = 0, .read_only = true};
        rc = m_space.page_tables().map(PUSH_BUFFER_ADDRESS, &push_buffer, 1, read_only);
        m_is_mapped = rc == OK;
    }

    if (rc == OK) {
        memory_run semaphore = {.address = m_memory.phys + SEMAPHORE_OFFSET, .bytes = pmm::PAGE_SIZE};
        page_mapping writable = {.aperture = page_aperture::system_memory, .kind = 0, .read_only = false};
        rc = m_space.page_tables().map(SEMAPHORE_ADDRESS, &semaphore, 1, writable);
    }

    bool gpu_may_reach = false;
    if (rc == OK) {
        channel_setup setup = {.engine = engine, .id = id, .gpfifo_address = GPFIFO_ADDRESS, .is_privileged = true};
        rc = m_channel.create(card, chip, m_space, setup);
        gpu_may_reach = m_channel.keeps_memory();
    }

    if (rc != OK) {
        release_memory(gpu_may_reach);
        if (!gpu_may_reach) {
            m_space.destroy();
        }

        return rc;
    }

    m_copy_class = chip.copy_class;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
        m_submitted = 0;
        m_completed = 0;
        m_is_hung = false;
        m_is_created = true;
        sync::spin_unlock_irqrestore(m_lock, irq);
    });

    return OK;
}

int32_t copy_channel::destroy() {
    bool was_created = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
        was_created = m_is_created;
        m_is_created = false;
        sync::spin_unlock_irqrestore(m_lock, irq);
    });

    if (!was_created) {
        return ERR_BAD_STATE;
    }

    int32_t channel_rc = m_channel.destroy();
    bool gpu_may_reach = m_channel.keeps_memory();
    int32_t memory_rc = release_memory(gpu_may_reach);
    int32_t space_rc = gpu_may_reach ? OK : m_space.destroy();

    if (channel_rc != OK) {
        return channel_rc;
    }

    return memory_rc != OK ? memory_rc : space_rc;
}

int32_t copy_channel::fill(uint64_t offset, uint64_t bytes, uint32_t value) {
    bool is_aligned = offset % sizeof(uint32_t) == 0 && bytes % sizeof(uint32_t) == 0;
    bool is_reachable = lies_inside(offset, bytes, FILL_END_MAX);
    if (!is_aligned || !is_reachable || bytes == 0 || bytes > FILL_BYTES_MAX) {
        return ERR_INVALID_ARG;
    }

    if (!m_is_created) {
        return ERR_BAD_STATE;
    }

    bool is_done = false;
    RUN_ELEVATED(is_done = run_fill(offset, bytes, value));

    return is_done ? OK : ERR_TIMEOUT;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void copy_channel::service() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
    if (m_is_created) {
        read_progress();
        sync::wake_all(m_waiters);
    }

    sync::spin_unlock_irqrestore(m_lock, irq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool copy_channel::run_fill(uint64_t offset, uint64_t bytes, uint32_t value) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
    uint64_t now_ns = clock::now_ns();
    read_progress();

    // At most `COPY_SLOTS - 1` fills run, so a slot is rewritten only once a later release shows the GPU fetched it
    while (!m_is_hung && !is_past_deadline(now_ns) && m_submitted - m_completed >= COPY_SLOTS - 1) {
        irq = sync::wait_until(m_waiters, m_lock, irq, m_deadline_ns);
        now_ns = clock::now_ns();
        read_progress();
    }

    uint64_t sequence = 0;
    if (!m_is_hung && !is_past_deadline(now_ns)) {
        sequence = ++m_submitted;
        submit(sequence, offset, bytes, value);
    }

    while (sequence != 0 && !m_is_hung && !is_past_deadline(now_ns) && m_completed < sequence) {
        irq = sync::wait_until(m_waiters, m_lock, irq, m_deadline_ns);
        now_ns = clock::now_ns();
        read_progress();
    }

    bool is_done = sequence != 0 && m_completed >= sequence;
    bool has_hung = !is_done && !m_is_hung && is_past_deadline(now_ns);
    if (has_hung) {
        m_is_hung = true;
        sync::wake_all(m_waiters);
    }

    sync::spin_unlock_irqrestore(m_lock, irq);

    if (has_hung) {
        log::error("nvidia: the kernel's copy channel left a fill unfinished past its deadline, so it is hung");
    }

    return is_done;
}

void copy_channel::submit(uint64_t sequence, uint64_t offset, uint64_t bytes, uint32_t value) {
    uint64_t slot_offset = (sequence % COPY_SLOTS) * SLOT_BYTES;
    uint32_t* words = reinterpret_cast<uint32_t*>(m_memory.virt + slot_offset);
    write_fill(words, m_copy_class, offset, bytes, value);
    write_release(words + FILL_WORDS, SEMAPHORE_ADDRESS, static_cast<uint32_t>(sequence));

    m_channel.write_entry(make_gpfifo_entry(PUSH_BUFFER_ADDRESS + slot_offset, FILL_WORDS + RELEASE_WORDS));
    m_channel.kick();

    if (sequence == m_completed + 1) {
        m_deadline_ns = clock::now_ns() + m_fill_timeout_ns;
    }
}

void copy_channel::read_progress() {
    // The semaphore holds the low 32 bits of the last sequence number released, fewer than `COPY_SLOTS` behind the last
    // submitted
    uint32_t value = mmio::read32(m_memory.virt + SEMAPHORE_OFFSET);
    uint32_t behind = static_cast<uint32_t>(m_submitted) - value;
    if (behind >= m_submitted - m_completed) {
        return;
    }

    m_completed = m_submitted - behind;
    if (m_completed != m_submitted) {
        m_deadline_ns = clock::now_ns() + m_fill_timeout_ns;
    }
}

bool copy_channel::is_past_deadline(uint64_t now_ns) const {
    return m_submitted != m_completed && now_ns >= m_deadline_ns;
}

int32_t copy_channel::release_memory(bool gpu_may_reach) {
    int32_t rc = OK;
    if (m_is_mapped && !gpu_may_reach) {
        rc = m_space.page_tables().unmap(PUSH_BUFFER_ADDRESS, MEMORY_PAGES * pmm::PAGE_SIZE);
        if (rc != OK) {
            log::error("nvidia: unmapping the copy channel's memory failed: %d, so it stays allocated", rc);
            gpu_may_reach = true;
        }
    }

    if (!gpu_may_reach) {
        RUN_ELEVATED(dma::free_pages(m_memory));
    }

    m_memory = {};
    m_is_mapped = false;

    return rc;
}

} // namespace drivers::nvidia

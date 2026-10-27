// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_VIDEO_MEMORY_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_VIDEO_MEMORY_H

#include "common/list.h"
#include "common/types.h"
#include "drivers/graphics/nvidia/gsp_info.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/vram_allocator.h"
#include "mm/pmm_types.h"
#include "sync/mutex.h"

namespace drivers::nvidia {

// Where the CPU reaches a range of video memory `video_memory::map_for_cpu` mapped
struct cpu_mapping {
    uintptr_t        virt; // where the range starts in the kernel's address space, mapped `paging::PAGE_USER`
    pmm::phys_addr_t phys; // where the range starts in physical addresses, or 0 when the implementation has none
};

/**
 * The card's video memory as the driver's parts use it: ranges handed out for
 * a `vram_use`, each its caller's until the caller releases it, and mappings
 * of those ranges for the CPU. `bar1_video_memory` implements it, and tests
 * replace it with a double over ordinary memory. Its calls can block on a lock
 * and wait on the card, so none may run in an interrupt handler or under a
 * spinlock, and a thread that cannot sleep, such as the boot thread, must call
 * only while no other thread can.
 */
class video_memory {
public:
    virtual ~video_memory() = default;

    /**
     * @brief Hand out `size` bytes of video memory for `use` at a multiple of
     * `alignment`.
     * @param size The bytes needed, rounded up to a multiple of 4 KiB.
     * @param alignment A power of two, any below 4 KiB counting as 4 KiB.
     * @param use What the card does with the memory.
     * @param out Receives the range on `OK`, its size the rounded size, and is
     * left unchanged on every error.
     * @return `OK`, `ERR_INVALID_ARG` when `size` is 0 or overflows when
     * rounded and when `alignment` is 0 or not a power of two, or
     * `ERR_NO_MEMORY` when no memory `use` may lie in has a free range of that
     * size at that alignment or the allocation cannot be recorded. Refused
     * arguments and a failure to record the allocation are logged.
     */
    virtual int32_t allocate(uint64_t size, uint64_t alignment, vram_use use, vram_range& out) = 0;

    /**
     * @brief Take back a range `allocate` handed out, whole and once, first
     * taking down its mapping when it is mapped. Any other range is logged and
     * changes nothing, so a release cannot fail.
     * @param range The range as `allocate` handed it out.
     */
    virtual void release(const vram_range& range) = 0;

    /**
     * @brief Map `range` for the CPU, until `unmap_for_cpu` or `release` takes
     * the mapping down, or until an implementation's own teardown, such as
     * `bar1_video_memory::drop_window`, ends it.
     * @param range A range `allocate` handed out, whole and not mapped.
     * @param out Receives where the CPU reaches the range on `OK`, and is left
     * unchanged on every error.
     * @return `OK`, `ERR_INVALID_ARG` when `range` is not such a range or lies
     * where no mapping reaches, `ERR_NO_MEMORY` when there is no room for the
     * mapping or its record, or `ERR_BAD_STATE` or `ERR_TIMEOUT` when the card
     * does not take the mapping, each logged.
     */
    virtual int32_t map_for_cpu(const vram_range& range, cpu_mapping& out) = 0;

    /**
     * @brief Take down the mapping `map_for_cpu` made of `range`, first
     * completing the CPU's writes through it, so they have reached video
     * memory when the call returns. A range not mapped is logged and changes
     * nothing, and the range counts as unmapped afterwards in every case, so
     * the call returns nothing.
     * @param range The range as `map_for_cpu` mapped it.
     */
    virtual void unmap_for_cpu(const vram_range& range) = 0;
};

/**
 * The card's video memory as `video_memory` gives it to the driver's parts:
 * ranges handed out by its `vram_allocator`, and mappings of those ranges for
 * the CPU through BAR1, where the CPU reaches video memory. It owns BAR1's
 * tables, which `set_up_bar1` allocates, writes into video memory and links
 * into the firmware's page tables, and the window (the part of BAR1 a mapping
 * may use, as `bar1_window_bytes` gives it), which `open_window` opens to
 * mappings once the kernel has mapped BAR1. A boot console that bring-up keeps
 * takes the window's first bytes, mapped onto the same offsets of video memory
 * for the kernel's life.
 *
 * `map_for_cpu` maps a range onto a run of the window, a stretch of its pages,
 * at the lowest offset past the console with room for it. It writes the
 * range's page table entries through BAR0's window into video memory, reads
 * the last one back, which completes those writes before the invalidate, as
 * NVIDIA's driver requires for its own writes to page tables, and invalidates
 * BAR1's cached translations, so no translation cached from the run's sparse
 * entries (entries naming nothing, through which an access does not fault)
 * outlives the mapping. Comparing the entry read back with the one written is
 * this driver's own step, since NVIDIA's driver reads only to flush.
 * `unmap_for_cpu` first completes the CPU's writes through the mapping, with a
 * full fence and a read through the mapping while its entries still translate
 * it, then makes the entries sparse again the same way. NVIDIA's driver also
 * flushes with a full fence and a read through a BAR before it unmaps a BAR's
 * range.
 *
 * When the last entry reads back as another value or the invalidate times out,
 * `map_for_cpu` rewrites the run's entries sparse and frees the run, and
 * `unmap_for_cpu` frees it all the same. A caller must reach BAR1 only through
 * a kept console's bytes or a mapping `map_for_cpu` handed out, until the
 * mapping is taken down. Every invalidate drops all of BAR1's cached
 * translations and such a mapping exists only once its own invalidate
 * finished, so whatever a failed update left cached is dropped before the
 * driver's accesses use it, and once released, the range's memory can be
 * handed out again. The CPU may still read ahead through such a translation,
 * though a read ahead writes nothing. Bring-up's checks read BAR1 where no
 * mapping lies, so that a stale translation shows in their reads. Failing
 * `map_for_cpu` on an invalidate that times out is this driver's own step,
 * since NVIDIA's driver reports no failed invalidate to its caller.
 *
 * The object keeps one record per mapping, sorted by BAR1 offset, on the
 * unprivileged heap, and frees them when it drops the window or is destroyed,
 * leaving BAR1's tables in video memory. Its mutex covers its mappings and the
 * uses of BAR0's window that `map_for_cpu`, `unmap_for_cpu` and `release`
 * make, taken outside the allocator's, so its calls can block as the
 * allocator's do: none may run in an interrupt handler or under a spinlock,
 * and a thread that cannot sleep, such as the boot thread, must call only
 * while no other thread can. `map_for_cpu`, `unmap_for_cpu` and a `release` of
 * a mapped range also spin with the mutex held for up to `DEFAULT_TIMEOUT_NS`
 * in an invalidate's two waits. `set_up_bar1` writes through BAR0's window
 * without it, as the boot thread alone calls it.
 */
class bar1_video_memory : public video_memory {
public:
    /**
     * @brief An object with no regions and no window, whose `allocate` fails
     * until `init` succeeds and whose `map_for_cpu` fails until `open_window`
     * opens a window.
     */
    bar1_video_memory();

    /**
     * @brief Free the record of every mapping.
     */
    ~bar1_video_memory() override;

    bar1_video_memory(const bar1_video_memory&) = delete;
    bar1_video_memory& operator=(const bar1_video_memory&) = delete;

    /**
     * @brief Take the video memory regions of `configuration` for the
     * allocations, as `vram_allocator::init` does.
     * @param configuration The static configuration
     * `read_static_configuration` read.
     * @param video_memory_size The video memory's size in bytes, as
     * `frts_region::video_memory_size` holds it.
     * @return `OK`, or `ERR_BAD_MESSAGE`, logged, as `vram_allocator::init`
     * returns it.
     */
    int32_t init(const static_configuration& configuration, uint64_t video_memory_size);

    /**
     * @brief Set up BAR1's window: allocate BAR1's tables in video memory for
     * the window `bar1_window_bytes` gives `bar1_bytes`, write them through
     * BAR0's window with the first `console_bytes` of the window mapping the
     * same offsets of video memory, and link them into the firmware's page
     * tables with `link_bar1_page_tables`. The window stays closed to mappings
     * until `open_window`. It must be called at most once, after `init` and
     * with the boot thread alone, since the link replaces how BAR1 translates
     * every offset.
     * @param regs BAR0's registers, for its window into video memory and the
     * invalidates. They must stay mapped until `drop_window`.
     * @param bar1_phys Where BAR1 starts in physical addresses.
     * @param bar1_bytes BAR1's size, which must leave a window.
     * @param root_offset Where in video memory the firmware's root page
     * directory for BAR1 lies.
     * @param console_bytes The kept console's size in bytes, as
     * `check_console_region` gives it, or 0 when bring-up keeps no console.
     * @return `OK`, `ERR_NO_MEMORY` when the tables or their CPU copy cannot
     * be allocated, `ERR_BAD_STATE` when the tables would end past what their
     * entries can name, or what `link_bar1_page_tables` returns, each logged.
     * On every error the object has no window, and on an error from the link
     * BAR1 may translate through the tables or the firmware's. Once allocated,
     * the tables' video memory stays handed out for the object's life,
     * whatever the call returns.
     */
    int32_t set_up_bar1(
        registers regs,
        pmm::phys_addr_t bar1_phys,
        uint64_t bar1_bytes,
        uint64_t root_offset,
        uint64_t console_bytes
    );

    /**
     * @brief Open to mappings the window `set_up_bar1` set up, once the kernel
     * has mapped BAR1 at `bar1_virt`, which must stay mapped until
     * `drop_window`. BAR1 must be mapped only after `set_up_bar1` returns
     * `OK`, since a write-combining mapping lets the CPU read ahead, and a
     * read through BAR1 before the link can fault the card's MMU. With no
     * window set up, the call opens none.
     * @param bar1_virt Where the kernel's mapping of BAR1 starts.
     */
    void open_window(uintptr_t bar1_virt);

    /**
     * @brief Hand out `size` bytes of video memory for `use` at a multiple of
     * `alignment`, as `vram_allocator::allocate` does.
     * @param size The bytes needed, as `vram_allocator::allocate` takes them.
     * @param alignment The alignment, as `vram_allocator::allocate` takes it.
     * @param use What the card does with the memory.
     * @param out Receives the range on `OK`, as `vram_allocator::allocate`
     * leaves it.
     * @return What `vram_allocator::allocate` returns.
     */
    int32_t allocate(uint64_t size, uint64_t alignment, vram_use use, vram_range& out) override;

    /**
     * @brief Take back a range `allocate` handed out, as
     * `vram_allocator::release` does, first unmapping it as `unmap_for_cpu`
     * does when it is mapped.
     * @param range The range as `allocate` handed it out.
     */
    void release(const vram_range& range) override;

    /**
     * @brief Map `range` for the CPU through BAR1, at the lowest offset of the
     * window past the console with room for it. Once the call returns `OK`, an
     * access through BAR1 uses the new entries. The mapping lasts until
     * `unmap_for_cpu` or `release` takes it down, or until `drop_window`
     * forgets it.
     * @param range A range `allocate` handed out, whole and not mapped.
     * @param out Receives where the CPU reaches the range on `OK`, and is left
     * unchanged on every error.
     * @return `OK`, `ERR_INVALID_ARG` when `range` is not such a range or ends
     * past the 128 GiB a page table entry can name, `ERR_NO_MEMORY` when the
     * window, open or not, has no free run of the range's size or the
     * mapping's record cannot be allocated, `ERR_BAD_STATE` when the last
     * entry reads back as another value, or `ERR_TIMEOUT` when the invalidate
     * times out, each logged. On `ERR_BAD_STATE` and `ERR_TIMEOUT` the entries
     * are rewritten sparse with no invalidate, and the run is free again. On
     * `ERR_INVALID_ARG` and `ERR_NO_MEMORY` BAR1's tables are left unwritten.
     */
    int32_t map_for_cpu(const vram_range& range, cpu_mapping& out) override;

    /**
     * @brief Take down the mapping `map_for_cpu` made of `range`: complete the
     * CPU's writes through it with a full fence and a read through it, make
     * its entries sparse and invalidate BAR1's cached translations, after
     * which an access through BAR1 uses the sparse entries. A range not mapped
     * is logged and changes nothing. When the last entry reads back as another
     * value or the invalidate times out, which is logged, the run is freed all
     * the same. In every case the range counts as unmapped afterwards, so the
     * call returns nothing.
     * @param range The range as `map_for_cpu` mapped it.
     */
    void unmap_for_cpu(const vram_range& range) override;

    /**
     * @brief Forget BAR1's window, so no later call of the object reaches BAR0
     * or BAR1. `map_for_cpu` then fails as before `open_window`, and the
     * mappings' records are freed, their entries left in video memory as they
     * are. Allocations stay handed out. It must be called before either BAR is
     * unmapped.
     */
    void drop_window();

private:
    friend struct video_memory_test_access;

    // A run of BAR1's window holding the mapping of `range`
    struct mapping {
        uint64_t   bar1_offset;
        vram_range range;
        list::node link;
    };

    // Records the window `set_up_bar1` set up, with its tables at `tables_offset`, closed until `open_window`
    void record_window(
        registers regs,
        pmm::phys_addr_t bar1_phys,
        uint64_t bar1_bytes,
        uint64_t root_offset,
        uint64_t tables_offset,
        uint64_t console_bytes
    );

    // `map_for_cpu` with the mutex held
    int32_t map_locked(const vram_range& range, cpu_mapping& out);

    // Takes down `held`, a mapping, and frees its record whether or not the update succeeds, with the mutex held
    void unmap_locked(mapping* held);

    // The lowest offset of the window past the console where `size` bytes are free, with the mutex held
    bool find_free_run(uint64_t size, uint64_t& bar1_offset_out);

    // Frees every mapping's record, with the mutex held or no other thread calling
    void free_mappings();

    // Removes `held` from the records and frees it, with the mutex held
    void free_mapping(mapping* held);

    // The mapping of exactly `range`, or null, with the mutex held
    mapping* find_mapping(const vram_range& range);

    // Points the entries of `held`'s run at its range's pages, or makes them sparse, with the mutex held
    int32_t write_run_entries(const mapping& held, bool maps_range);

    // Writes the run's entries, reads the last one back and invalidates, with the mutex held
    int32_t update_run(const mapping& held, bool maps_range);

    vram_allocator                      m_allocator;
    registers                           m_regs{0};
    uintptr_t                           m_bar1_virt = 0;
    pmm::phys_addr_t                    m_bar1_phys = 0;
    uint64_t                            m_bar1_bytes = 0;
    uint64_t                            m_root_offset = 0;
    uint64_t                            m_tables_offset = 0;
    uint64_t                            m_window_bytes = 0;          // 0 while the window is closed
    uint64_t                            m_console_bytes = 0;
    uint64_t                            m_invalidate_timeout_ns = DEFAULT_TIMEOUT_NS;
    list::head<mapping, &mapping::link> m_mappings;                  // sorted by BAR1 offset
    sync::mutex                         m_lock;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_VIDEO_MEMORY_H

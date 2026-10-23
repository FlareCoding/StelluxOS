#ifndef STELLUX_MM_DEVICE_BACKING_H
#define STELLUX_MM_DEVICE_BACKING_H

#include "common/types.h"
#include "rc/reaper.h"
#include "rc/ref_counted.h"

namespace mm {

/**
 * A device backing is the owner of device memory that processes map. It keeps
 * the memory allocated while any mapping can still reach it, since its driver
 * must not free the memory before `release_memory()` hands it back.
 *
 * A driver derives a class from it that implements `release_memory()`, and
 * creates one object of that class for each piece of memory it lets processes
 * map, such as a buffer. For each mapping `mm_context_map_device()` makes with
 * the object, mm holds a reference while the mapping lasts, so the memory
 * stays allocated until every mapping is unmapped and no CPU can still
 * translate it. A split (a partial unmap or protection change) gives each part
 * a reference of its own, and two mappings that merge keep one. The driver
 * holds whatever references it needs besides.
 *
 * Dropping a reference never blocks, so an interrupt handler or a holder of
 * `mm_context::lock` may drop one, but code that may drop the last one must be
 * elevated, since `ref_destroy()` is privileged. Dropping the last one only
 * queues the backing for the reaper, the kernel task that runs deferred
 * cleanup. The reaper calls `release_memory()` once, holding no lock, so a
 * driver never releases memory inside an interrupt handler or under a lock the
 * dropping code holds. A derived class must not declare a `ref_destroy()` of
 * its own, so that every last drop reaches the reaper.
 */
class device_backing : public rc::ref_counted<device_backing> {
public:
    /**
     * @brief Queue the backing for the reaper. `rc::strong_ref` calls it when
     * the last reference is dropped.
     * @param self The backing whose last reference was dropped.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(device_backing* self);

protected:
    device_backing();
    ~device_backing() = default;

private:
    /**
     * @brief Hand the memory back to its driver and free this object. Runs
     * once, in the reaper's task, after the last reference is dropped. It must
     * not block, since the reaper runs no other cleanup until it returns.
     */
    virtual void release_memory() = 0;

    /**
     * @brief Call `release_memory()` for the reaper. The call reads the
     * object, which may live on the privileged heap.
     * @param self The backing whose last reference was dropped.
     * @return `rc::reaper::DONE`, since the release runs once.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static rc::reaper::cleanup_result reap_backing(device_backing* self);

    /**
     * @brief Call `reap_backing()` on the backing that holds `node`.
     * @param node The backing's reaper node.
     * @return What `reap_backing()` returns.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static rc::reaper::cleanup_result reap_backing_thunk(rc::reaper::dead_node* node);

    rc::reaper::dead_node m_reaper_node;
};

} // namespace mm

#endif // STELLUX_MM_DEVICE_BACKING_H

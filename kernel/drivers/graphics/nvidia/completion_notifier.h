// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_COMPLETION_NOTIFIER_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_COMPLETION_NOTIFIER_H

#include "common/types.h"
#include "dma/dma.h"
#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/nvidia.h"

namespace drivers::nvidia {

// NVIDIA's NV_DMA_EVO_NOTIFIER_SIZE, the bytes of the page `completion_notifier` holds
constexpr uint64_t COMPLETION_NOTIFIER_BYTES = 0x1000;

// NVIDIA's NV_DISP_NOTIFIER__0_STATUS, bits 31:30 of the notifier's first word, and its values: NOT_BEGUN 0, which
// `clear` writes, and FINISHED 2, which the display engine writes once an update naming the notifier has finished
constexpr uint32_t NOTIFIER_STATUS_SHIFT    = 30;
constexpr uint32_t NOTIFIER_STATUS_FINISHED = 2;

// NVIDIA's NV_EVO_NOTIFIER_SHORT_TIMEOUT_USEC, 3 seconds, the wait a caller of `wait_finished` gives an update
constexpr uint64_t COMPLETION_NOTIFIER_WAIT_NS = 3000 * NS_PER_MS;

/**
 * The core channel's completion notifier, a page of system memory where the
 * display engine (the GPU's unit that scans images out to monitors) writes
 * whether an update of the core channel has finished, so the driver can wait
 * for it. The core channel is the display channel that sets up heads, links
 * and which head owns each window, and an update is the method of the core
 * channel that applies the methods written before it. The display engine
 * reaches the page through a context DMA, an object whose record in the
 * display's instance memory names the page, and writes the page at an update
 * that names that context DMA once it is bound to the core channel. The
 * instance memory is the video memory where the display engine looks up the
 * memory its channels may read. `display_engine` holds the core channel's
 * notifier, allocating its page and binding its context DMA in `set_up`. The
 * page is never freed once allocated, as the core channel's push buffer is
 * not: once its context DMA is bound, nothing undoes the binding, and a
 * channel released with the hardware kept as it is stays allocated in the
 * hardware, where NVIDIA's modesetting module frees the notifier and its
 * context DMA after the channel's release and the push buffer's. A page whose
 * binding is refused is kept as well, rather than freed by a call made only
 * for that refusal, so a path that fails `attach()` loses one page, where that
 * module frees the notifier with the core channel at once.
 */
class completion_notifier {
public:
    /**
     * @brief Allocate the notifier's page in DMA pages the card reaches,
     * cached only when the card's DMA is coherent, as the core channel's push
     * buffer is, since the notifier's context DMA has the display engine read
     * and write it with snooping on, where NVIDIA's modesetting module first
     * tries write-combined memory with snooping off when the bus allows it. It
     * must be called at most once, since a second call would replace the page
     * with a new one while the first stays allocated and the notifier's
     * context DMA may still name it.
     * @param card The card's DMA profile.
     * @return `OK`, or `ERR_NO_MEMORY` when the page cannot be allocated,
     * logged.
     */
    int32_t allocate(const dma::device_profile& card);

    /**
     * @brief The page's physical address, a multiple of the page size, 0 until
     * `allocate` succeeds.
     */
    uint64_t physical_address() const;

    /**
     * @brief Write the not-begun status into the notifier's first word, as
     * NVIDIA's modesetting module does right before each update that names the
     * notifier, so the finished status `wait_finished` waits for can only come
     * from that update. `allocate` must have succeeded.
     */
    void clear();

    /**
     * @brief The notifier's first word as it reads now, last written by
     * `clear` or the display engine, its status in bits 31:30. `allocate` must
     * have succeeded.
     */
    uint32_t status_word() const;

    /**
     * @brief Spin until the display engine writes the finished status into the
     * notifier's first word or `deadline_ns` passes. Once the deadline passes,
     * a core channel whose GET reads PUT has read every kicked method, which
     * NVIDIA's modesetting module takes as an update that finished without
     * writing the notifier: the wait logs the lost notification with the word
     * and returns `OK`, as that module goes on as if the status read finished.
     * With GET anywhere but at PUT, where that module waits on with no
     * deadline, the wait returns `ERR_TIMEOUT`, logged. The wait never sleeps,
     * since the boot thread waits before interrupts are on, and spins until
     * the deadline, so it must never run under a spinlock or in an interrupt
     * handler. `allocate` must have succeeded.
     * @param core The core channel, whose GET and PUT tell an update the
     * display engine has read from one it has not.
     * @param deadline_ns A time as `clock::now_ns` reads it,
     * `COMPLETION_NOTIFIER_WAIT_NS` after the update's kick by NVIDIA's rule.
     * @return `OK` when the status reads finished or the notification is lost,
     * else `ERR_TIMEOUT`.
     */
    int32_t wait_finished(const display_channel& core, uint64_t deadline_ns) const;

private:
    friend struct completion_notifier_test_access;

    dma::buffer m_page = {};
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_COMPLETION_NOTIFIER_H

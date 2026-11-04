#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_ADDRESS_SPACE_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_ADDRESS_SPACE_H

#include "common/types.h"
#include "drivers/graphics/nvidia/context_page_tables.h"
#include "drivers/graphics/nvidia/failed_mark.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/gpu_objects.h"
#include "drivers/graphics/nvidia/rm.h"
#include "drivers/graphics/nvidia/video_memory.h"

namespace drivers::nvidia {

/**
 * A GPU address space: the firmware's address space object, allocated
 * externally owned under the device, whose page tables are the kernel's
 * `context_page_tables`, and whose translations only the kernel writes.
 */
class address_space {
public:
    // An object holding nothing, using `fw`, `objects`, `memory` and `failed`, which must outlive it
    address_space(firmware& fw, const gpu_objects& objects, video_memory& memory, failed_mark& failed);

    address_space(const address_space&) = delete;
    address_space& operator=(const address_space&) = delete;

    /**
     * @brief Create the page tables, allocate the firmware's object
     * externally owned, and hand it the tables' root page directory.
     * @return `OK`, `ERR_BAD_STATE` while the object exists, an error of
     * `context_page_tables::create`, or the error the firmware returns, after
     * releasing what the call created unless the firmware refuses. Every
     * firmware error is logged.
     */
    int32_t create();

    /**
     * @brief Take the root page directory back from the firmware, release its
     * object, then destroy the page tables. Every channel group in the
     * address space must be released first.
     * @return `OK`, `ERR_BAD_STATE` before `create`, the first error the
     * firmware returns, which is logged, or an error of
     * `context_page_tables::destroy`. After a refused take-back the object
     * and the tables stay until a later `destroy`, and after a refused
     * release the object stays in the firmware for good.
     */
    int32_t destroy();

    // The firmware's handle of the address space, which a channel group's allocation names, or 0 when none is held
    rm::handle handle() const;

    context_page_tables& page_tables();

private:
    firmware&           m_firmware;
    const gpu_objects&  m_objects;
    context_page_tables m_page_tables;
    rm::handle          m_handle = 0;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_ADDRESS_SPACE_H

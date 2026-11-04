// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/address_space.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"

namespace drivers::nvidia {

// The root page directory's entries in use, 128 TiB each across the 49-bit address space
constexpr uint32_t ROOT_ENTRIES_IN_USE = 4;

// NVIDIA's subDeviceId of the driver's only subdevice, its instance 0 plus 1
constexpr uint32_t SUBDEVICE_ID = 1;

address_space::address_space(firmware& fw, const gpu_objects& objects, video_memory& memory, failed_mark& failed)
    : m_firmware(fw), m_objects(objects), m_page_tables(memory, failed) {}

int32_t address_space::create() {
    if (m_handle != 0) {
        return ERR_BAD_STATE;
    }

    int32_t rc = m_page_tables.create();
    if (rc != OK) {
        return rc;
    }

    rm::handle object = m_firmware.new_handle();
    rm::address_space_params params = {};
    params.flags = rm::EXTERNALLY_OWNED_ADDRESS_SPACE;
    rc = m_firmware.allocate(m_objects.device, object, rm::CLASS_ADDRESS_SPACE, &params, sizeof(params));
    if (rc != OK) {
        log::error("nvidia: allocating a GPU address space failed: %d", rc);
        m_page_tables.destroy();
        return rc;
    }

    rm::set_page_directory_params directory = {};
    directory.address = m_page_tables.directory_offset();
    directory.entry_count = ROOT_ENTRIES_IN_USE;
    directory.flags = rm::PAGE_DIRECTORY_IN_VIDEO_MEMORY | rm::PAGE_DIRECTORY_ALL_CHANNELS;
    directory.address_space = object;
    directory.subdevice_id = SUBDEVICE_ID;

    rc = m_firmware.control(m_objects.device, rm::CONTROL_SET_PAGE_DIRECTORY, &directory, sizeof(directory));
    if (rc != OK) {
        log::error("nvidia: handing a GPU address space its page directory failed: %d", rc);
        m_firmware.release(object);
        m_page_tables.destroy();
        return rc;
    }

    m_handle = object;
    return OK;
}

int32_t address_space::destroy() {
    if (m_handle == 0) {
        return ERR_BAD_STATE;
    }

    rm::unset_page_directory_params directory = {};
    directory.address_space = m_handle;
    directory.subdevice_id = SUBDEVICE_ID;
    int32_t rc = m_firmware.control(m_objects.device, rm::CONTROL_UNSET_PAGE_DIRECTORY, &directory, sizeof(directory));
    if (rc != OK) {
        log::error("nvidia: taking back a GPU address space's page directory failed: %d", rc);
        return rc;
    }

    // The take-back gave every channel of the space the firmware's own directory, so the tables are free either way
    int32_t release_rc = m_firmware.release(m_handle);
    if (release_rc != OK) {
        log::error("nvidia: releasing a GPU address space failed: %d", release_rc);
    }

    m_handle = 0;
    rc = m_page_tables.destroy();
    return release_rc != OK ? release_rc : rc;
}

rm::handle address_space::handle() const {
    return m_handle;
}

context_page_tables& address_space::page_tables() {
    return m_page_tables;
}

} // namespace drivers::nvidia

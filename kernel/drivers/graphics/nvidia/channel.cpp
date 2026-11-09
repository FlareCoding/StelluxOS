// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/channel.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "hw/barrier.h"
#include "hw/mmio.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"

namespace drivers::nvidia {

// NVIDIA's NV_RAMIN_ALLOC_SIZE, the bytes of an instance block, also its alignment (1 << NV_RAMIN_BASE_SHIFT, 4 KiB),
// and the bytes of its RAMFC (NV_RAMIN_RAMFC), which the block starts with
constexpr uint64_t INSTANCE_BLOCK_BYTES = 0x1000;
constexpr uint64_t RAMFC_BYTES          = 0x200;

// NVIDIA's NV_RAMUSERD_CHAN_SIZE, the bytes of a USERD, and NV_RAMUSERD_GP_PUT, the byte offset in USERD of the word
// holding the index of the slot after the last entry the GPU may fetch
constexpr uint64_t USERD_BYTES  = 0x200;
constexpr uint32_t USERD_GP_PUT = 0x8C;

// NVIDIA's NV_VIRTUAL_FUNCTION_DOORBELL, in the block at 0xB80000
constexpr uint32_t DOORBELL = 0xBB0090;

// The PBDMA_FAULTED and ENG_FAULTED bits of NVIDIA's NV_CHRAM_CHANNEL(i), set at a fault in fetching the channel's
// methods or in running them
constexpr uint32_t CHANNEL_RAM_PBDMA_FAULTED = 1u << 4;
constexpr uint32_t CHANNEL_RAM_ENG_FAULTED   = 1u << 5;

// What the word reads on a card that stopped responding, both fault bits set though nothing faulted
constexpr uint32_t CHANNEL_RAM_CARD_GONE = 0xFFFFFFFF;

// The type name of a fault record that names no fault. Its type reads 0, as a page directory fault's does
constexpr char NO_MMU_FAULT[] = "NONE";

// NVIDIA's NVB0B5_ALLOCATION_PARAMETERS_VERSION_1, the version of the copy class's parameters naming an engine type
constexpr uint32_t COPY_OBJECT_VERSION = 1;

static_assert(GPFIFO_BYTES % pmm::PAGE_SIZE == 0, "the GPFIFO maps as whole pages");

static int32_t allocate_system_memory(const dma::device_profile& card, uint64_t bytes, dma::buffer& out) {
    size_t pages = (bytes + pmm::PAGE_SIZE - 1) / pmm::PAGE_SIZE;
    int32_t rc = dma::OK;
    RUN_ELEVATED(rc = dma::alloc_pages(card, pages, dma::caching::cached_when_coherent, paging::PAGE_USER, out));
    return rc == dma::OK ? OK : ERR_NO_MEMORY;
}

static rm::memory_desc_params system_memory_desc(const dma::buffer& buffer, uint64_t bytes) {
    rm::memory_desc_params desc = {};
    desc.base = buffer.phys;
    desc.size = bytes;
    desc.address_space = rm::ADDRESS_SPACE_SYSTEM_MEMORY;
    desc.cpu_caching = rm::CPU_CACHING_CACHED;
    return desc;
}

static rm::memory_desc_params video_memory_desc(uint64_t offset, uint64_t bytes) {
    rm::memory_desc_params desc = {};
    desc.base = offset;
    desc.size = bytes;
    desc.address_space = rm::ADDRESS_SPACE_VIDEO_MEMORY;
    desc.cpu_caching = rm::CPU_CACHING_UNCACHED;
    return desc;
}

channel::channel(firmware& fw, const gpu_objects& objects, video_memory& memory, registers bar0)
    : m_firmware(fw), m_objects(objects), m_video_memory(memory), m_bar0(bar0) {}

int32_t channel::create(
    const dma::device_profile& card,
    const chip_info& chip,
    address_space& space,
    const channel_setup& setup
) {
    int32_t rc = allocate(card, chip, space, setup);
    if (rc != OK) {
        return rc;
    }

    rc = add_copy_object(chip, setup.engine.type);
    if (rc == OK) {
        rc = schedule();
    }

    if (rc != OK) {
        release_objects();
        release_memory();
    }

    return rc;
}

int32_t channel::allocate(
    const dma::device_profile& card,
    const chip_info& chip,
    address_space& space,
    const channel_setup& setup
) {
    if (m_group != 0) {
        return ERR_BAD_STATE;
    }

    m_keeps_memory = false;
    if (setup.id >= CHANNEL_IDS) {
        return ERR_INVALID_ARG;
    }

    int32_t rc = allocate_memory(card);
    if (rc == OK) {
        memory_run gpfifo = {.address = m_gpfifo.phys, .bytes = GPFIFO_BYTES};
        page_mapping read_only = {.aperture = page_aperture::system_memory, .kind = 0, .read_only = true};
        rc = space.page_tables().map(setup.gpfifo_address, &gpfifo, 1, read_only);
        if (rc == OK) {
            m_gpfifo_tables = &space.page_tables();
            m_gpfifo_address = setup.gpfifo_address;
        }
    }

    if (rc == OK) {
        rc = allocate_channel(chip, space, setup);
    }

    if (rc != OK) {
        release_objects();
        release_memory();
        return rc;
    }

    m_id = setup.id;
    m_engine_type = setup.engine.type;
    m_doorbell_token = doorbell_token(setup.engine.runlist, setup.id);
    m_channel_ram_entry = setup.engine.channel_ram + setup.id * CHANNEL_RAM_ENTRY_BYTES;
    m_put = 0;

    return OK;
}

int32_t channel::add_object(uint32_t object_class, void* params, size_t bytes) {
    if (m_channel == 0 || m_object_count == CHANNEL_OBJECTS_MAX) {
        return ERR_BAD_STATE;
    }

    rm::handle object = m_firmware.new_handle();
    int32_t rc = m_firmware.allocate(m_channel, object, object_class, params, bytes);
    if (rc != OK) {
        log::error("nvidia: allocating channel %u's object of class 0x%X failed: %d", m_id, object_class, rc);
        return rc;
    }

    m_objects_held[m_object_count] = object;
    m_object_count++;

    return OK;
}

int32_t channel::add_copy_object(const chip_info& chip, uint32_t engine_type) {
    rm::copy_object_params params = {.version = COPY_OBJECT_VERSION, .engine_type = engine_type};
    return add_object(chip.copy_class, &params, sizeof(params));
}

int32_t channel::schedule() {
    if (m_channel == 0) {
        return ERR_BAD_STATE;
    }

    rm::bind_channel_params bind = {.engine_type = m_engine_type};
    int32_t rc = m_firmware.control(m_channel, rm::CONTROL_BIND_CHANNEL, &bind, sizeof(bind));
    if (rc != OK) {
        log::error("nvidia: binding channel %u to its engine failed: %d", m_id, rc);
        return rc;
    }

    rm::schedule_channel_params schedule = {.enable = 1, .skip_submit = 0};
    rc = m_firmware.control(m_channel, rm::CONTROL_SCHEDULE_CHANNEL, &schedule, sizeof(schedule));
    if (rc != OK) {
        log::error("nvidia: scheduling channel %u failed: %d", m_id, rc);
        return rc;
    }

    return OK;
}

int32_t channel::destroy() {
    if (m_group == 0) {
        return ERR_BAD_STATE;
    }

    int32_t objects_rc = release_objects();
    int32_t memory_rc = release_memory();

    return objects_rc != OK ? objects_rc : memory_rc;
}

int32_t channel::destroy_keeping_memory() {
    if (m_group == 0) {
        return ERR_BAD_STATE;
    }

    m_keeps_memory = true;
    int32_t rc = release_objects();

    return rc;
}

void channel::write_entry(const gpfifo_entry& entry) {
    uintptr_t slot = m_gpfifo.virt + static_cast<uintptr_t>(m_put) * GPFIFO_ENTRY_BYTES;
    mmio::write32(slot, entry.words[0]);
    mmio::write32(slot + sizeof(uint32_t), entry.words[1]);

    m_put = (m_put + 1) % GPFIFO_ENTRIES;
}

void channel::kick() {
    // The GPU reads GP_PUT once the doorbell rings, then fetches the entries and the methods before it
    barrier::dma_write();
    mmio::write32(m_userd.virt + USERD_GP_PUT, m_put);
    barrier::dma_write();
    m_bar0.write(DOORBELL, m_doorbell_token);
}

uint32_t channel::id() const {
    return m_id;
}

rm::handle channel::handle() const {
    return m_channel;
}

bool channel::has_faulted() const {
    uint32_t entry = m_bar0.read(m_channel_ram_entry);
    bool is_card_gone = entry == CHANNEL_RAM_CARD_GONE;
    return !is_card_gone && (entry & (CHANNEL_RAM_PBDMA_FAULTED | CHANNEL_RAM_ENG_FAULTED)) != 0;
}

bool channel::read_mmu_fault(uint64_t& address_out) {
    rm::mmu_fault_info_params record = {};
    int32_t rc = m_firmware.control(m_channel, rm::CONTROL_GET_MMU_FAULT_INFO, &record, sizeof(record));
    if (rc != OK) {
        log::error("nvidia: asking the GSP's firmware for channel %u's MMU fault record failed: %d", m_id, rc);
        return false;
    }

    record.type_name[rm::MMU_FAULT_NAME_BYTES - 1] = '\0';
    if (string::strcmp(record.type_name, NO_MMU_FAULT) == 0) {
        return false;
    }

    address_out = static_cast<uint64_t>(record.address_high) << 32 | record.address_low;
    log::info(
        "nvidia: the GSP's firmware records an MMU fault of type %s at GPU address 0x%lX on channel %u",
        record.type_name,
        address_out,
        m_id
    );

    return true;
}

bool channel::keeps_memory() const {
    return m_keeps_memory;
}

int32_t channel::allocate_memory(const dma::device_profile& card) {
    vram_range block = {};
    int32_t rc = m_video_memory.allocate(INSTANCE_BLOCK_BYTES, INSTANCE_BLOCK_BYTES, vram_use::general, block);
    if (rc != OK) {
        return rc;
    }

    m_instance_block = block;

    cpu_mapping mapping = {};
    rc = m_video_memory.map_for_cpu(m_instance_block, mapping);
    if (rc != OK) {
        return rc;
    }

    for (uint64_t offset = 0; offset < INSTANCE_BLOCK_BYTES; offset += sizeof(uint64_t)) {
        mmio::write64(mapping.virt + offset, 0);
    }

    m_video_memory.unmap_for_cpu(m_instance_block);

    rm::fault_method_buffer_size_params size = {};
    rc = m_firmware.control(m_objects.subdevice, rm::CONTROL_GET_FAULT_METHOD_BUFFER_SIZE, &size, sizeof(size));
    if (rc != OK) {
        log::error("nvidia: asking the GSP's firmware for a fault method buffer's size failed: %d", rc);
        return rc;
    }

    if (size.size == 0) {
        log::error("nvidia: the GSP's firmware gave a fault method buffer's size as 0");
        return ERR_BAD_MESSAGE;
    }

    if (allocate_system_memory(card, USERD_BYTES, m_userd) != OK) {
        return ERR_NO_MEMORY;
    }

    if (allocate_system_memory(card, GPFIFO_BYTES, m_gpfifo) != OK) {
        return ERR_NO_MEMORY;
    }

    for (dma::buffer& buffer : m_method_buffers) {
        if (allocate_system_memory(card, size.size, buffer) != OK) {
            return ERR_NO_MEMORY;
        }
    }

    m_method_buffer_bytes = size.size;

    return OK;
}

int32_t channel::allocate_channel(const chip_info& chip, address_space& space, const channel_setup& setup) {
    rm::channel_group_params group_params = {};
    group_params.address_space = space.handle();
    group_params.engine_type = setup.engine.type;

    rm::handle group = m_firmware.new_handle();
    int32_t rc = m_firmware.allocate(
        m_objects.device,
        group,
        rm::CLASS_CHANNEL_GROUP,
        &group_params,
        sizeof(group_params)
    );
    if (rc != OK) {
        log::error("nvidia: allocating a channel group failed: %d", rc);
        return rc;
    }

    m_group = group;

    rm::channel_params params = {};
    params.gpfifo_address = setup.gpfifo_address;
    params.gpfifo_entries = GPFIFO_ENTRIES;
    params.flags = channel_allocation_flags(setup.id, setup.is_privileged);
    params.engine_type = setup.engine.type;

    params.internal_flags = setup.is_privileged ? rm::CHANNEL_PRIVILEGE_KERNEL : rm::CHANNEL_PRIVILEGE_USER;
    params.internal_flags |= rm::CHANNEL_NO_ERROR_NOTIFIERS;

    params.instance_block = video_memory_desc(m_instance_block.offset, INSTANCE_BLOCK_BYTES);
    params.userd = system_memory_desc(m_userd, USERD_BYTES);
    params.ramfc = video_memory_desc(m_instance_block.offset, RAMFC_BYTES);
    params.method_buffer = system_memory_desc(m_method_buffers[0], m_method_buffer_bytes);

    rm::handle object = m_firmware.new_handle();
    rc = m_firmware.allocate(m_group, object, chip.channel_class, &params, sizeof(params));
    if (rc != OK) {
        log::error("nvidia: allocating channel %u failed: %d", setup.id, rc);
        return rc;
    }

    m_channel = object;

    return OK;
}

int32_t channel::release_objects() {
    int32_t first_rc = OK;
    for (uint32_t i = m_object_count; i > 0; i--) {
        first_rc = release_object(m_objects_held[i - 1], first_rc);
    }

    m_object_count = 0;
    first_rc = release_object(m_channel, first_rc);
    first_rc = release_object(m_group, first_rc);

    return first_rc;
}

int32_t channel::release_object(rm::handle& object, int32_t first_rc) {
    if (object == 0) {
        return first_rc;
    }

    int32_t rc = m_firmware.release(object);
    if (rc != OK) {
        log::error("nvidia: releasing channel object 0x%X failed: %d, so the channel's memory stays", object, rc);
        m_keeps_memory = true;
    }

    object = 0;

    return first_rc != OK ? first_rc : rc;
}

int32_t channel::release_memory() {
    int32_t rc = OK;
    if (m_gpfifo_tables != nullptr && !m_keeps_memory) {
        rc = m_gpfifo_tables->unmap(m_gpfifo_address, GPFIFO_BYTES);
        if (rc != OK) {
            log::error("nvidia: unmapping a channel's GPFIFO failed: %d, so the channel's memory stays allocated", rc);
            m_keeps_memory = true;
        }
    }

    if (!m_keeps_memory) {
        if (m_instance_block.size != 0) {
            m_video_memory.release(m_instance_block);
        }

        RUN_ELEVATED({
            dma::free_pages(m_userd);
            dma::free_pages(m_gpfifo);
            for (dma::buffer& buffer : m_method_buffers) {
                dma::free_pages(buffer);
            }
        });
    }

    m_instance_block = {};
    m_userd = {};
    m_gpfifo = {};
    for (dma::buffer& buffer : m_method_buffers) {
        buffer = {};
    }

    m_gpfifo_tables = nullptr;
    m_gpfifo_address = 0;
    m_method_buffer_bytes = 0;

    return rc;
}

} // namespace drivers::nvidia

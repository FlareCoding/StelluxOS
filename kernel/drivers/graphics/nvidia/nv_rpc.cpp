#include "drivers/graphics/nvidia/nv_rpc.h"
#include "drivers/graphics/nvidia/nv_gpu.h"
#include "hw/delay.h"
#include "hw/barrier.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"
#include "common/logging.h"
#include "common/string.h"

namespace nv {

// ============================================================================
// RPC Send — Write message to command queue
// ============================================================================

int32_t rpc_send(nv_gpu* gpu, gsp_boot_state& state,
                 uint32_t function, const void* payload, uint32_t payload_size) {
    // Calculate total message size (transport header + RPC header + payload)
    uint32_t rpc_len = sizeof(rpc_header) + payload_size;
    uint32_t total_len = sizeof(gsp_msg_element) + rpc_len;
    uint32_t padded_len = (total_len + GSP_PAGE_SIZE_VAL - 1) & ~(GSP_PAGE_SIZE_VAL - 1);
    uint32_t elem_count = padded_len / GSP_PAGE_SIZE_VAL;

    if (elem_count > 16) {
        log::error("nvidia: rpc: message too large (%u bytes, %u pages)", total_len, elem_count);
        return ERR_INVALID;
    }

    // Get command queue pointers
    uint8_t* cmdq_base = reinterpret_cast<uint8_t*>(state.shm_dma.virt + state.cmdq_offset);
    volatile queue_header* cmdq_hdr = reinterpret_cast<volatile queue_header*>(cmdq_base);
    uint32_t entry_off = cmdq_hdr->tx.entry_off;
    uint32_t msg_count = cmdq_hdr->tx.msg_count;

    // Check for space (read pointer is in the msgq's rx header)
    volatile queue_header* msgq_hdr = reinterpret_cast<volatile queue_header*>(
        state.shm_dma.virt + state.msgq_offset);
    uint32_t wptr = cmdq_hdr->tx.write_ptr;
    uint32_t rptr = msgq_hdr->rx.read_ptr;

    uint32_t used = (wptr >= rptr) ? (wptr - rptr) : (msg_count - rptr + wptr);
    uint32_t free_slots = msg_count - used - 1;

    if (free_slots < elem_count) {
        log::error("nvidia: rpc: command queue full (free=%u need=%u)", free_slots, elem_count);
        return ERR_IO;
    }

    // Build the message at the write pointer position
    uint32_t slot = wptr % msg_count;
    uint8_t* entry = cmdq_base + entry_off + slot * QUEUE_ENTRY_SIZE;

    // Zero the entire padded area
    string::memset(entry, 0, padded_len);

    // Fill transport header
    gsp_msg_element* msg = reinterpret_cast<gsp_msg_element*>(entry);
    msg->elem_count = elem_count;
    msg->seq_num = state.cmdq_seq++;
    msg->pad = 0;

    // Fill RPC header
    rpc_header* rpc = reinterpret_cast<rpc_header*>(entry + sizeof(gsp_msg_element));
    rpc->header_version = RPC_HDR_VERSION;
    rpc->signature = 0x56525043; // 'V'<<24 | 'R'<<16 | 'P'<<8 | 'C'
    rpc->length = rpc_len;
    rpc->function = function;
    rpc->rpc_result = 0xFFFFFFFF;
    rpc->rpc_result_private = 0xFFFFFFFF;
    rpc->sequence = state.rpc_seq++;
    rpc->spare = 0;

    // Copy payload
    if (payload && payload_size > 0) {
        string::memcpy(entry + sizeof(gsp_msg_element) + sizeof(rpc_header),
                       payload, payload_size);
    }

    // Compute XOR checksum over entire padded message
    msg->checksum = 0;
    uint64_t csum = 0;
    for (uint32_t i = 0; i < padded_len / sizeof(uint64_t); i++) {
        csum ^= reinterpret_cast<volatile uint64_t*>(entry)[i];
    }
    msg->checksum = static_cast<uint32_t>((csum >> 32) ^ (csum & 0xFFFFFFFF));

    // Update write pointer (with memory barriers)
    barrier::smp_write(); // ensure all writes are visible before updating wptr
    cmdq_hdr->tx.write_ptr = (wptr + elem_count) % msg_count;
    barrier::smp_full(); // full barrier before doorbell

    // Ring doorbell — write to falcon MAILBOX0 at GSP base + 0xC00
    gpu->reg_wr32(reg::FALCON_GSP_BASE + FALCON_DOORBELL, 0x00000000);

    log::debug("nvidia: rpc: sent fn=%u seq=%u payload=%u bytes slot=%u",
               function, rpc->sequence, payload_size, slot);

    return OK;
}

// ============================================================================
// RPC Receive — Read message from status queue
// ============================================================================

int32_t rpc_recv(nv_gpu* gpu, gsp_boot_state& state,
                 uint32_t function, void* out_payload, uint32_t max_payload,
                 uint32_t* out_size, uint32_t timeout_ms) {
    (void)gpu;

    uint8_t* msgq_base = reinterpret_cast<uint8_t*>(state.shm_dma.virt + state.msgq_offset);
    volatile queue_header* msgq_hdr = reinterpret_cast<volatile queue_header*>(msgq_base);

    // msgq read pointer is in cmdq's rx header
    volatile uint32_t* msgq_rptr = &reinterpret_cast<volatile queue_header*>(
        state.shm_dma.virt + state.cmdq_offset)->rx.read_ptr;

    uint32_t entry_off = msgq_hdr->tx.entry_off;
    uint32_t msg_count = msgq_hdr->tx.msg_count;

    uint64_t deadline = clock::now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ULL;

    while (clock::now_ns() < deadline) {
        uint32_t wptr = msgq_hdr->tx.write_ptr;
        uint32_t rptr = *msgq_rptr;

        if (wptr == rptr) {
            delay::us(100);
            continue;
        }

        // Read message at rptr
        uint32_t slot = rptr % msg_count;
        uint8_t* entry = msgq_base + entry_off + slot * QUEUE_ENTRY_SIZE;

        gsp_msg_element* msg = reinterpret_cast<gsp_msg_element*>(entry);
        rpc_header* rpc = reinterpret_cast<rpc_header*>(entry + sizeof(gsp_msg_element));

        uint32_t payload_size = 0;
        if (rpc->length > sizeof(rpc_header)) {
            payload_size = rpc->length - sizeof(rpc_header);
        }

        log::debug("nvidia: rpc: recv fn=0x%x result=0x%x seq=%u payload=%u",
                   rpc->function, rpc->rpc_result, rpc->sequence, payload_size);

        // Advance read pointer
        uint32_t advance = msg->elem_count;
        if (advance == 0) advance = 1;
        *msgq_rptr = (rptr + advance) % msg_count;
        barrier::smp_full();

        // Check if this is the message we're waiting for
        if (rpc->function == function) {
            if (out_payload && payload_size > 0 && max_payload > 0) {
                uint32_t copy = (payload_size < max_payload) ? payload_size : max_payload;
                string::memcpy(out_payload,
                               entry + sizeof(gsp_msg_element) + sizeof(rpc_header),
                               copy);
                if (out_size) *out_size = copy;
            } else {
                if (out_size) *out_size = 0;
            }

            if (rpc->rpc_result != 0 && rpc->rpc_result != 0xFFFFFFFF) {
                log::warn("nvidia: rpc: fn=%u returned error 0x%x", function, rpc->rpc_result);
            }

            return OK;
        }

        // Handle events we're not waiting for
        if (rpc->function == NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQ) {
            log::info("nvidia: rpc: received CPU sequencer event (skipping)");
        } else if (rpc->function == NV_VGPU_MSG_EVENT_OS_ERROR_LOG) {
            log::warn("nvidia: rpc: received OS error log from GSP");
        } else {
            log::info("nvidia: rpc: ignoring unexpected message fn=0x%x", rpc->function);
        }
    }

    log::error("nvidia: rpc: timeout waiting for fn=%u (%u ms)", function, timeout_ms);
    return ERR_TIMEOUT;
}

// ============================================================================
// RPC Call — Send + Receive
// ============================================================================

int32_t rpc_call(nv_gpu* gpu, gsp_boot_state& state,
                 uint32_t function, const void* payload, uint32_t payload_size,
                 void* out_payload, uint32_t max_out, uint32_t* out_size,
                 uint32_t timeout_ms) {
    int32_t rc = rpc_send(gpu, state, function, payload, payload_size);
    if (rc != OK) return rc;

    return rpc_recv(gpu, state, function, out_payload, max_out, out_size, timeout_ms);
}

// ============================================================================
// RM Alloc
// ============================================================================

int32_t rm_alloc(nv_gpu* gpu, gsp_boot_state& state,
                 uint32_t h_client, uint32_t h_parent, uint32_t h_object,
                 uint32_t h_class, const void* params, uint32_t params_size) {
    // Build the rm_alloc payload: header + class-specific params
    uint32_t total = sizeof(rm_alloc_params) + params_size;
    uint8_t buf[4096]; // Stack buffer (max 4KB payload)
    if (total > sizeof(buf)) {
        log::error("nvidia: rm_alloc: payload too large (%u)", total);
        return ERR_INVALID;
    }

    string::memset(buf, 0, total);
    rm_alloc_params* alloc = reinterpret_cast<rm_alloc_params*>(buf);
    alloc->h_client = h_client;
    alloc->h_parent = h_parent;
    alloc->h_object = h_object;
    alloc->h_class = h_class;
    alloc->status = 0;
    alloc->params_size = params_size;
    alloc->flags = 0;

    if (params && params_size > 0) {
        string::memcpy(buf + sizeof(rm_alloc_params), params, params_size);
    }

    log::info("nvidia: rm_alloc: client=0x%x parent=0x%x object=0x%x class=0x%04x params=%u",
              h_client, h_parent, h_object, h_class, params_size);

    // Send and receive
    uint8_t reply[4096];
    uint32_t reply_size = 0;
    int32_t rc = rpc_call(gpu, state, NV_VGPU_MSG_FUNCTION_RM_ALLOC,
                          buf, total, reply, sizeof(reply), &reply_size, 4000);
    if (rc != OK) {
        log::error("nvidia: rm_alloc: RPC failed: %d", rc);
        return rc;
    }

    // Check result
    if (reply_size >= sizeof(rm_alloc_params)) {
        rm_alloc_params* result = reinterpret_cast<rm_alloc_params*>(reply);
        if (result->status != 0) {
            log::error("nvidia: rm_alloc: GSP returned status 0x%x for class 0x%04x",
                       result->status, h_class);
            return ERR_IO;
        }
    }

    log::info("nvidia: rm_alloc: success — handle 0x%x (class 0x%04x)", h_object, h_class);
    return OK;
}

// ============================================================================
// RM Control
// ============================================================================

int32_t rm_control(nv_gpu* gpu, gsp_boot_state& state,
                   uint32_t h_client, uint32_t h_object,
                   uint32_t cmd, void* params, uint32_t params_size) {
    uint32_t total = sizeof(rm_control_params) + params_size;
    uint8_t buf[8192]; // Stack buffer
    if (total > sizeof(buf)) {
        log::error("nvidia: rm_control: payload too large (%u)", total);
        return ERR_INVALID;
    }

    string::memset(buf, 0, total);
    rm_control_params* ctrl = reinterpret_cast<rm_control_params*>(buf);
    ctrl->h_client = h_client;
    ctrl->h_object = h_object;
    ctrl->cmd = cmd;
    ctrl->status = 0;
    ctrl->params_size = params_size;
    ctrl->flags = 0;

    if (params && params_size > 0) {
        string::memcpy(buf + sizeof(rm_control_params), params, params_size);
    }

    log::debug("nvidia: rm_control: client=0x%x obj=0x%x cmd=0x%08x params=%u",
               h_client, h_object, cmd, params_size);

    uint8_t reply[8192];
    uint32_t reply_size = 0;
    int32_t rc = rpc_call(gpu, state, NV_VGPU_MSG_FUNCTION_RM_CONTROL,
                          buf, total, reply, sizeof(reply), &reply_size, 4000);
    if (rc != OK) return rc;

    // Copy output params back to caller
    if (reply_size >= sizeof(rm_control_params) + params_size && params && params_size > 0) {
        string::memcpy(params, reply + sizeof(rm_control_params), params_size);
    }

    // Check status
    if (reply_size >= sizeof(rm_control_params)) {
        rm_control_params* result = reinterpret_cast<rm_control_params*>(reply);
        if (result->status != 0) {
            log::warn("nvidia: rm_control: cmd 0x%08x returned status 0x%x", cmd, result->status);
            return ERR_IO;
        }
    }

    return OK;
}

// ============================================================================
// RM Free
// ============================================================================

int32_t rm_free(nv_gpu* gpu, gsp_boot_state& state,
                uint32_t h_client, uint32_t h_object) {
    rm_free_params params;
    params.h_client = h_client;
    params.h_object = h_object;
    params.status = 0;

    return rpc_send(gpu, state, NV_VGPU_MSG_FUNCTION_RM_FREE, &params, sizeof(params));
}

// ============================================================================
// SET_SYSTEM_INFO RPC
// ============================================================================

// Minimal GspSystemInfo — only critical fields populated
struct __attribute__((packed)) gsp_system_info_minimal {
    uint64_t gpu_phys_addr;              // 0x00: BAR0 physical
    uint64_t gpu_phys_fb_addr;           // 0x08: BAR1 physical
    uint64_t gpu_phys_inst_addr;         // 0x10: BAR2/3 physical
    uint64_t nv_domain_bus_device_func;  // 0x18: PCI BDF encoding
    uint64_t sim_access_buf_phys;        // 0x20: 0
    uint64_t pcie_atomics_op_mask;       // 0x28: 0
    uint64_t console_mem_size;           // 0x30: 0
    uint64_t max_user_va;                // 0x38: 0x800000000000 for x86_64
    uint32_t pci_config_mirror_base;     // 0x40: 0x088000
    uint32_t pci_config_mirror_size;     // 0x44: 0x001000
    // Remaining fields zeroed (ACPI, chipset info, etc.)
    uint8_t  remaining[512];             // Zero-filled for remaining fields
};

int32_t rpc_set_system_info(nv_gpu* gpu, gsp_boot_state& state) {
    log::info("nvidia: rpc: sending SET_SYSTEM_INFO");

    gsp_system_info_minimal info;
    string::memset(&info, 0, sizeof(info));

    // Fill critical fields from PCI device
    const pci::bar& bar0 = gpu->dev().get_bar(0);
    const pci::bar& bar1 = gpu->dev().get_bar(1);
    const pci::bar& bar3 = gpu->dev().get_bar(3);

    info.gpu_phys_addr = bar0.phys;
    info.gpu_phys_fb_addr = bar1.phys;
    info.gpu_phys_inst_addr = bar3.phys;

    // PCI BDF encoding: (bus << 8) | (slot << 3) | func
    info.nv_domain_bus_device_func =
        (static_cast<uint64_t>(gpu->dev().bus()) << 8) |
        (static_cast<uint64_t>(gpu->dev().slot()) << 3) |
        gpu->dev().func();

    info.max_user_va = 0x800000000000ULL; // x86_64 TASK_SIZE equivalent
    info.pci_config_mirror_base = 0x088000;
    info.pci_config_mirror_size = 0x001000;

    log::info("nvidia: rpc: sysinfo: bar0=0x%lx bar1=0x%lx bar3=0x%lx bdf=0x%lx",
              info.gpu_phys_addr, info.gpu_phys_fb_addr,
              info.gpu_phys_inst_addr, info.nv_domain_bus_device_func);

    return rpc_send(gpu, state, NV_VGPU_MSG_FUNCTION_SET_SYSTEM_INFO,
                    &info, sizeof(info));
}

// ============================================================================
// SET_REGISTRY RPC
// ============================================================================

int32_t rpc_set_registry(nv_gpu* gpu, gsp_boot_state& state) {
    log::info("nvidia: rpc: sending SET_REGISTRY (minimal)");

    // Minimal registry: header (8 bytes) + 1 dummy entry (13 bytes) + 1 NUL byte
    uint8_t buf[32];
    string::memset(buf, 0, sizeof(buf));

    // PACKED_REGISTRY_TABLE header
    uint32_t* header = reinterpret_cast<uint32_t*>(buf);
    header[0] = 8;  // size = sizeof(header) = 8
    header[1] = 1;  // numEntries = 1

    // PACKED_REGISTRY_ENTRY (13 bytes, packed)
    uint8_t* entry = buf + 8;
    // nameOffset = offset from start of PACKED_REGISTRY_TABLE to string
    // = 8 (header) + 13 (entry) = 21
    uint32_t name_off = 21;
    string::memcpy(entry + 0, &name_off, 4);  // nameOffset
    entry[4] = 1;                               // type = DWORD
    uint32_t zero = 0;
    string::memcpy(entry + 5, &zero, 4);       // data = 0
    uint32_t four = 4;
    string::memcpy(entry + 9, &four, 4);       // length = 4

    // String data: empty string (just NUL)
    buf[21] = '\0';

    return rpc_send(gpu, state, NV_VGPU_MSG_FUNCTION_SET_REGISTRY, buf, 22);
}

// ============================================================================
// GET_GSP_STATIC_INFO RPC
// ============================================================================

int32_t rpc_get_gsp_static_info(nv_gpu* gpu, gsp_boot_state& state) {
    log::info("nvidia: rpc: requesting GET_GSP_STATIC_INFO");

    // Send empty request
    uint8_t reply[8192]; // GspStaticConfigInfo is large
    uint32_t reply_size = 0;

    int32_t rc = rpc_call(gpu, state, NV_VGPU_MSG_FUNCTION_GET_STATIC_INFO,
                          nullptr, 0, reply, sizeof(reply), &reply_size, 4000);
    if (rc != OK) {
        log::error("nvidia: rpc: GET_GSP_STATIC_INFO failed: %d", rc);
        return rc;
    }

    log::info("nvidia: rpc: GET_GSP_STATIC_INFO received (%u bytes)", reply_size);

    // Extract a few key fields for logging (offsets depend on exact struct version)
    // For now, just log that we received it successfully
    if (reply_size >= 8) {
        // The first u64 should contain the FB length
        uint64_t fb_length = *reinterpret_cast<uint64_t*>(reply);
        log::info("nvidia: rpc: static info: fb_length=0x%lx", fb_length);
    }

    return OK;
}

// ============================================================================
// RM Init — Create object hierarchy
// ============================================================================

int32_t rm_init(nv_gpu* gpu, gsp_boot_state& state, rm_state& rm) {
    log::info("nvidia: ========================================");
    log::info("nvidia: Phase D: RM object initialization");
    log::info("nvidia: ========================================");

    string::memset(&rm, 0, sizeof(rm));
    rm.next_handle = 0xCAFE0001; // Start handle allocator
    rm.initialized = false;

    int32_t rc;

    // Step 1: Get GSP static info
    rc = rpc_get_gsp_static_info(gpu, state);
    if (rc != OK) {
        log::error("nvidia: rm: GET_GSP_STATIC_INFO failed: %d", rc);
        return rc;
    }

    // Step 2: Allocate root client (NV01_ROOT = 0x0000)
    rm.h_client = rm.next_handle++;
    // NV0000_ALLOC_PARAMETERS: 108 bytes, mostly zeros for minimal init
    uint8_t root_params[108];
    string::memset(root_params, 0, sizeof(root_params));

    rc = rm_alloc(gpu, state, rm.h_client, rm.h_client, rm.h_client,
                  NV01_ROOT, root_params, sizeof(root_params));
    if (rc != OK) {
        log::error("nvidia: rm: root client alloc failed: %d", rc);
        return rc;
    }
    log::info("nvidia: rm: root client allocated: handle=0x%x", rm.h_client);

    // Step 3: Allocate device (NV01_DEVICE_0 = 0x0080)
    rm.h_device = rm.next_handle++;
    // NV0080_ALLOC_PARAMETERS: 48 bytes
    uint8_t dev_params[48];
    string::memset(dev_params, 0, sizeof(dev_params));
    // deviceId at offset 0 = 0 (device 0)
    // hClientShare at offset 4 = h_client
    uint32_t client_share = rm.h_client;
    string::memcpy(dev_params + 4, &client_share, 4);

    rc = rm_alloc(gpu, state, rm.h_client, rm.h_client, rm.h_device,
                  NV01_DEVICE_0, dev_params, sizeof(dev_params));
    if (rc != OK) {
        log::error("nvidia: rm: device alloc failed: %d", rc);
        return rc;
    }
    log::info("nvidia: rm: device allocated: handle=0x%x", rm.h_device);

    // Step 4: Allocate subdevice (NV20_SUBDEVICE_0 = 0x2080)
    rm.h_subdevice = rm.next_handle++;
    uint32_t subdev_id = 0; // subDeviceId = 0
    rc = rm_alloc(gpu, state, rm.h_client, rm.h_device, rm.h_subdevice,
                  NV20_SUBDEVICE_0, &subdev_id, sizeof(subdev_id));
    if (rc != OK) {
        log::error("nvidia: rm: subdevice alloc failed: %d", rc);
        return rc;
    }
    log::info("nvidia: rm: subdevice allocated: handle=0x%x", rm.h_subdevice);

    // Step 5: Allocate display common (NV04_DISPLAY_COMMON = 0x0073)
    rm.h_display = rm.next_handle++;
    rc = rm_alloc(gpu, state, rm.h_client, rm.h_device, rm.h_display,
                  NV04_DISPLAY_COMMON, nullptr, 0);
    if (rc != OK) {
        log::error("nvidia: rm: display alloc failed: %d", rc);
        return rc;
    }
    log::info("nvidia: rm: display allocated: handle=0x%x", rm.h_display);

    rm.initialized = true;

    log::info("nvidia: ========================================");
    log::info("nvidia: RM object hierarchy created:");
    log::info("nvidia:   client=0x%x → device=0x%x → subdevice=0x%x",
              rm.h_client, rm.h_device, rm.h_subdevice);
    log::info("nvidia:   display=0x%x", rm.h_display);
    log::info("nvidia: ========================================");

    return OK;
}

} // namespace nv

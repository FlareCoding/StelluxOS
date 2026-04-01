#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_RPC_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_RPC_H

#include "common/types.h"
#include "drivers/graphics/nvidia/nv_types.h"
#include "drivers/graphics/nvidia/nv_gsp_boot.h"

namespace nv {

class nv_gpu;

// ============================================================================
// RPC Send/Receive API
// ============================================================================

/**
 * Send an RPC to GSP via the command queue.
 * Builds the full message: gsp_msg_element + rpc_header + payload.
 * Computes XOR checksum, updates write pointer, rings doorbell.
 *
 * @param gpu       GPU device
 * @param state     GSP boot state (contains queue pointers)
 * @param function  RPC function number (NV_VGPU_MSG_FUNCTION_*)
 * @param payload   Payload data (copied after rpc_header)
 * @param payload_size  Payload size in bytes
 * @return OK on success
 */
int32_t rpc_send(nv_gpu* gpu, gsp_boot_state& state,
                 uint32_t function, const void* payload, uint32_t payload_size);

/**
 * Receive an RPC reply from GSP via the status queue.
 * Polls the queue with a timeout, looking for a message with the specified
 * function number. Handles interleaved events (sequencer, error log).
 *
 * @param gpu         GPU device
 * @param state       GSP boot state
 * @param function    Expected function number to wait for
 * @param out_payload Output buffer for payload (caller provides)
 * @param max_payload Maximum payload buffer size
 * @param out_size    Output: actual payload size received
 * @param timeout_ms  Timeout in milliseconds
 * @return OK on success, ERR_TIMEOUT on timeout
 */
int32_t rpc_recv(nv_gpu* gpu, gsp_boot_state& state,
                 uint32_t function, void* out_payload, uint32_t max_payload,
                 uint32_t* out_size, uint32_t timeout_ms);

/**
 * Send an RPC and wait for the reply.
 * Combines rpc_send + rpc_recv.
 */
int32_t rpc_call(nv_gpu* gpu, gsp_boot_state& state,
                 uint32_t function, const void* payload, uint32_t payload_size,
                 void* out_payload, uint32_t max_out, uint32_t* out_size,
                 uint32_t timeout_ms);

// ============================================================================
// RM Object Management
// ============================================================================

// RM Alloc RPC payload (function 103)
struct __attribute__((packed)) rm_alloc_params {
    uint32_t h_client;     // Client handle
    uint32_t h_parent;     // Parent object handle
    uint32_t h_object;     // New object handle (allocated by caller)
    uint32_t h_class;      // Object class (NV01_ROOT, etc.)
    uint32_t status;       // Result status (filled by GSP)
    uint32_t params_size;  // Size of class-specific params
    uint32_t flags;        // Allocation flags
    // Followed by params_size bytes of class-specific parameters
};
static_assert(sizeof(rm_alloc_params) == 28);

// RM Control RPC payload (function 76)
struct __attribute__((packed)) rm_control_params {
    uint32_t h_client;     // Client handle
    uint32_t h_object;     // Object handle to control
    uint32_t cmd;          // Control command
    uint32_t status;       // Result status (filled by GSP)
    uint32_t params_size;  // Size of command-specific params
    uint32_t flags;        // Control flags
    // Followed by params_size bytes of command-specific parameters
};
static_assert(sizeof(rm_control_params) == 24);

// RM Free RPC payload (function 104)
struct __attribute__((packed)) rm_free_params {
    uint32_t h_client;
    uint32_t h_object;
    uint32_t status;
};
static_assert(sizeof(rm_free_params) == 12);

// RM class numbers
constexpr uint32_t NV01_ROOT          = 0x0000;
constexpr uint32_t NV01_DEVICE_0      = 0x0080;
constexpr uint32_t NV20_SUBDEVICE_0   = 0x2080;
constexpr uint32_t NV04_DISPLAY_COMMON = 0x0073;

// RM function numbers
constexpr uint32_t NV_VGPU_MSG_FUNCTION_RM_CONTROL = 76;
constexpr uint32_t NV_VGPU_MSG_FUNCTION_RM_FREE    = 104;

/**
 * Allocate an RM object via GSP.
 *
 * @param gpu       GPU device
 * @param state     GSP boot state
 * @param h_client  Client handle (or 0 for root allocation)
 * @param h_parent  Parent object handle
 * @param h_object  New object handle
 * @param h_class   Object class
 * @param params    Class-specific parameters (may be nullptr)
 * @param params_size Size of params
 * @return OK on success
 */
int32_t rm_alloc(nv_gpu* gpu, gsp_boot_state& state,
                 uint32_t h_client, uint32_t h_parent, uint32_t h_object,
                 uint32_t h_class, const void* params, uint32_t params_size);

/**
 * Execute an RM control command via GSP.
 *
 * @param gpu         GPU device
 * @param state       GSP boot state
 * @param h_client    Client handle
 * @param h_object    Target object handle
 * @param cmd         Control command
 * @param params      Command-specific parameters (in/out)
 * @param params_size Size of params
 * @return OK on success
 */
int32_t rm_control(nv_gpu* gpu, gsp_boot_state& state,
                   uint32_t h_client, uint32_t h_object,
                   uint32_t cmd, void* params, uint32_t params_size);

/**
 * Free an RM object via GSP.
 */
int32_t rm_free(nv_gpu* gpu, gsp_boot_state& state,
                uint32_t h_client, uint32_t h_object);

// ============================================================================
// High-Level Init RPCs
// ============================================================================

/**
 * Send SET_SYSTEM_INFO RPC with GPU's PCI topology information.
 */
int32_t rpc_set_system_info(nv_gpu* gpu, gsp_boot_state& state);

/**
 * Send SET_REGISTRY RPC with minimal (empty) registry.
 */
int32_t rpc_set_registry(nv_gpu* gpu, gsp_boot_state& state);

/**
 * Send GET_GSP_STATIC_INFO and receive GPU configuration.
 * Returns internal client/device/subdevice handles.
 */
int32_t rpc_get_gsp_static_info(nv_gpu* gpu, gsp_boot_state& state);

// ============================================================================
// RM Object State (handles allocated during init)
// ============================================================================

struct rm_state {
    uint32_t h_client;      // Root client handle
    uint32_t h_device;      // Device handle
    uint32_t h_subdevice;   // Subdevice handle
    uint32_t h_display;     // Display common handle

    // Internal handles (from GET_GSP_STATIC_INFO)
    uint32_t h_internal_client;
    uint32_t h_internal_device;
    uint32_t h_internal_subdevice;

    uint32_t next_handle;   // Handle allocator

    bool initialized;
};

/**
 * Initialize the RM object hierarchy:
 * 1. Allocate root client (NV01_ROOT)
 * 2. Allocate device (NV01_DEVICE_0)
 * 3. Allocate subdevice (NV20_SUBDEVICE_0)
 * 4. Allocate display (NV04_DISPLAY_COMMON)
 */
int32_t rm_init(nv_gpu* gpu, gsp_boot_state& state, rm_state& rm);

} // namespace nv

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_RPC_H

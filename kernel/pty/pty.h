#ifndef STELLUX_PTY_PTY_H
#define STELLUX_PTY_PTY_H

#include "common/types.h"
#include "rc/ref_counted.h"
#include "rc/strong_ref.h"
#include "terminal/line_discipline.h"

struct ring_buffer;
namespace resource { struct resource_object; }

namespace pty {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;
constexpr size_t PTY_RING_CAPACITY = 4096;

/** Output processing flags (applied in the slave→master direction). */
constexpr uint32_t PTY_OFLAG_ONLCR = (1u << 0);   // map NL → CR+NL

struct pty_channel : rc::ref_counted<pty_channel> {
    ring_buffer* m_input_rb;              // master write -> ld -> slave read
    ring_buffer* m_output_rb;             // slave write -> master read
    terminal::line_discipline m_ld;
    terminal::echo_target m_echo;
    uint32_t m_id;
    uint32_t m_oflags;                    // output processing flags

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE static void ref_destroy(pty_channel* self);
};

struct pty_endpoint {
    rc::strong_ref<pty_channel> channel;
    bool is_master;
};

/**
 * @brief Create a connected PTY master/slave pair.
 * Returns two resource_objects, each with refcount 1.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_pair(
    resource::resource_object** out_master,
    resource::resource_object** out_slave
);

} // namespace pty

#endif // STELLUX_PTY_PTY_H

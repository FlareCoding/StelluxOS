#ifndef STELLUX_PIPE_PIPE_H
#define STELLUX_PIPE_PIPE_H

#include "common/types.h"
#include "rc/ref_counted.h"
#include "rc/strong_ref.h"

struct ring_buffer;
namespace resource { struct resource_object; }

namespace pipe {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;
constexpr size_t PIPE_RING_CAPACITY = 8192;

struct pipe_channel : rc::ref_counted<pipe_channel> {
    ring_buffer* rb;

    /*
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(pipe_channel* self);
};

struct pipe_endpoint {
    rc::strong_ref<pipe_channel> channel;
    bool is_read_end;
};

/**
 * Create a connected pipe read/write pair.
 * Returns two resource_objects, each with refcount 1.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_pair(
    resource::resource_object** out_read,
    resource::resource_object** out_write
);

} // namespace pipe

#endif // STELLUX_PIPE_PIPE_H

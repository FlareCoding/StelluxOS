#include "mm/device_backing.h"

namespace mm {

device_backing::device_backing() {
    m_reaper_node.init(reap_backing_thunk);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void device_backing::ref_destroy(device_backing* self) {
    rc::reaper::defer(&self->m_reaper_node);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE rc::reaper::cleanup_result device_backing::reap_backing(device_backing* self) {
    self->release_memory();
    return rc::reaper::DONE;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE rc::reaper::cleanup_result device_backing::reap_backing_thunk(rc::reaper::dead_node* node) {
    return rc::reaper::reaper_thunk<device_backing, reap_backing, &device_backing::m_reaper_node>(node);
}

} // namespace mm

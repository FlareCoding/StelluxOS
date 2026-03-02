#include "net/net.h"
#include "net/unix_stream.h"

namespace net {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    int32_t rc = unix_stream::init();
    if (rc != unix_stream::OK) {
        return ERR_INIT;
    }
    return OK;
}

} // namespace net

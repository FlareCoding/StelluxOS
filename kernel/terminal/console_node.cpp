#include "terminal/console_node.h"
#include "terminal/terminal.h"
#include "common/ring_buffer.h"
#include "serial/serial.h"
#include "fs/file.h"
#include "fs/fs.h"

namespace terminal {

console_node::console_node(fs::instance* fs, const char* name)
    : fs::node(fs::node_type::char_device, fs, name) {
}

ssize_t console_node::read(fs::file*, void* buf, size_t count, uint32_t flags) {
    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    ssize_t rc = ring_buffer_read(console_input_rb(),
                                  static_cast<uint8_t*>(buf), count, nonblock);
    if (rc == RB_ERR_AGAIN) {
        return fs::ERR_AGAIN;
    }

    if (rc < 0) {
        return fs::ERR_IO;
    }

    return rc;
}

ssize_t console_node::write(fs::file*, const void* buf, size_t count, uint32_t) {
    serial::write(static_cast<const char*>(buf), count);
    return static_cast<ssize_t>(count);
}

int32_t console_node::ioctl(fs::file*, uint32_t cmd, uint64_t arg) {
    return terminal::console_ioctl(cmd, arg);
}

} // namespace terminal

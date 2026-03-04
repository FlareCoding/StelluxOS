#include "terminal/console_node.h"
#include "terminal/terminal.h"
#include "common/ring_buffer.h"
#include "io/serial.h"
#include "fs/fs.h"
#include "fs/file.h"

namespace terminal {

console_node::console_node(fs::instance* fs, const char* name)
    : fs::node(fs::node_type::char_device, fs, name) {
}

ssize_t console_node::read(fs::file* f, void* buf, size_t count) {
    bool nonblock = (f->flags() & fs::O_NONBLOCK) != 0;
    return ring_buffer_read(console_input_rb(),
                            static_cast<uint8_t*>(buf), count, nonblock);
}

ssize_t console_node::write(fs::file*, const void* buf, size_t count) {
    serial::write(static_cast<const char*>(buf), count);
    return static_cast<ssize_t>(count);
}

int32_t console_node::getattr(fs::vattr* attr) {
    if (!attr) return fs::ERR_INVAL;
    attr->type = fs::node_type::char_device;
    attr->size = 0;
    return fs::OK;
}

} // namespace terminal

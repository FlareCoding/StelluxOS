#ifndef STELLUX_TERMINAL_CONSOLE_NODE_H
#define STELLUX_TERMINAL_CONSOLE_NODE_H

#include "fs/node.h"

namespace terminal {

class console_node : public fs::node {
public:
    console_node(fs::instance* fs, const char* name);

    ssize_t read(fs::file* f, void* buf, size_t count) override;
    ssize_t write(fs::file* f, const void* buf, size_t count) override;
    int32_t getattr(fs::vattr* attr) override;
};

} // namespace terminal

#endif // STELLUX_TERMINAL_CONSOLE_NODE_H

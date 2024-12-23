#ifndef MODULE_BASE_H
#define MODULE_BASE_H
#include <types.h>

namespace modules {
enum class module_state {
    unloaded,
    loaded,
    initialized,
    running,
    stopped,
    error
};

class module_base {
public:
    explicit module_base(const char* name)
        : m_name(name), m_state(module_state::unloaded) {}

    virtual ~module_base() = default;

    // ------------------------------------------------------------------------
    // Lifecycle Hooks
    // ------------------------------------------------------------------------

    virtual bool init() = 0;
    virtual bool start() = 0;
    virtual bool stop() = 0;

    // ------------------------------------------------------------------------
    // Command Interface
    // ------------------------------------------------------------------------

    virtual bool on_command(
        uint64_t command,
        const void* data_in,
        size_t data_in_size,
        void* data_out,
        size_t data_out_size
    ) = 0;

    // ------------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------------

    __force_inline__ const char* name() const {
        return m_name;
    }

    __force_inline__ module_state state() const {
        return m_state;
    }

private:
    const char*   m_name;
    module_state  m_state;

    friend class module_manager;
};
} // namespace modules

#endif // MODULE_BASE_H

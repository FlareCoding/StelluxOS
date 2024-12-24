#ifndef MODULE_BASE_H
#define MODULE_BASE_H
#include <core/string.h>

namespace modules {
enum class module_state {
    unloaded,
    loaded,
    running,
    stopped,
    error
};

class module_base {
public:
    module_base(const kstl::string& name)
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

    inline const kstl::string& name() const {
        return m_name;
    }

    inline module_state state() const {
        return m_state;
    }

private:
    kstl::string  m_name;
    module_state  m_state;

    friend class module_manager;
};
} // namespace modules

#endif // MODULE_BASE_H

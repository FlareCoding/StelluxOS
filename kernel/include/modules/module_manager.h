#ifndef MODULE_MANAGER_H
#define MODULE_MANAGER_H
#include "module_base.h"
#include <memory/memory.h>
#include <kstl/vector.h>

namespace modules {
class module_manager {
public:
    module_manager() = default;
    ~module_manager() = default;

    // Disable copy/move semantics
    module_manager(const module_manager&) = delete;
    module_manager& operator=(const module_manager&) = delete;

    // ------------------------------------------------------------------------
    // Registration
    // ------------------------------------------------------------------------

    bool register_module(const kstl::shared_ptr<module_base>& mod);
    bool unregister_module(module_base* mod);

    // ------------------------------------------------------------------------
    // Lifecycle Control
    // ------------------------------------------------------------------------

    bool init_module(const char* name);
    bool start_module(const char* name);
    bool stop_module(const char* name);

    // ------------------------------------------------------------------------
    // Command Interface
    // ------------------------------------------------------------------------

    bool send_command(const char*   name,
                      uint64_t      command,
                      const void*   data_in,
                      size_t        data_in_size,
                      void*         data_out,
                      size_t        data_out_size);

    // ------------------------------------------------------------------------
    // Debug / Inspection
    // ------------------------------------------------------------------------

    module_base* find_module(const char* name);

private:
    kstl::vector<kstl::shared_ptr<module_base>> m_modules;

    static void _module_start_task_entry(module_base* mod);
};
} // namespace modules

#endif // MODULE_MANAGER_H

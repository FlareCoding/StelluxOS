#ifndef MODULE_MANAGER_H
#define MODULE_MANAGER_H
#include "module_base.h"
#include <memory/memory.h>
#include <kstl/vector.h>

namespace modules {
class module_manager {
public:
    static module_manager& get();

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

    bool start_module(const kstl::string& name);
    bool stop_module(const kstl::string& name);

    bool start_module(module_base* mod);
    bool stop_module(module_base* mod);

    // ------------------------------------------------------------------------
    // Command Interface
    // ------------------------------------------------------------------------

    bool send_command(const kstl::string&   name,
                      uint64_t      command,
                      const void*   data_in,
                      size_t        data_in_size,
                      void*         data_out,
                      size_t        data_out_size);

    // ------------------------------------------------------------------------
    // Debug / Inspection
    // ------------------------------------------------------------------------

    module_base* find_module(const kstl::string& name);


    // ------------------------------------------------------------------------
    // Module Discovery
    // ------------------------------------------------------------------------

    void start_pci_device_modules();

private:
    kstl::vector<kstl::shared_ptr<module_base>> m_modules;

    static void _module_start_task_entry(module_base* mod);
};
} // namespace modules

#endif // MODULE_MANAGER_H

#include <modules/module_manager.h>
#include <pci/pci_manager.h>
#include <core/string.h>
#include <sched/sched.h>
#include <serial/serial.h>
#include <dynpriv/dynpriv.h>

namespace modules {
module_manager g_global_module_manager;

module_manager& module_manager::get() {
    return g_global_module_manager;
}

bool module_manager::register_module(const kstl::shared_ptr<module_base>& mod) {
    if (!mod) {
        // Cannot register a null pointer
        serial::printf("[!] Failed to register module, parameter was nullptr\n");
        return false;
    }

    // Check if a module with the same name is already registered
    module_base* existing = find_module(mod->name());
    if (existing != nullptr) {
        // A module with this name is already registered
        serial::printf("[!] Failed to register module with duplicate name: '%s'\n", mod->name());
        return false;
    }

    // Set the module state to indicate that it has been loaded into the system
    mod->m_state = module_state::loaded;

    m_modules.push_back(mod);
    return true;
}

bool module_manager::unregister_module(module_base* mod) {
    if (!mod) {
        serial::printf("[!] Failed to find module '%s'\n", mod->name());
        return false;
    }

    for (size_t i = 0; i < m_modules.size(); i++) {
        if (m_modules[i].get() == mod) {
            m_modules.erase(i);

            // Set the module state to indicate that it has been unloaded
            mod->m_state = module_state::unloaded;

            return true;
        }
    }

    serial::printf("[!] Failed to unregister module, module '%s' not found\n", mod->name());
    return false;
}

bool module_manager::start_module(const kstl::string& name) {
    module_base* mod = find_module(name);
    if (!mod) {
        serial::printf("[!] Failed to find module '%s'\n", name);
        return false;
    }

    return start_module(mod);
}

bool module_manager::stop_module(const kstl::string& name) {
    module_base* mod = find_module(name);
    if (!mod) {
        serial::printf("[!] Failed to find module '%s'\n", name);
        return false;
    }

    return stop_module(mod);
}

bool module_manager::start_module(module_base* mod) {
    RUN_ELEVATED({
        task_control_block* module_task = sched::create_unpriv_kernel_task(
            reinterpret_cast<sched::task_entry_fn_t>(&module_manager::_module_start_task_entry),
            mod
        );

        strcpy(module_task->name, mod->name().c_str());

        sched::scheduler::get().add_task(module_task);
    });
    return true;
}

bool module_manager::stop_module(module_base* mod) {
    bool success = mod->stop();
    mod->m_state = success ? module_state::stopped : module_state::error;

    if (!success) {
        serial::printf("[!] Failed to stop module '%s'\n", mod->name());
    }

    return success;
}

bool module_manager::send_command(
    const kstl::string&   name,
    uint64_t      command,
    const void*   data_in,
    size_t        data_in_size,
    void*         data_out,
    size_t        data_out_size
) {
    module_base* mod = find_module(name);
    if (!mod) {
        serial::printf("[!] Failed to find module '%s'\n", name);
        return false;
    }

    return send_command(mod, command, data_in, data_in_size, data_out, data_out_size);
}

bool module_manager::send_command(
    module_base*  mod,
    uint64_t      command,
    const void*   data_in,
    size_t        data_in_size,
    void*         data_out,
    size_t        data_out_size
) {
    return mod->on_command(command, data_in, data_in_size, data_out, data_out_size);
}

module_base* module_manager::find_module(const kstl::string& name) {
    if (name.empty()) {
        return nullptr;
    }

    for (size_t i = 0; i < m_modules.size(); i++) {
        const kstl::string& mod_name = m_modules[i]->name();
        if (mod_name == name) {
            return m_modules[i].get();
        }
    }

    return nullptr;
}

void module_manager::_module_start_task_entry(module_base* mod) {
    if (!mod->init()) {
        mod->m_state = module_state::error;
        serial::printf("[!] Failed to initialize module '%s'\n", mod->name());
        sched::exit_thread();
    }

    mod->m_state = module_state::running;

    if (!mod->start()) {
        mod->m_state = module_state::error;
        serial::printf("[!] Failed to start module '%s'\n", mod->name());
        sched::exit_thread();
    }

    // Final thread exit
    sched::exit_thread();
}
} // namespace modules

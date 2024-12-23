#include <modules/module_manager.h>
#include <core/string.h>
#include <sched/sched.h>
#include <serial/serial.h>

namespace modules {
bool module_manager::register_module(const kstl::shared_ptr<module_base>& mod) {
    if (!mod) {
        // Cannot register a null pointer
        serial::printf("[!] Failed to register module\n");
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

bool module_manager::init_module(const char* name) {
    module_base* mod = find_module(name);
    if (!mod) {
        return false; 
    }

    bool success = mod->init();
    mod->m_state = success ? module_state::initialized : module_state::error;

    return success;
}

bool module_manager::start_module(const char* name) {
    module_base* mod = find_module(name);
    if (!mod) {
        return false;
    }

    task_control_block* module_task = sched::create_unpriv_kernel_task(
        reinterpret_cast<sched::task_entry_fn_t>(&module_manager::_module_start_task_entry),
        mod
    );

    strcpy(module_task->name, name);

    sched::scheduler::get().add_task(module_task);
}

bool module_manager::stop_module(const char* name) {
    module_base* mod = find_module(name);
    if (!mod) {
        return false;
    }

    bool success = mod->stop();
    mod->m_state = success ? module_state::stopped : module_state::error;

    return success;
}

bool module_manager::send_command(
    const char*   name,
    uint64_t      command,
    const void*   data_in,
    size_t        data_in_size,
    void*         data_out,
    size_t        data_out_size
) {
    module_base* mod = find_module(name);
    if (!mod) {
        return false;
    }

    return mod->on_command(command, data_in, data_in_size, data_out, data_out_size);
}

module_base* module_manager::find_module(const char* name) {
    if (!name) {
        return nullptr;
    }

    for (size_t i = 0; i < m_modules.size(); i++) {
        const char* mod_name = m_modules[i]->name();
        if (mod_name && (strcmp(mod_name, name) == 0)) {
            return m_modules[i].get();
        }
    }

    serial::printf("[!] Failed to find module '%s'\n", name);
    return nullptr;
}

void module_manager::_module_start_task_entry(module_base* mod) {
    mod->m_state = module_state::running;
    if (!mod->start()) {
        mod->m_state = module_state::error;
    }

    sched::exit_thread();
}
} // namespace modules

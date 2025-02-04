#ifndef MODULE_MANAGER_H
#define MODULE_MANAGER_H
#include "module_base.h"
#include <memory/memory.h>
#include <kstl/vector.h>

namespace modules {
/**
 * @class module_manager
 * @brief Manages the lifecycle and interaction of system modules.
 * 
 * Provides functionality for registering, starting, stopping, and sending commands to modules.
 * Also includes methods for debugging and inspecting module states.
 */
class module_manager {
public:
    /**
     * @brief Retrieves the singleton instance of the module manager.
     * @return Reference to the singleton instance of the `module_manager`.
     * 
     * Provides global access to the module manager for coordinating module operations.
     */
    static module_manager& get();

    /**
     * @brief Default constructor for the module manager.
     */
    module_manager() = default;

    /**
     * @brief Default destructor for the module manager.
     */
    ~module_manager() = default;

    /**
     * @brief Deleted copy constructor to prevent copying.
     */
    module_manager(const module_manager&) = delete;

    /**
     * @brief Deleted copy assignment operator to prevent copying.
     */
    module_manager& operator=(const module_manager&) = delete;

    // ------------------------------------------------------------------------
    // Registration
    // ------------------------------------------------------------------------

    /**
     * @brief Registers a module with the manager.
     * @param mod Shared pointer to the module to register.
     * @return True if the module is successfully registered, false otherwise.
     */
    bool register_module(const kstl::shared_ptr<module_base>& mod);

    /**
     * @brief Unregisters a module from the manager.
     * @param mod Pointer to the module to unregister.
     * @return True if the module is successfully unregistered, false otherwise.
     */
    bool unregister_module(module_base* mod);

    // ------------------------------------------------------------------------
    // Lifecycle Control
    // ------------------------------------------------------------------------

    /**
     * @brief Starts a module by its name.
     * @param name The name of the module to start.
     * @return True if the module is successfully started, false otherwise.
     */
    bool start_module(const kstl::string& name);

    /**
     * @brief Stops a module by its name.
     * @param name The name of the module to stop.
     * @return True if the module is successfully stopped, false otherwise.
     */
    bool stop_module(const kstl::string& name);

    /**
     * @brief Starts a module directly by its pointer.
     * @param mod Pointer to the module to start.
     * @return True if the module is successfully started, false otherwise.
     */
    bool start_module(module_base* mod);

    /**
     * @brief Stops a module directly by its pointer.
     * @param mod Pointer to the module to stop.
     * @return True if the module is successfully stopped, false otherwise.
     */
    bool stop_module(module_base* mod);

    // ------------------------------------------------------------------------
    // Command Interface
    // ------------------------------------------------------------------------

    /**
     * @brief Sends a command to a module by its name.
     * @param name The name of the target module.
     * @param command The command identifier.
     * @param data_in Pointer to the input data for the command.
     * @param data_in_size Size of the input data, in bytes.
     * @param data_out Pointer to the buffer for output data.
     * @param data_out_size Size of the output buffer, in bytes.
     * @return True if the command is successfully handled, false otherwise.
     * 
     * Allows interaction with a module by sending commands and exchanging data.
     */
    bool send_command(
        const kstl::string& name,
        uint64_t      command,
        const void*   data_in,
        size_t        data_in_size,
        void*         data_out,
        size_t        data_out_size
    );

    /**
     * @brief Sends a command to a module by its name.
     * @param mod Pointer to the module to stop.
     * @param command The command identifier.
     * @param data_in Pointer to the input data for the command.
     * @param data_in_size Size of the input data, in bytes.
     * @param data_out Pointer to the buffer for output data.
     * @param data_out_size Size of the output buffer, in bytes.
     * @return True if the command is successfully handled, false otherwise.
     * 
     * Allows interaction with a module by sending commands and exchanging data.
     */
    bool send_command(
        module_base*  mod,
        uint64_t      command,
        const void*   data_in,
        size_t        data_in_size,
        void*         data_out,
        size_t        data_out_size
    );

    // ------------------------------------------------------------------------
    // Debug / Inspection
    // ------------------------------------------------------------------------

    /**
     * @brief Finds a module by its name.
     * @param name The name of the module to find.
     * @return Pointer to the module if found, or `nullptr` otherwise.
     */
    module_base* find_module(const kstl::string& name);

private:
    kstl::vector<kstl::shared_ptr<module_base>> m_modules; /** List of registered modules */

    /**
     * @brief Entry point for tasks to start modules asynchronously in a separate thread.
     * @param mod Pointer to the module to start.
     */
    static void _module_start_task_entry(module_base* mod);
};
} // namespace modules

#endif // MODULE_MANAGER_H

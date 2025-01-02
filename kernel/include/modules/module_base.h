#ifndef MODULE_BASE_H
#define MODULE_BASE_H
#include <core/string.h>

namespace modules {
/**
 * @enum module_state
 * @brief Represents the state of a module in the system.
 * 
 * Modules can transition through various states during their lifecycle, from unloaded to running.
 */
enum class module_state {
    unloaded,
    loaded,
    running,
    stopped,
    error
};

/**
 * @class module_base
 * @brief Base class for defining modules in the system.
 * 
 * Provides a common interface and lifecycle hooks for modules, along with a command interface
 * and state management.
 */
class module_base {
public:
    /**
     * @brief Constructs a module with a given name.
     * @param name The name of the module.
     * 
     * Initializes the module in the `unloaded` state.
     */
    module_base(const kstl::string& name)
        : m_name(name), m_state(module_state::unloaded) {}

    /**
     * @brief Virtual destructor for the module.
     * 
     * Ensures proper cleanup in derived classes.
     */
    virtual ~module_base() = default;

    // ------------------------------------------------------------------------
    // Lifecycle Hooks
    // ------------------------------------------------------------------------

    /**
     * @brief Initializes the module.
     * @return True if the initialization succeeds, false otherwise.
     * 
     * This function is called to prepare the module for use, such as allocating resources
     * or setting up initial states.
     */
    virtual bool init() = 0;

    /**
     * @brief Starts the module.
     * @return True if the module starts successfully, false otherwise.
     * 
     * Transitions the module to the `running` state if successful. Called after initialization.
     */
    virtual bool start() = 0;

    /**
     * @brief Stops the module.
     * @return True if the module stops successfully, false otherwise.
     * 
     * Transitions the module to the `stopped` state. This is typically used for cleanup
     * or halting operations while keeping the module loaded.
     */
    virtual bool stop() = 0;

    // ------------------------------------------------------------------------
    // Command Interface
    // ------------------------------------------------------------------------

    /**
     * @brief Handles a command sent to the module.
     * @param command The command identifier.
     * @param data_in Pointer to input data for the command.
     * @param data_in_size Size of the input data, in bytes.
     * @param data_out Pointer to a buffer for output data.
     * @param data_out_size Size of the output buffer, in bytes.
     * @return True if the command is handled successfully, false otherwise.
     * 
     * Allows the module to process custom commands and exchange data with the caller.
     * The command ID and data format are defined by the module's implementation.
     */
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

    /**
     * @brief Retrieves the name of the module.
     * @return Module's registration name.
     */
    inline const kstl::string& name() const {
        return m_name;
    }

    /**
     * @brief Retrieves the current state of the module.
     * @return The current state of the module as a `module_state` value.
     */
    inline module_state state() const {
        return m_state;
    }

private:
    kstl::string  m_name;      /** The name of the module */
    module_state  m_state;     /** The current state of the module */

    friend class module_manager;
};
} // namespace modules

#endif // MODULE_BASE_H

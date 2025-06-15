#ifndef PROCESS_ENV_H
#define PROCESS_ENV_H

#include <core/string.h>
#include <kstl/hashmap.h>

#define MAX_CWD_LEN 255

typedef int64_t eid_t;

/**
 * @brief Allocates a new environment ID.
 * 
 * This function atomically allocates a new environment ID by incrementing
 * a global counter. The allocation is protected by a mutex to ensure
 * no duplicate EIDs are generated.
 * 
 * @return eid_t The newly allocated environment ID.
 */
eid_t alloc_environment_id();

/**
 * @enum process_creation_flags
 * @brief Flags used to control process creation behavior.
 */
enum class process_creation_flags : uint64_t {
    NONE            = 0 << 0,   // No special creation behavior
    CAN_ELEVATE     = 1 << 0,   // Allows process to use dynpriv functionality
    PRIV_KERN_THREAD= 1 << 1,   // Allows process to use dynpriv functionality
    IS_KERNEL       = 1 << 2,   // Indicates the process is a kernel-level thread
    SCHEDULE_NOW    = 1 << 3,   // Automatically schedules the process right after the creation
    IS_IDLE         = 1 << 4    // Indicates this is an idle process
};

// Enable bitwise operations for process_creation_flags
inline process_creation_flags operator|(process_creation_flags lhs, process_creation_flags rhs) {
    return static_cast<process_creation_flags>(
        static_cast<uint64_t>(lhs) | static_cast<uint64_t>(rhs)
    );
}

inline process_creation_flags operator&(process_creation_flags lhs, process_creation_flags rhs) {
    return static_cast<process_creation_flags>(
        static_cast<uint64_t>(lhs) & static_cast<uint64_t>(rhs)
    );
}

inline process_creation_flags operator~(process_creation_flags val) {
    return static_cast<process_creation_flags>(~static_cast<uint64_t>(val));
}

inline bool operator!(process_creation_flags val) {
    return static_cast<uint64_t>(val) == 0;
}

inline bool has_process_flag(process_creation_flags value, process_creation_flags flag) {
    return static_cast<uint64_t>(value & flag) != 0;
}

/**
 * @struct process_env
 * @brief Represents the environment and context of a process.
 * 
 * This structure contains all the context-specific data for a process,
 * including environment variables, resource limits, and process identity.
 * It can be modified independently of the process core.
 */
struct process_env {
    /**
     * @brief Process environment identity information.
     */
    struct {
        eid_t eid;                              // Environment ID
    } identity;

    /**
     * @brief Environment variables management.
     * 
     * Stores key-value pairs for environment variables.
     */
    struct env_vars {
        static constexpr size_t MAX_ENV_VARS = 32;
        struct env_var {
            char key[32] = { 0 };
            char value[128] = { 0 };
        } vars[MAX_ENV_VARS] = { 0 };
        size_t var_count;

        constexpr env_vars() : var_count(0) {}

        void init() {
            var_count = 0;
        }

        void cleanup() {
            var_count = 0;
        }

        constexpr void static_init() {
            // No-op for static initialization
        }
    } environment;

    /**
     * @brief Resource limits and tracking.
     * 
     * Defines various resource limits and their current usage.
     */
    struct resource_limits {
        uint64_t max_memory;        // Maximum memory usage in bytes
        uint64_t current_memory;    // Current memory usage in bytes
    } limits;

    /**
     * @brief Working directory information.
     */
    char working_dir[MAX_CWD_LEN + 1] = { 0 };

    /**
     * @brief File descriptor management.
     * 
     * Tracks open file descriptors and their associated resources.
     */
    struct fd_table {
        static constexpr size_t MAX_FDS = 1024;
        struct fd_entry {
            uint8_t in_use;
            int32_t flags;
            uint8_t padding[3];
        } __attribute__((packed)) entries[MAX_FDS];

        /**
         * @brief Initializes the file descriptor table.
         */
        void init() {
            for (auto& entry : entries) {
                entry.in_use = false;
                entry.flags = 0;
            }
        }

        /**
         * @brief Static initialization for compile-time initialization.
         * This is used for the idle process environment.
         */
        constexpr void static_init() {
            // No-op for static initialization
        }
    } fds;

    /**
     * @brief Process creation flags.
     * 
     * Defines various flags that control process creation
     * and execution behavior.
     */
    process_creation_flags flags;

    /**
     * @brief Constructor for process environment.
     * 
     * Initializes all members to their default values.
     * Assigns a new EID to the environment.
     */
    process_env() {
        identity.eid = alloc_environment_id();
        environment.init();
        limits = { .max_memory = 0, .current_memory = 0 };
        working_dir[0] = '\0';
        fds.init();
        flags = process_creation_flags::NONE;
    }

    /**
     * @brief Static initialization constructor.
     * 
     * Used for compile-time initialization of the idle process environment.
     * Avoids any dynamic memory allocation and EID assignment.
     */
    constexpr process_env(process_creation_flags init_flags) 
        : identity{0},
          environment{},
          limits{0, 0},
          working_dir{0},
          fds{},
          flags(init_flags) {
        environment.static_init();
        fds.static_init();
    }

    /**
     * @brief Destructor for process environment.
     * 
     * Cleans up all resources.
     */
    ~process_env() {
        environment.cleanup();
        working_dir[0] = '\0';
    }
};

// Static idle process environment that can be used for all idle processes
extern process_env g_idle_process_env;

#endif // PROCESS_ENV_H

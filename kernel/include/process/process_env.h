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

inline process_creation_flags& operator|=(process_creation_flags& lhs, process_creation_flags rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline process_creation_flags& operator&=(process_creation_flags& lhs, process_creation_flags rhs) {
    lhs = lhs & rhs;
    return lhs;
}

inline bool has_process_flag(process_creation_flags value, process_creation_flags flag) {
    return static_cast<uint64_t>(value & flag) != 0;
}

/**
 * @enum handle_type
 * @brief Represents the type of kernel object a handle refers to.
 */
enum class handle_type : uint32_t {
    INVALID = 0,    // Invalid handle
    PROCESS,        // Process handle
    THREAD,         // Thread handle
    FILE,           // File handle
    MUTEX,          // Mutex handle
    SEMAPHORE,      // Semaphore handle
    EVENT,          // Event handle
    SHARED_MEM,     // Shared memory handle
    SOCKET,         // Socket handle
    // Add more handle types as needed
};

/**
 * @struct handle_entry
 * @brief Represents a single handle table entry.
 */
struct handle_entry {
    handle_type type = handle_type::INVALID;    // Type of kernel object
    void*       object = nullptr;               // Pointer to kernel object
    uint32_t    access_rights = 0;              // Access rights for the handle
    uint32_t    flags = 0;                      // Additional handle flags
    uint64_t    metadata = 0;                   // Additional metadata (usage dependent)
} __attribute__((packed));

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
     * @brief Working directory information.
     */
    char working_dir[MAX_CWD_LEN + 1] = { '/', 0 };

    /**
     * @brief Handle management.
     * 
     * Stores handles to kernel objects, including processes.
     */
    struct handle_table {
        static constexpr size_t MAX_HANDLES = 1024;
        handle_entry entries[MAX_HANDLES];

        /**
         * @brief Default constructor.
         * 
         * Initializes all entries to their default values.
         */
        constexpr handle_table() = default;

        /**
         * @brief Initializes the handle table.
         */
        void init() {
            for (size_t i = 0; i < MAX_HANDLES; i++) {
                entries[i].type = handle_type::INVALID;
                entries[i].object = nullptr;
                entries[i].access_rights = 0;
                entries[i].flags = 0;
                entries[i].metadata = 0;
            }
        }

        /**
         * @brief Static initialization for compile-time initialization.
         */
        constexpr void static_init() {
            // No-op for static initialization
        }

        /**
         * @brief Adds a handle to the table.
         * @param type Type of kernel object
         * @param object Pointer to the kernel object
         * @param access_rights Access rights for the handle
         * @param flags Additional handle flags
         * @param metadata Additional metadata
         * @return The handle index, or SIZE_MAX if table is full
         */
        size_t add_handle(handle_type type, void* object, uint32_t access_rights = 0, uint32_t flags = 0, uint64_t metadata = 0);

        /**
         * @brief Removes a handle from the table.
         * @param handle The handle to remove
         * @return true if the handle was removed, false if invalid handle
         */
        bool remove_handle(int32_t handle);

        /**
         * @brief Gets a handle entry.
         * @param handle The handle to get
         * @return Pointer to the handle entry, or nullptr if invalid handle
         */
        handle_entry* get_handle(int32_t handle);

        /**
         * @brief Finds a handle by object pointer.
         * @param object The object pointer to find
         * @return The handle index, or SIZE_MAX if not found
         */
        size_t find_handle_by_object(void* object);
    } handles;

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
     * @brief Process creation flags.
     * 
     * Defines various flags that control process creation
     * and execution behavior.
     */
    process_creation_flags creation_flags;

    /**
     * @brief Constructor for process environment.
     * 
     * Initializes all members to their default values.
     * Assigns a new EID to the environment.
     */
    process_env() 
        : identity{0},
          environment{},
          working_dir{0},
          handles{},
          limits{0, 0},
          creation_flags(process_creation_flags::NONE) {
        identity.eid = alloc_environment_id();
        environment.init();
        handles.init();
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
          working_dir{0},
          handles{},
          limits{0, 0},
          creation_flags(init_flags) {
        environment.static_init();
        handles.static_init();
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

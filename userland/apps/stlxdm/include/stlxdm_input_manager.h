#ifndef STLXDM_INPUT_MANAGER_H
#define STLXDM_INPUT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stlibc/input/input.h>
#include "stlxdm_compositor.h"
#include "stlxdm_server.h"

// Trace/debug output control
#define STLXDM_INPUT_TRACE_ENABLED 0  // Set to 0 to disable all input manager trace output

#if STLXDM_INPUT_TRACE_ENABLED
    #define STLXDM_INPUT_TRACE(fmt, ...) printf("[STLXDM_INPUT] " fmt "\n", ##__VA_ARGS__)
#else
    #define STLXDM_INPUT_TRACE(fmt, ...) ((void)0)  // No-op when disabled
#endif

// Input manager configuration
#define STLXDM_INPUT_MAX_EVENTS_PER_FRAME   32
#define STLXDM_INPUT_CURSOR_DEFAULT_X       400
#define STLXDM_INPUT_CURSOR_DEFAULT_Y       300

// Drag operation configuration
#define STLXDM_DRAG_MIN_DISTANCE_THRESHOLD  3    // Minimum pixels to move before drag starts
#define STLXDM_DRAG_BOUNDARY_MARGIN         0    // Pixels from screen edge where dragging stops

// Drag operation types (for future extensibility)
typedef enum {
    STLXDM_DRAG_TYPE_NONE = 0,
    STLXDM_DRAG_TYPE_MOVE,                 // Move window
    STLXDM_DRAG_TYPE_RESIZE,               // Resize window (future)
    STLXDM_DRAG_TYPE_CUSTOM                // Custom drag operation (future)
} stlxdm_drag_type_t;

// Drag validation constants
#define STLXDM_DRAG_MAX_WINDOW_WIDTH       4096  // Maximum window width for drag validation
#define STLXDM_DRAG_MAX_WINDOW_HEIGHT      4096  // Maximum window height for drag validation

// Global shortcut combinations
typedef enum {
    STLXDM_SHORTCUT_NONE = 0,
    STLXDM_SHORTCUT_ALT_TAB,        // Switch windows
    STLXDM_SHORTCUT_CTRL_ALT_T,     // Terminal
    STLXDM_SHORTCUT_CTRL_ALT_ESC,   // Force quit DM
    STLXDM_SHORTCUT_PRINT_SCREEN,   // Screenshot
} stlxdm_global_shortcut_t;

// Input manager context
typedef struct {
    // Core components
    stlxdm_server_t* server;               // Reference to server for client management
    void* hud;                             // Reference to HUD for click handling
    
    // Cursor state management
    int32_t cursor_x;                      // Current cursor X position
    int32_t cursor_y;                      // Current cursor Y position
    int32_t cursor_max_x;                  // Maximum cursor X boundary
    int32_t cursor_max_y;                  // Maximum cursor Y boundary
    bool cursor_visible;                   // Cursor visibility state
    bool cursor_needs_redraw;              // Flag for cursor rendering optimization
    
    // Focus management
    uint32_t focused_window_id;            // Currently focused window ID (0 = no focus)
    stlxdm_client_info_t* focused_client;  // Pointer to focused client
    uint32_t last_click_window_id;         // Last window that received a click
    
    // Modifier key state tracking
    struct {
        bool ctrl_left;
        bool ctrl_right;
        bool alt_left;
        bool alt_right;
        bool shift_left;
        bool shift_right;
        bool super_left;                   // Windows/Cmd key
        bool super_right;
    } modifiers;
    
    // Input capture/grab state
    bool input_grabbed;                    // Is input currently grabbed by a window?
    uint32_t grab_window_id;               // Window that has grabbed input
    uint32_t grab_type;                    // Type of grab (keyboard, mouse, both)
    
    // Window drag state management
    struct {
        bool is_dragging;                  // Is a window currently being dragged?
        stlxdm_drag_type_t drag_type;      // Type of drag operation being performed
        uint32_t dragged_window_id;        // ID of the window being dragged
        stlxdm_client_info_t* dragged_client; // Pointer to the client being dragged
        int32_t drag_start_x;              // Screen X where drag started
        int32_t drag_start_y;              // Screen Y where drag started
        int32_t window_start_x;            // Window X position when drag started
        int32_t window_start_y;            // Window Y position when drag started
        int32_t drag_offset_x;             // Offset from window corner to click point
        int32_t drag_offset_y;             // Offset from window corner to click point
        uint32_t drag_start_time_ms;       // Timestamp when drag started
    } drag_state;
    
    // Event processing statistics (for debugging and monitoring)
    struct {
        uint64_t total_events_processed;
        uint64_t keyboard_events;
        uint64_t mouse_events;
        uint64_t events_this_frame;
        uint64_t global_shortcuts_triggered;
        uint64_t focus_changes;
    } stats;
    
    // Configuration flags
    struct {
        bool enable_focus_follows_mouse;   // Focus window under mouse cursor
        bool enable_click_to_focus;        // Focus window on click
        bool enable_global_shortcuts;      // Enable global shortcut processing
        bool enable_cursor_acceleration;   // Mouse acceleration
        uint32_t double_click_timeout_ms;  // Double-click detection timeout
    } config;
    
    // Internal state
    int initialized;                       // Initialization flag
    uint32_t last_click_time_ms;          // For double-click detection
    uint32_t last_clicked_button;         // Last clicked mouse button
} stlxdm_input_manager_t;

/**
 * @brief Initialize the input manager
 * @param input_mgr Input manager context to initialize
 * @param compositor Compositor reference for rendering coordination
 * @param server Server reference for client management
 * @param hud HUD reference for click handling (can be NULL)
 * @return 0 on success, negative on error
 */
int stlxdm_input_manager_init(stlxdm_input_manager_t* input_mgr, 
                             stlxdm_compositor_t* compositor,
                             stlxdm_server_t* server,
                             void* hud);

/**
 * @brief Clean up the input manager
 * @param input_mgr Input manager context to cleanup
 */
void stlxdm_input_manager_cleanup(stlxdm_input_manager_t* input_mgr);

/**
 * @brief Process all pending input events
 * @param input_mgr Input manager context
 * @return Number of events processed, negative on error
 */
int stlxdm_input_manager_process_events(stlxdm_input_manager_t* input_mgr);

/**
 * @brief Set window focus to the specified client
 * @param input_mgr Input manager context
 * @param client Client to focus (NULL to clear focus)
 * @return 0 on success, negative on error
 */
int stlxdm_input_manager_set_focus(stlxdm_input_manager_t* input_mgr, 
                                  stlxdm_client_info_t* client);

/**
 * @brief Get current cursor position
 * @param input_mgr Input manager context
 * @param x Pointer to store X position
 * @param y Pointer to store Y position
 * @return 0 on success, negative on error
 */
int stlxdm_input_manager_get_cursor_position(const stlxdm_input_manager_t* input_mgr, 
                                            int32_t* x, int32_t* y);

/**
 * @brief Get currently focused window ID
 * @param input_mgr Input manager context
 * @return Focused window ID (0 if no window is focused)
 */
uint32_t stlxdm_input_manager_get_focused_window_id(const stlxdm_input_manager_t* input_mgr);

/**
 * @brief Set cursor position
 * @param input_mgr Input manager context
 * @param x New X position
 * @param y New Y position
 * @return 0 on success, negative on error
 */
int stlxdm_input_manager_set_cursor_position(stlxdm_input_manager_t* input_mgr, 
                                            int32_t x, int32_t y);

/**
 * @brief Grab input for a specific window
 * @param input_mgr Input manager context
 * @param window_id Window to grab input for
 * @param grab_type Type of input to grab (keyboard, mouse, or both)
 * @return 0 on success, negative on error
 */
int stlxdm_input_manager_grab_input(stlxdm_input_manager_t* input_mgr, 
                                   uint32_t window_id, uint32_t grab_type);

/**
 * @brief Release input grab
 * @param input_mgr Input manager context
 * @return 0 on success, negative on error
 */
int stlxdm_input_manager_ungrab_input(stlxdm_input_manager_t* input_mgr);

/**
 * @brief Find window at the specified screen coordinates
 * @param input_mgr Input manager context
 * @param x Screen X coordinate
 * @param y Screen Y coordinate
 * @return Pointer to client info, or NULL if no window found
 */
stlxdm_client_info_t* stlxdm_input_manager_find_window_at_position(
    const stlxdm_input_manager_t* input_mgr, int32_t x, int32_t y);

/**
 * @brief Check if cursor needs to be redrawn
 * @param input_mgr Input manager context
 * @return true if cursor needs redraw, false otherwise
 */
bool stlxdm_input_manager_cursor_needs_redraw(const stlxdm_input_manager_t* input_mgr);

/**
 * @brief Mark cursor as drawn (clears redraw flag)
 * @param input_mgr Input manager context
 */
void stlxdm_input_manager_mark_cursor_drawn(stlxdm_input_manager_t* input_mgr);

/**
 * @brief Get input processing statistics
 * @param input_mgr Input manager context
 * @return Pointer to statistics structure
 */
const void* stlxdm_input_manager_get_stats(const stlxdm_input_manager_t* input_mgr);

#endif // STLXDM_INPUT_MANAGER_H
 
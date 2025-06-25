#ifndef STLXDM_SERVER_H
#define STLXDM_SERVER_H

#include <stdint.h>
#include <stlxgfx/stlxgfx.h>
#include <stlxgfx/window.h>

// Maximum number of concurrent clients
#define STLXDM_MAX_CLIENTS 16

// Client connection state
typedef enum {
    STLXDM_CLIENT_DISCONNECTED = 0,
    STLXDM_CLIENT_CONNECTED,
    STLXDM_CLIENT_AUTHENTICATED
} stlxdm_client_state_t;

// Client information structure
typedef struct {
    int socket_fd;                      // Client socket file descriptor
    stlxdm_client_state_t state;        // Connection state
    uint32_t client_id;                 // Unique client identifier
    stlxgfx_window_t* window;           // Client's window (NULL if no window created)
    uint8_t* receive_buffer;            // Dynamically allocated receive buffer
} stlxdm_client_info_t;

// Display manager server context
typedef struct {
    stlxgfx_context_t* gfx_ctx;         // Graphics context (contains server socket)
    stlxgfx_pixel_format_t format;      // Pixel format for window surfaces
    stlxdm_client_info_t clients[STLXDM_MAX_CLIENTS]; // Client table
    int client_count;                   // Number of active clients
    uint32_t next_client_id;            // Next available client ID
} stlxdm_server_t;

/**
 * Initialize the display manager server
 * @param server - server context to initialize
 * @param gfx_ctx - graphics context with server socket
 * @param format - pixel format to use for window surfaces
 * @return 0 on success, negative on error
 */
int stlxdm_server_init(stlxdm_server_t* server, stlxgfx_context_t* gfx_ctx, stlxgfx_pixel_format_t format);

/**
 * Cleanup the display manager server
 * @param server - server context to cleanup
 */
void stlxdm_server_cleanup(stlxdm_server_t* server);

/**
 * Accept new client connections
 * @param server - server context
 * @return number of new connections accepted, negative on error
 */
int stlxdm_server_accept_new_connections(stlxdm_server_t* server);

/**
 * Handle requests from all connected clients
 * @param server - server context
 * @return 0 on success, negative on error
 */
int stlxdm_server_handle_client_requests(stlxdm_server_t* server);

/**
 * Get client count
 * @param server - server context
 * @return number of active clients
 */
int stlxdm_server_get_client_count(const stlxdm_server_t* server);

/**
 * Disconnect a client by index
 * @param server - server context
 * @param client_index - index in client table
 * @return 0 on success, negative on error
 */
int stlxdm_server_disconnect_client(stlxdm_server_t* server, int client_index);

/**
 * Get a readable name for a message type (for debugging)
 * @param message_type - message type ID
 * @return string name of the message type
 */
const char* stlxdm_server_get_message_type_name(uint32_t message_type);

#endif // STLXDM_SERVER_H

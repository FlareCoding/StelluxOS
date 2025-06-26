#include "stlxdm_server.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stlxgfx/internal/stlxgfx_comm.h>
#include <stlxgfx/internal/stlxgfx_protocol.h>
#include <stlxgfx/internal/stlxgfx_event_dm.h>
#include <stlxgfx/surface.h>

int stlxdm_server_init(stlxdm_server_t* server, stlxgfx_context_t* gfx_ctx, stlxgfx_pixel_format_t format) {
    if (!server || !gfx_ctx) {
        printf("[STLXDM_SERVER] ERROR: Invalid parameters for server init\n");
        return -1;
    }
    
    // Initialize server context
    server->gfx_ctx = gfx_ctx;
    server->format = format;
    server->client_count = 0;
    server->next_client_id = 1000; // Start client IDs at 1000
    
    // Initialize all client slots as disconnected
    for (int i = 0; i < STLXDM_MAX_CLIENTS; i++) {
        server->clients[i].socket_fd = -1;
        server->clients[i].state = STLXDM_CLIENT_DISCONNECTED;
        server->clients[i].client_id = 0;
        server->clients[i].window = NULL;
        server->clients[i].receive_buffer = NULL;
    }
    
    return 0;
}

void stlxdm_server_cleanup(stlxdm_server_t* server) {
    if (!server) {
        return;
    }
    
    // Disconnect all clients
    for (int i = 0; i < STLXDM_MAX_CLIENTS; i++) {
        if (server->clients[i].state != STLXDM_CLIENT_DISCONNECTED) {
            stlxdm_server_disconnect_client(server, i);
        }

        // Free the receive buffer if it was allocated
        if (server->clients[i].receive_buffer) {
            free(server->clients[i].receive_buffer);
            server->clients[i].receive_buffer = NULL;
        }
    }
}

int stlxdm_server_accept_new_connections(stlxdm_server_t* server) {
    if (!server || !server->gfx_ctx) {
        return -1;
    }
    
    int new_connections = 0;
    
    // Keep accepting connections until none are available
    while (server->client_count < STLXDM_MAX_CLIENTS) {
        int new_client_fd = stlxgfx_try_accept(server->gfx_ctx->server_socket_fd);
        
        if (new_client_fd >= 0) {
            // Find empty slot for new client
            int slot = -1;
            for (int i = 0; i < STLXDM_MAX_CLIENTS; i++) {
                if (server->clients[i].state == STLXDM_CLIENT_DISCONNECTED) {
                    slot = i;
                    break;
                }
            }
            
            if (slot >= 0) {
                // Initialize client info
                server->clients[slot].socket_fd = new_client_fd;
                server->clients[slot].state = STLXDM_CLIENT_CONNECTED;
                server->clients[slot].client_id = server->next_client_id++;
                server->clients[slot].window = NULL;
                
                // Allocate receive buffer for this client
                server->clients[slot].receive_buffer = malloc(STLXGFX_MAX_PAYLOAD_SIZE);
                if (!server->clients[slot].receive_buffer) {
                    printf("[STLXDM_SERVER] ERROR: Failed to allocate receive buffer for client %u\n", server->clients[slot].client_id);
                    // Clean up the connection
                    close(new_client_fd);
                    server->clients[slot].socket_fd = -1;
                    server->clients[slot].state = STLXDM_CLIENT_DISCONNECTED;
                    server->clients[slot].client_id = 0;
                    break;
                }
                
                server->client_count++;
                new_connections++;
            } else {
                // No slots available, close the connection
                printf("[STLXDM_SERVER] WARNING: No client slots available, closing connection\n");
                close(new_client_fd);
                break;
            }
        } else if (new_client_fd == -2) {
            // Error accepting connection
            printf("[STLXDM_SERVER] ERROR: Failed to accept client connection\n");
            return -1;
        } else {
            // No more pending connections (new_client_fd == -1)
            break;
        }
    }
    
    return new_connections;
}

// Forward declarations for message handlers
static int stlxdm_server_handle_create_window_request(stlxdm_server_t* server,
                                                     stlxdm_client_info_t* client, 
                                                     const stlxgfx_message_header_t* header,
                                                     const uint8_t* payload);

static int stlxdm_server_handle_destroy_window_request(stlxdm_server_t* server,
                                                      stlxdm_client_info_t* client, 
                                                      const stlxgfx_message_header_t* header,
                                                      const uint8_t* payload);

static int stlxdm_server_dispatch_message(stlxdm_server_t* server,
                                         stlxdm_client_info_t* client, 
                                         const stlxgfx_message_header_t* header,
                                         const uint8_t* payload) {
    switch (header->message_type) {
        case STLXGFX_MSG_CREATE_WINDOW_REQUEST: {
            return stlxdm_server_handle_create_window_request(server, client, header, payload);
        }
        case STLXGFX_MSG_DESTROY_WINDOW_REQUEST: {
            return stlxdm_server_handle_destroy_window_request(server, client, header, payload);
        }
        default: {
            printf("[STLXDM_SERVER] Unknown message type %u from client %u\n", 
                   header->message_type, client->client_id);
            return -1;
        }
    }
}

static int stlxdm_server_handle_create_window_request(stlxdm_server_t* server,
                                                     stlxdm_client_info_t* client,
                                                     const stlxgfx_message_header_t* header,
                                                     const uint8_t* payload) {
    const stlxgfx_create_window_request_t* req = (const stlxgfx_create_window_request_t*)payload;
    

    
    // Check if client already has a window
    if (client->window != NULL) {
        printf("[STLXDM_SERVER] Client %u already has a window\n", client->client_id);
        // TODO: Send error response
        return -1;
    }
    
    // Validate window dimensions
    if (req->width == 0 || req->height == 0 || req->width > 4096 || req->height > 4096) {
        printf("[STLXDM_SERVER] Invalid window dimensions: %ux%u\n", req->width, req->height);
        // TODO: Send error response
        return -1;
    }
    
    // Validate title length
    if (req->title_length > 255) {
        printf("[STLXDM_SERVER] Title too long: %u characters\n", req->title_length);
        // TODO: Send error response
        return -1;
    }
    
    // Get graphics context from server
    stlxgfx_context_t* gfx_ctx = server->gfx_ctx;
    
    if (!gfx_ctx) {
        printf("[STLXDM_SERVER] ERROR: No graphics context available\n");
        return -1;
    }
    
    // Create window sync shared memory
    shm_handle_t sync_shm_handle;
    stlxgfx_window_sync_t* sync_data;
    
    if (stlxgfx_dm_create_window_sync_shm(gfx_ctx, &sync_shm_handle, &sync_data) != 0) {
        printf("[STLXDM_SERVER] Failed to create window sync SHM\n");
        return -1;
    }
    
    // Create surface set shared memory (triple buffering)
    shm_handle_t surface_shm_handle;
    stlxgfx_surface_t* surface0;
    stlxgfx_surface_t* surface1;
    stlxgfx_surface_t* surface2;
    stlxgfx_pixel_format_t surface_format = server->format; // Use stored format
    
    if (stlxgfx_dm_create_shared_surface_set(gfx_ctx, req->width, req->height, surface_format,
                                             &surface_shm_handle, &surface0, &surface1, &surface2) != 0) {
        printf("[STLXDM_SERVER] Failed to create surface set SHM\n");
        // Clean up sync SHM
        stlxgfx_dm_destroy_window_sync_shm(gfx_ctx, sync_shm_handle, sync_data);
        return -1;
    }
    
    // Create event ring buffer shared memory
    shm_handle_t event_shm_handle;
    stlxgfx_event_ring_t* event_ring;
    
    if (stlxgfx_dm_create_event_ring_shm(gfx_ctx, &event_shm_handle, &event_ring) != 0) {
        printf("[STLXDM_SERVER] Failed to create event ring SHM\n");
        // Clean up surface set SHM
        stlxgfx_dm_destroy_shared_surface_set(gfx_ctx, surface_shm_handle, surface0, surface1, surface2);
        // Clean up sync SHM
        stlxgfx_dm_destroy_window_sync_shm(gfx_ctx, sync_shm_handle, sync_data);
        return -1;
    }
    
    // Create and populate window structure
    stlxgfx_window_t* window = malloc(sizeof(stlxgfx_window_t));
    if (!window) {
        printf("[STLXDM_SERVER] Failed to allocate window structure\n");
        // Clean up all shared memory
        stlxgfx_dm_destroy_event_ring_shm(gfx_ctx, event_shm_handle, event_ring);
        stlxgfx_dm_destroy_shared_surface_set(gfx_ctx, surface_shm_handle, surface0, surface1, surface2);
        stlxgfx_dm_destroy_window_sync_shm(gfx_ctx, sync_shm_handle, sync_data);
        return -1;
    }
    
    // Initialize window structure
    window->window_id = client->client_id + 1000;  // Simple window ID based on client ID
    window->width = req->width;
    window->height = req->height;
    window->posx = req->posx;
    window->posy = req->posy;
    if (req->title_length > 0) {
        memcpy(window->title, req->title, req->title_length);
        window->title[req->title_length] = '\0';
    } else {
        window->title[0] = '\0';
    }
    window->format = surface_format;
    window->sync_shm_handle = sync_shm_handle;
    window->surface_shm_handle = surface_shm_handle;
    window->event_shm_handle = event_shm_handle;
    window->sync_data = sync_data;
    window->surface0 = surface0;
    window->surface1 = surface1;
    window->surface2 = surface2;
    window->event_ring = event_ring;
    window->initialized = 1;
    
    // Store window in client info
    client->window = window;
    
    // Send success response with all three SHM handles
    stlxgfx_message_header_t response_header = {
        .protocol_version = STLXGFX_PROTOCOL_VERSION,
        .message_type = STLXGFX_MSG_CREATE_WINDOW_RESPONSE,
        .sequence_number = header->sequence_number,
        .payload_size = sizeof(stlxgfx_create_window_response_t),
        .flags = 0
    };
    
    stlxgfx_create_window_response_t response = {
        .window_id = window->window_id,
        .sync_shm_handle = sync_shm_handle,
        .surface_shm_handle = surface_shm_handle,
        .event_shm_handle = event_shm_handle,
        .surface_format = surface_format,
        .result_code = STLXGFX_ERROR_SUCCESS
    };
    
    if (stlxgfx_send_message(client->socket_fd, &response_header, &response) == 0) {
        printf("[STLXDM_SERVER] Created window ID=%u with sync_shm=%lu, surface_shm=%lu, event_shm=%lu for client %u\n", 
               window->window_id, sync_shm_handle, surface_shm_handle, event_shm_handle, client->client_id);
        return 0;
    } else {
        printf("[STLXDM_SERVER] Failed to send response to client %u\n", client->client_id);
        // Clean up everything on send failure
        client->window = NULL;
        free(window);
        stlxgfx_dm_destroy_event_ring_shm(gfx_ctx, event_shm_handle, event_ring);
        stlxgfx_dm_destroy_shared_surface_set(gfx_ctx, surface_shm_handle, surface0, surface1, surface2);
        stlxgfx_dm_destroy_window_sync_shm(gfx_ctx, sync_shm_handle, sync_data);
        return -1;
    }
}

static int stlxdm_server_handle_destroy_window_request(stlxdm_server_t* server,
                                                     stlxdm_client_info_t* client,
                                                     const stlxgfx_message_header_t* header,
                                                     const uint8_t* payload) {
    const stlxgfx_destroy_window_request_t* req = (const stlxgfx_destroy_window_request_t*)payload;
    
    // Check if client has a window
    if (client->window == NULL) {
        printf("[STLXDM_SERVER] Client %u has no window to destroy\n", client->client_id);
        // Send error response
        stlxgfx_message_header_t error_header = {
            .protocol_version = STLXGFX_PROTOCOL_VERSION,
            .message_type = STLXGFX_MSG_ERROR_RESPONSE,
            .sequence_number = header->sequence_number,
            .payload_size = sizeof(stlxgfx_error_response_t),
            .flags = 0
        };
        
        stlxgfx_error_response_t error_response = {
            .error_code = STLXGFX_ERROR_INTERNAL_ERROR,
            .original_sequence = header->sequence_number,
            .error_message = "No window to destroy"
        };
        
        stlxgfx_send_message(client->socket_fd, &error_header, &error_response);
        return -1;
    }
    
    // Validate that the window ID matches
    if (client->window->window_id != req->window_id) {
        printf("[STLXDM_SERVER] Window ID mismatch: client has %u, request for %u\n", 
               client->window->window_id, req->window_id);
        // Send error response
        stlxgfx_message_header_t error_header = {
            .protocol_version = STLXGFX_PROTOCOL_VERSION,
            .message_type = STLXGFX_MSG_ERROR_RESPONSE,
            .sequence_number = header->sequence_number,
            .payload_size = sizeof(stlxgfx_error_response_t),
            .flags = 0
        };
        
        stlxgfx_error_response_t error_response = {
            .error_code = STLXGFX_ERROR_INTERNAL_ERROR,
            .original_sequence = header->sequence_number,
            .error_message = "Window ID mismatch"
        };
        
        stlxgfx_send_message(client->socket_fd, &error_header, &error_response);
        return -1;
    }
    
    // Get graphics context from server
    stlxgfx_context_t* gfx_ctx = server->gfx_ctx;
    
    if (!gfx_ctx) {
        printf("[STLXDM_SERVER] ERROR: No graphics context available\n");
        return -1;
    }
    
    // Clean up window resources
    stlxgfx_window_t* window = client->window;
    printf("[STLXDM_SERVER] Destroying window ID=%u for client %u\n", 
           window->window_id, client->client_id);
    
    // Clean up shared memory resources
    if (window->initialized) {
        // Clean up event ring buffer shared memory
        if (window->event_shm_handle != 0 && window->event_ring) {
            stlxgfx_dm_destroy_event_ring_shm(gfx_ctx, 
                                              window->event_shm_handle, 
                                              window->event_ring);
        }
        
        // Clean up surface set shared memory
        if (window->surface_shm_handle != 0 && window->surface0) {
            stlxgfx_dm_destroy_shared_surface_set(gfx_ctx, 
                                                  window->surface_shm_handle,
                                                  window->surface0, 
                                                  window->surface1, 
                                                  window->surface2);
        }
        
        // Clean up window sync shared memory
        if (window->sync_shm_handle != 0 && window->sync_data) {
            stlxgfx_dm_destroy_window_sync_shm(gfx_ctx, 
                                               window->sync_shm_handle, 
                                               window->sync_data);
        }
    }
    
    // Free window structure
    free(window);
    client->window = NULL;
    
    // Send success response
    stlxgfx_message_header_t response_header = {
        .protocol_version = STLXGFX_PROTOCOL_VERSION,
        .message_type = STLXGFX_MSG_DESTROY_WINDOW_RESPONSE,
        .sequence_number = header->sequence_number,
        .payload_size = sizeof(stlxgfx_destroy_window_response_t),
        .flags = 0
    };
    
    stlxgfx_destroy_window_response_t response = {
        .window_id = req->window_id,
        .result_code = STLXGFX_ERROR_SUCCESS
    };
    
    if (stlxgfx_send_message(client->socket_fd, &response_header, &response) == 0) {
        printf("[STLXDM_SERVER] Successfully destroyed window ID=%u for client %u\n", 
               req->window_id, client->client_id);
        return 0;
    } else {
        printf("[STLXDM_SERVER] Failed to send destroy response to client %u\n", client->client_id);
        return -1;
    }
}

int stlxdm_server_handle_client_requests(stlxdm_server_t* server) {
    if (!server) {
        return -1;
    }
    
    // Check all connected clients for messages
    for (int i = 0; i < STLXDM_MAX_CLIENTS; i++) {
        if (server->clients[i].state != STLXDM_CLIENT_CONNECTED) {
            continue;
        }
        
        stlxdm_client_info_t* client = &server->clients[i];
        
        stlxgfx_message_header_t header;
        
        // Use the pre-allocated per-client receive buffer
        int result = stlxgfx_try_receive(client->socket_fd, &header, client->receive_buffer, STLXGFX_MAX_PAYLOAD_SIZE);
        
        if (result == 0) {
            // Message received, dispatch to appropriate handler
            printf("[STLXDM_SERVER] Received %s (%u) from client %u (slot %d)\n", 
                   stlxdm_server_get_message_type_name(header.message_type),
                   header.message_type, client->client_id, i);
            
            if (stlxdm_server_dispatch_message(server, client, &header, client->receive_buffer) < 0) {
                printf("[STLXDM_SERVER] Failed to handle message from client %u\n", client->client_id);
            }
        } else if (result == -2) {
            // Client disconnected or error
            printf("[STLXDM_SERVER] Client %u (slot %d) disconnected\n", client->client_id, i);
            stlxdm_server_disconnect_client(server, i);
        }
        // result == -1 means no data available, which is normal
    }
    
    return 0;
}

int stlxdm_server_get_client_count(const stlxdm_server_t* server) {
    return server ? server->client_count : 0;
}

int stlxdm_server_disconnect_client(stlxdm_server_t* server, int client_index) {
    if (!server || client_index < 0 || client_index >= STLXDM_MAX_CLIENTS) {
        return -1;
    }
    
    stlxdm_client_info_t* client = &server->clients[client_index];
    
    if (client->state == STLXDM_CLIENT_DISCONNECTED) {
        return 0; // Already disconnected
    }
    
    printf("[STLXDM_SERVER] Disconnecting client %u (slot %d, fd %d)\n", 
           client->client_id, client_index, client->socket_fd);
    
    // Close socket
    if (client->socket_fd >= 0) {
        close(client->socket_fd);
    }
    
    // Clean up window if it exists
    if (client->window) {
        stlxgfx_window_t* window = client->window;
        printf("[STLXDM_SERVER] Cleaning up window ID=%u for client %u\n", 
               window->window_id, client->client_id);
        
        // Properly destroy window with shared memory cleanup
        if (window->initialized) {
            // Clean up event ring buffer shared memory
            if (window->event_shm_handle != 0 && window->event_ring) {
                stlxgfx_dm_destroy_event_ring_shm(server->gfx_ctx, 
                                                  window->event_shm_handle, 
                                                  window->event_ring);
            }
            
            // Clean up surface set shared memory
            if (window->surface_shm_handle != 0 && window->surface0) {
                stlxgfx_dm_destroy_shared_surface_set(server->gfx_ctx, 
                                                      window->surface_shm_handle,
                                                      window->surface0, 
                                                      window->surface1, 
                                                      window->surface2);
            }
            
            // Clean up window sync shared memory
            if (window->sync_shm_handle != 0 && window->sync_data) {
                stlxgfx_dm_destroy_window_sync_shm(server->gfx_ctx, 
                                                   window->sync_shm_handle, 
                                                   window->sync_data);
            }
        }
        
        // Free window structure
        free(window);
        client->window = NULL;
    }
    
    // Reset client info
    client->socket_fd = -1;
    client->state = STLXDM_CLIENT_DISCONNECTED;
    client->client_id = 0;
    client->window = NULL;
    
    // Free the receive buffer
    if (client->receive_buffer) {
        free(client->receive_buffer);
        client->receive_buffer = NULL;
    }
    
    // Decrease client count
    server->client_count--;
    
    return 0;
}

const char* stlxdm_server_get_message_type_name(uint32_t message_type) {
    switch (message_type) {
        case STLXGFX_MSG_CREATE_WINDOW_REQUEST:
            return "CREATE_WINDOW_REQUEST";
        case STLXGFX_MSG_CREATE_WINDOW_RESPONSE:
            return "CREATE_WINDOW_RESPONSE";
        case STLXGFX_MSG_DESTROY_WINDOW_REQUEST:
            return "DESTROY_WINDOW_REQUEST";
        case STLXGFX_MSG_DESTROY_WINDOW_RESPONSE:
            return "DESTROY_WINDOW_RESPONSE";
        case STLXGFX_MSG_ERROR_RESPONSE:
            return "ERROR_RESPONSE";
        default:
            return "UNKNOWN";
    }
}

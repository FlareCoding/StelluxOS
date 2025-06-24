#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "stlxgfx/internal/stlxgfx_ctx.h"
#include "stlxgfx/internal/stlxgfx_protocol.h"

// Helper function to set socket to non-blocking mode
static int stlxgfx_set_nonblocking(int socket_fd) {
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags == -1) {
        printf("STLXGFX: Failed to get socket flags: %d\n", errno);
        return -1;
    }
    
    if (fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        printf("STLXGFX: Failed to set socket non-blocking: %d\n", errno);
        return -1;
    }
    
    return 0;
}

static int stlxgfx_setup_server_socket(stlxgfx_context_t* ctx) {
    // Create Unix domain socket
    ctx->server_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->server_socket_fd < 0) {
        printf("STLXGFX: Failed to create server socket: %d\n", errno);
        return -1;
    }
    
    // Bind to socket path
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, STLXGFX_DM_SOCKET_PATH);
    
    if (bind(ctx->server_socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("STLXGFX: Failed to bind server socket: %d\n", errno);
        close(ctx->server_socket_fd);
        return -1;
    }
    
    // Start listening
    if (listen(ctx->server_socket_fd, 5) < 0) {
        printf("STLXGFX: Failed to listen on server socket: %d\n", errno);
        close(ctx->server_socket_fd);
        return -1;
    }
    
    // Set server socket to non-blocking mode
    if (stlxgfx_set_nonblocking(ctx->server_socket_fd) != 0) {
        close(ctx->server_socket_fd);
        return -1;
    }
    
    return 0;
}

static int stlxgfx_connect_to_server(stlxgfx_context_t* ctx) {
    // Create Unix domain socket
    ctx->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->socket_fd < 0) {
        printf("STLXGFX: Failed to create client socket: %d\n", errno);
        return -1;
    }
    
    // Connect to display manager
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, STLXGFX_DM_SOCKET_PATH);
    
    if (connect(ctx->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("STLXGFX: Failed to connect to display manager: %d\n", errno);
        close(ctx->socket_fd);
        return -1;
    }
    
    ctx->connected_to_dm = 1;
    return 0;
}

int stlxgfx_init_comm_channel(stlxgfx_context_t* ctx) {
    if (!ctx) return -1;
    
    // Initialize common fields
    ctx->socket_fd = -1;
    ctx->server_socket_fd = -1;
    ctx->next_sequence_number = 1;
    ctx->client_count = 0;
    ctx->connected_to_dm = 0;
    
    if (ctx->mode == STLXGFX_MODE_DISPLAY_MANAGER) {
        return stlxgfx_setup_server_socket(ctx);
    } else {
        return stlxgfx_connect_to_server(ctx);
    }
}

void stlxgfx_cleanup_comm_channel(stlxgfx_context_t* ctx) {
    if (!ctx) return;
    
    if (ctx->socket_fd >= 0) {
        close(ctx->socket_fd);
        ctx->socket_fd = -1;
    }
    
    if (ctx->server_socket_fd >= 0) {
        close(ctx->server_socket_fd);
        ctx->server_socket_fd = -1;
    }
    
    ctx->connected_to_dm = 0;
}

int stlxgfx_send_message(int socket_fd, const stlxgfx_message_header_t* header, const void* payload) {
    // Send header
    ssize_t sent = send(socket_fd, header, sizeof(*header), 0);
    if (sent != sizeof(*header)) {
        printf("STLXGFX: Failed to send message header: %zd\n", sent);
        return -1;
    }
    
    // Send payload if present
    if (header->payload_size > 0 && payload) {
        sent = send(socket_fd, payload, header->payload_size, 0);
        if (sent != (ssize_t)header->payload_size) {
            printf("STLXGFX: Failed to send message payload: %zd\n", sent);
            return -1;
        }
    }
    
    return 0;
}

int stlxgfx_receive_message(int socket_fd, stlxgfx_message_header_t* header, void* payload, size_t max_payload_size) {
    // Receive header
    ssize_t received = recv(socket_fd, header, sizeof(*header), MSG_WAITALL);
    if (received != sizeof(*header)) {
        printf("STLXGFX: Failed to receive message header: %zd\n", received);
        return -1;
    }
    
    // Validate protocol version
    if (header->protocol_version != STLXGFX_PROTOCOL_VERSION) {
        printf("STLXGFX: Protocol version mismatch: got %u, expected %u\n", 
               header->protocol_version, STLXGFX_PROTOCOL_VERSION);
        return -1;
    }
    
    // Receive payload if present
    if (header->payload_size > 0) {
        if (header->payload_size > max_payload_size) {
            printf("STLXGFX: Payload too large: %u > %zu\n", header->payload_size, max_payload_size);
            return -1;
        }
        
        if (!payload) {
            printf("STLXGFX: Payload expected but buffer is NULL\n");
            return -1;
        }
        
        received = recv(socket_fd, payload, header->payload_size, MSG_WAITALL);
        if (received != (ssize_t)header->payload_size) {
            printf("STLXGFX: Failed to receive message payload: %zd\n", received);
            return -1;
        }
    }
    
    return 0;
}

int stlxgfx_try_accept(int server_fd) {
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1; // No clients waiting
        }
        return -2; // Error
    }
    
    // Set client socket to non-blocking too
    if (stlxgfx_set_nonblocking(client_fd) != 0) {
        close(client_fd);
        return -2;
    }
    
    return client_fd;
}

int stlxgfx_try_receive(int client_fd, stlxgfx_message_header_t* header, void* payload, size_t max_payload_size) {
    // Try to receive header (non-blocking)
    ssize_t received = recv(client_fd, header, sizeof(*header), MSG_DONTWAIT);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1; // No data available
        }
        return -2; // Error or disconnect
    }
    
    if (received == 0) {
        return -2; // Client disconnected
    }
    
    if (received != sizeof(*header)) {
        return -2; // Protocol error
    }
    
    // Validate protocol version
    if (header->protocol_version != STLXGFX_PROTOCOL_VERSION) {
        printf("STLXGFX: Protocol version mismatch: got %u, expected %u\n", 
               header->protocol_version, STLXGFX_PROTOCOL_VERSION);
        return -2;
    }
    
    // Receive payload if present (blocking read since we know it's coming)
    if (header->payload_size > 0) {
        if (header->payload_size > max_payload_size || !payload) {
            return -2;
        }
        
        received = recv(client_fd, payload, header->payload_size, MSG_WAITALL);
        if (received != (ssize_t)header->payload_size) {
            return -2;
        }
    }
    
    return 0; // Success
}
 
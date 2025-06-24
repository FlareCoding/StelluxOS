#ifndef STLXGFX_COMM_H
#define STLXGFX_COMM_H

#include "stlxgfx_ctx.h"
#include "stlxgfx_protocol.h"

/**
 * Initialize socket communication channel based on context mode
 * @param ctx - graphics context 
 * @return 0 on success, negative on error
 */
int stlxgfx_init_comm_channel(stlxgfx_context_t* ctx);

/**
 * Clean up socket communication channel
 * @param ctx - graphics context
 */
void stlxgfx_cleanup_comm_channel(stlxgfx_context_t* ctx);

/**
 * Send a message over the socket
 * @param socket_fd - socket file descriptor
 * @param header - message header
 * @param payload - message payload (can be NULL if payload_size is 0)
 * @return 0 on success, negative on error
 */
int stlxgfx_send_message(int socket_fd, const stlxgfx_message_header_t* header, const void* payload);

/**
 * Receive a message from the socket
 * @param socket_fd - socket file descriptor
 * @param header - buffer for message header
 * @param payload - buffer for message payload (can be NULL if no payload expected)
 * @param max_payload_size - maximum payload buffer size
 * @return 0 on success, negative on error
 */
int stlxgfx_receive_message(int socket_fd, stlxgfx_message_header_t* header, void* payload, size_t max_payload_size);

/**
 * Try to accept a new client (non-blocking)
 * @param server_fd - server socket file descriptor
 * @return client fd on success, -1 if no clients waiting, -2 on error
 */
int stlxgfx_try_accept(int server_fd);

/**
 * Try to receive a message (non-blocking)
 * @param client_fd - client socket file descriptor  
 * @param header - buffer for message header
 * @param payload - buffer for message payload
 * @param max_payload_size - maximum payload buffer size
 * @return 0 on success, -1 if no data available, -2 on error/disconnect
 */
int stlxgfx_try_receive(int client_fd, stlxgfx_message_header_t* header, void* payload, size_t max_payload_size);

#endif // STLXGFX_COMM_H
 
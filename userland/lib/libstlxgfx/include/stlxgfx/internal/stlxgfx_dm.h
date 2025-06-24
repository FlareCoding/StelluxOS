#ifndef STLXGFX_DM_H
#define STLXGFX_DM_H

#include "stlxgfx_ctx.h"
#include <stlxgfx/internal/stlxgfx_protocol.h>

/**
 * Accept a new client connection to the display manager
 * @param ctx - display manager context
 * @return client file descriptor on success, negative on error
 */
int stlxgfx_dm_accept_client(stlxgfx_context_t* ctx);

/**
 * Handle a message from a client
 * @param ctx - display manager context
 * @param client_fd - client file descriptor
 * @return 0 on success, negative on error
 */
int stlxgfx_dm_handle_client_message(stlxgfx_context_t* ctx, int client_fd);

#endif // STLXGFX_DM_H

#ifndef STLXGFX_CTX_H
#define STLXGFX_CTX_H

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <stlxgfx/internal/stb_truetype.h>
#pragma GCC diagnostic pop

#include <stlxgfx/stlxgfx.h>

#define STLXGFX_PAGE_SIZE 0x1000

// Internal context structure definition
struct stlxgfx_context {
    stlxgfx_mode_t mode;
    int initialized;
    
    // Font management (DM mode only)
    void* font_data;
    size_t font_data_size;
    stbtt_fontinfo font_info;
    int font_loaded;
    
    // Socket communication
    int socket_fd;
    uint32_t next_sequence_number;
    
    // Display manager specific
    int server_socket_fd;  // DM mode: listening socket
    int client_count;      // DM mode: number of connected clients
    
    // Application specific  
    int connected_to_dm;   // App mode: connection status
};

#endif // STLXGFX_CTX_H
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

// Character bitmap cache for text rendering optimization
#define STLXGFX_CHAR_CACHE_SIZE 128  // Cache first 128 ASCII characters
typedef struct {
    unsigned char* bitmap;
    int width;
    int height;
    int xoff;
    int yoff;
    uint32_t font_size;  // Font size this bitmap was generated for
    int valid;           // Whether this cache entry is valid
} stlxgfx_char_cache_t;

// Internal context structure definition
struct stlxgfx_context {
    stlxgfx_mode_t mode;
    int initialized;
    
    // Font management (DM mode only)
    void* font_data;
    size_t font_data_size;
    stbtt_fontinfo font_info;
    int font_loaded;
    
    // Character bitmap cache
    stlxgfx_char_cache_t char_cache[STLXGFX_CHAR_CACHE_SIZE];
    uint32_t cached_font_size;  // Font size currently cached
    
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
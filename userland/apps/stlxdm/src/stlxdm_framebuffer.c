#include "stlxdm_framebuffer.h"
#include "stlxdm.h"
#include <stdio.h>
#include <stlibc/stlibc.h>

int stlxdm_get_framebuffer_info(struct gfx_framebuffer_info* fb_info) {
    if (!fb_info) {
        printf("ERROR: NULL framebuffer info pointer\n");
        return -1;
    }
    
    long result = syscall2(SYS_GRAPHICS_FRAMEBUFFER_OP, GFX_OP_GET_INFO, (uint64_t)fb_info);
    if (result != 0) {
        printf("ERROR: Failed to get framebuffer info: %ld\n", result);
        return -1;
    }

    return 0;
}

uint8_t* stlxdm_map_framebuffer(void) {
    long map_result = syscall1(SYS_GRAPHICS_FRAMEBUFFER_OP, GFX_OP_MAP_FRAMEBUFFER);
    if (map_result <= 0) {
        printf("ERROR: Failed to map framebuffer: %ld\n", map_result);
        return NULL;
    }

    return (uint8_t*)map_result;
}

int stlxdm_unmap_framebuffer(void) {
    long result = syscall1(SYS_GRAPHICS_FRAMEBUFFER_OP, GFX_OP_UNMAP_FRAMEBUFFER);
    if (result != 0) {
        printf("ERROR: Failed to unmap framebuffer: %ld\n", result);
        return -1;
    }

    return 0;
}

int stlxdm_begin_frame(void) {
    long result = syscall1(SYS_GRAPHICS_FRAMEBUFFER_OP, GFX_OP_DISABLE_PREEMPT);
    if (result != 0) {
        printf("ERROR: Failed to disable preemption: %ld\n", result);
        return -1;
    }
    
    return 0;
}

int stlxdm_end_frame(void) {
    long result = syscall1(SYS_GRAPHICS_FRAMEBUFFER_OP, GFX_OP_ENABLE_PREEMPT);
    if (result != 0) {
        printf("ERROR: Failed to enable preemption: %ld\n", result);
        return -1;
    }
    
    return 0;
}

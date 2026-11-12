#define _GNU_SOURCE
#include <stlx/handle.h>
#include <stlx/syscall_nums.h>
#include <unistd.h>

int stlx_handle_restrict(int handle, uint32_t rights) {
    return (int)syscall(SYS_HANDLE_RESTRICT, handle, rights);
}

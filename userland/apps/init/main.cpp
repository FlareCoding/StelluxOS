#include <stlibc/stlibc.h>
#include <stlibc/memory/malloc.h>
#include <stlibc/system/syscall.h>
#include <stlibc/memory/memory.h>
#include <stlibc/string/string.h>
#include <stlibc/string/format.h>
#include <stlibc/proc/proc.h>

int main() {
    printf("Hello Userland!\n");

    printf("this->pid: %lli\n", getpid());

    const int sz = 6;

    int* arr = (int*)malloc(sz * sizeof(int));
    printf("arr: %p\n", arr);

    for (int i = 0; i < sz; i++) {
        arr[i] = i * i * i;
    }

    for (int i = 0; i < sz; i++) {
        printf("arr[%d]: %i\n", i, arr[i]);
    }

    free(arr);

    while (true) {
        handle_t shell_handle = proc_create("/initrd/bin/shell", PROC_NEW_ENV | PROC_CAN_ELEVATE, PROC_ACCESS_ALL, PROC_HANDLE_NONE, nullptr);

        if (shell_handle < 0) {
            printf("Failed to create 'shell' process!\n");
            break;
        } else {
            int shell_exit_code = 0;
            if (proc_wait(shell_handle, &shell_exit_code) != 0) {
                printf("proc_wait failed on shell_pid\n");
            }
        }
    }

    printf("init process exiting!\n");
    return 0;
}

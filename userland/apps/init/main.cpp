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

    if (proc_create("/initrd/bin/hello_world", PROC_NEW_ENV) < 0) {
        printf("Failed to create 'hello_world' process!\n");
    }

    printf("init process exiting!\n");
    return 0;
}

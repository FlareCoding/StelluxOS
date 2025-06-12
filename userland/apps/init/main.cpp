#include <stlibc/stlibc.h>
#include <stlibc/memory/malloc.h>
#include <stlibc/system/syscall.h>
#include <stlibc/memory/memory.h>
#include <stlibc/string/string.h>
#include <stlibc/string/format.h>

int main() {
    printf("Hello Userland!\n");

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

    printf("init process exiting!\n");
    return 0;
}

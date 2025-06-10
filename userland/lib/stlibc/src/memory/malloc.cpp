#include <stlibc/memory/malloc.h>
#include <stlibc/memory/heap.h>

extern "C" {

void* malloc(size_t size) {
    return stlibc::heap_allocator::get().allocate(size);
}

void free(void* ptr) {
    if (ptr) {
        stlibc::heap_allocator::get().free(ptr);
    }
}

void* realloc(void* ptr, size_t size) {
    return stlibc::heap_allocator::get().reallocate(ptr, size);
}

} // extern "C"

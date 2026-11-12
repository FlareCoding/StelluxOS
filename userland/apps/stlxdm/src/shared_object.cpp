#include "shared_object.hpp"

#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

int make_shared_object(const void* data, size_t len) {
    int object = memfd_create("stlxdm-clipboard", MFD_CLOEXEC);
    bool is_sized = object >= 0 && ftruncate(object, static_cast<off_t>(len)) == 0;
    void* shared = is_sized ? mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, object, 0) : MAP_FAILED;
    if (shared == MAP_FAILED) {
        int error = errno;
        if (object >= 0) {
            close(object);
        }

        errno = error;
        return -1;
    }

    memcpy(shared, data, len);
    munmap(shared, len);

    return object;
}

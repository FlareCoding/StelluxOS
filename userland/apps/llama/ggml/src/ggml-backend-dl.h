#pragma once

#if defined(__stellux__)
#include <string>
#include <memory>

// Minimal fs::path shim for Stellux (dynamic loading is a no-op)
namespace fs {
    using path = std::string;
}

using dl_handle = void;
struct dl_handle_deleter {
    void operator()(void *) {}
};
using dl_handle_ptr = std::unique_ptr<dl_handle, dl_handle_deleter>;

#else // !__stellux__

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#   include <winevt.h>
#else
#    include <dlfcn.h>
#    include <unistd.h>
#endif
#include <filesystem>

namespace fs = std::filesystem;

#ifdef _WIN32

using dl_handle = std::remove_pointer_t<HMODULE>;

struct dl_handle_deleter {
    void operator()(HMODULE handle) {
        FreeLibrary(handle);
    }
};

#else

using dl_handle = void;

struct dl_handle_deleter {
    void operator()(void * handle) {
        dlclose(handle);
    }
};

#endif

using dl_handle_ptr = std::unique_ptr<dl_handle, dl_handle_deleter>;

#endif // __stellux__

dl_handle * dl_load_library(const fs::path & path);
void * dl_get_sym(dl_handle * handle, const char * name);
const char * dl_error();

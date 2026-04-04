#include "ggml-backend-dl.h"

#if defined(__stellux__)

// Stellux: no dynamic backend loading
dl_handle * dl_load_library(const fs::path &) { return nullptr; }
void * dl_get_sym(dl_handle *, const char *) { return nullptr; }
const char * dl_error() { return "dynamic loading not supported"; }

#elif defined(_WIN32)

dl_handle * dl_load_library(const fs::path & path) {
    DWORD old_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
    SetErrorMode(old_mode | SEM_FAILCRITICALERRORS);
    HMODULE handle = LoadLibraryW(path.wstring().c_str());
    SetErrorMode(old_mode);
    return handle;
}

void * dl_get_sym(dl_handle * handle, const char * name) {
    DWORD old_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
    SetErrorMode(old_mode | SEM_FAILCRITICALERRORS);
    void * p = (void *) GetProcAddress(handle, name);
    SetErrorMode(old_mode);
    return p;
}

const char * dl_error() { return ""; }

#else

dl_handle * dl_load_library(const fs::path & path) {
    dl_handle * handle = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    return handle;
}

void * dl_get_sym(dl_handle * handle, const char * name) {
    return dlsym(handle, name);
}

const char * dl_error() {
    const char *rslt = dlerror();
    return rslt != nullptr ? rslt : "";
}

#endif

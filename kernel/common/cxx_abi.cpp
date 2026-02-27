#include "common/logging.h"

extern "C" void __cxa_pure_virtual() {
    log::fatal("pure virtual function called");
}

// Virtual destructors generate "deleting destructors" that call operator delete.
// In freestanding, we never use delete (we use explicit dtor + kfree/ufree),
// but the linker still needs the symbol. These should never be reached.
namespace std {
    enum class align_val_t : decltype(sizeof(0)) {};
}

void operator delete(void*) noexcept {
    log::fatal("operator delete called in freestanding kernel");
}

void operator delete(void*, unsigned long) noexcept {
    log::fatal("operator delete(void*, size_t) called in freestanding kernel");
}

void operator delete(void*, std::align_val_t) noexcept {
    log::fatal("operator delete(void*, align_val_t) called in freestanding kernel");
}

#include "common/logging.h"

extern "C" void __cxa_pure_virtual() {
    log::fatal("pure virtual function called");
}

#include "common/utils/string.h"

namespace string {

size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len] != '\0') {
        ++len;
    }
    return len;
}

} // namespace string

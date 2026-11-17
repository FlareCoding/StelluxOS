#include "fade.h"

const char* fade_label(int level) {
    return level > 5 ? "fade bright" : "fade dim";
}

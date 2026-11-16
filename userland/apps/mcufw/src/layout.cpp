#include "layout.hpp"

directory_layout system_layout() {
    directory_layout layout;
    layout.hardware = "/var/lib/mcufw/hardware";
    layout.data_tools = "/var/lib/mcufw/tools";
    layout.program_tools = "/usr/arduino/tools";

    return layout;
}

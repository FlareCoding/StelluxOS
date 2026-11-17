#include "layout.hpp"

directory_layout system_layout() {
    directory_layout layout;
    layout.hardware = "/var/lib/mcufw/hardware";
    layout.data_tools = "/var/lib/mcufw/tools";
    layout.program_tools = "/usr/arduino/tools";

    layout.state = "/var/lib/mcufw";
    layout.overrides = "/etc/res/mcufw/overrides";
    layout.libraries = "/var/lib/mcufw/libraries";
    layout.builds = "/var/cache/mcufw/builds";

    return layout;
}

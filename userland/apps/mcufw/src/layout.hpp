#ifndef MCUFW_LAYOUT_HPP
#define MCUFW_LAYOUT_HPP

// The folders mcufw reads what is installed from and keeps what it writes in. Every module takes them from its
// caller, so each host test points a module at a tree of its own.

#include <string>

// The file in the state folder that installs and removals lock exclusively and `mcufw boards` locks shared
constexpr const char* STATE_LOCK_NAME = "lock";

struct directory_layout {
    std::string hardware;       // installed cores, as <packager>/<architecture>/<version>
    std::string data_tools;     // tools mcufw installs from a core's index entry, as <packager>/<name>/<version>
    std::string program_tools;  // links to programs Stellux packages provide, named as data tools are
    std::string state;          // Arduino's indexes, the staging folder and the lock
    std::string overrides;      // the image's Stellux overrides, as <packager>/<architecture>/<version>.txt
    std::string libraries;      // libraries mcufw installs or a person places, one folder each
    std::string builds;         // one build tree per sketch folder
};

// The folders of a running system
directory_layout system_layout();

#endif // MCUFW_LAYOUT_HPP

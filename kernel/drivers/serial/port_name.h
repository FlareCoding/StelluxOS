#ifndef STELLUX_DRIVERS_SERIAL_PORT_NAME_H
#define STELLUX_DRIVERS_SERIAL_PORT_NAME_H

#include "drivers/serial/serial.h"
#include "common/list.h"
#include "fs/fstypes.h"

namespace serial {

// A port's name is its file name in /dev/serial
constexpr size_t PORT_NAME_MAX = fs::NAME_MAX;

/**
 * The name a registered port holds, and the device the port belongs to. The
 * name is a base its device's ports share, then the port's channel letter.
 */
struct held_name {
    list::node link;        // in the registry's list while the port holds the name
    identity   device;      // its strings must last as long as the name is held
    size_t     base_length; // the characters of `text` before the channel letter
    char       text[PORT_NAME_MAX + 1];
};

using held_name_list = list::head<held_name, &held_name::link>;

/**
 * @brief Chooses the name for the port `id` describes, given the names other
 * ports hold. Every port of one device shares a base, which grows from the
 * chip and the last four letters or digits of the serial number, such as
 * `cp2102-0001`, by two more of those at a time, then by the location, so the
 * board that arrives first keeps the shorter base. A channel adds its letter.
 * Never blocks or allocates, so the registry chooses and links `chosen` under
 * one hold of the lock that guards `held`.
 * @return false when no name is free or fits, and `chosen` must not be read then.
 */
bool choose_port_name(const identity& id, held_name_list& held, held_name& chosen);

} // namespace serial

#endif // STELLUX_DRIVERS_SERIAL_PORT_NAME_H

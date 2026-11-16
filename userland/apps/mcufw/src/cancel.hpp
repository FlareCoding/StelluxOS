#ifndef MCUFW_CANCEL_HPP
#define MCUFW_CANCEL_HPP

// Makes SIGINT, SIGTERM and SIGPIPE set the flag `cancel_requested` reads instead of ending mcufw, so a run stops only
// where it checks that flag
void watch_for_cancel();

// Whether one of those signals arrived
bool cancel_requested();

#endif // MCUFW_CANCEL_HPP

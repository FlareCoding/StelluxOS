#ifndef MCUFW_FAILURE_HPP
#define MCUFW_FAILURE_HPP

// Why a run failed: a kind a program can act on and a sentence a person reads

#include <string>

enum class failure_kind {
    board, core, library, compile, recipe, link, size, port, flash, network, verify, lock, cancelled, io,
};

struct failure {
    failure_kind kind = failure_kind::io;
    std::string  reason;
};

#endif // MCUFW_FAILURE_HPP

#ifndef MCUFW_CLEAN_HPP
#define MCUFW_CLEAN_HPP

// What `mcufw clean` deletes: a sketch's build tree, renamed aside under its lock so a build that starts meanwhile
// makes a fresh tree, and the core cache

#include "failure.hpp"
#include "layout.hpp"

#include <string>

// Deletes the build tree of the sketch `operand` names, resolved as a build resolves it or lexically once it is gone,
// and with `cores` the core cache. An empty operand names the working directory, and nothing to delete is no failure.
bool clean_sketch(const directory_layout& layout, const std::string& operand, bool cores, failure& failed);

#endif // MCUFW_CLEAN_HPP

#ifndef MCUFW_CLEAN_HPP
#define MCUFW_CLEAN_HPP

// What `mcufw clean` deletes: a sketch's build tree, renamed aside under its lock so a build that starts meanwhile
// makes a fresh tree

#include "failure.hpp"
#include "layout.hpp"

#include <string>

// Deletes the build tree of the sketch `operand` names, the working directory when empty, resolved as a build resolves
// it while it exists and made absolute lexically once it is gone. Nothing to delete is no failure.
bool clean_sketch(const directory_layout& layout, const std::string& operand, failure& failed);

#endif // MCUFW_CLEAN_HPP

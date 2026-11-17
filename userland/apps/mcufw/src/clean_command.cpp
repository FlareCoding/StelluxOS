#include "commands.hpp"

#include "clean.hpp"

int run_clean(const arguments& parsed, const directory_layout& layout, reporter& report) {
    std::string operand = parsed.operands.empty() ? std::string() : parsed.operands[0];
    failure failed;
    if (!clean_sketch(layout, operand, parsed.cores, failed)) {
        report.failed(failed);
        return 1;
    }

    return 0;
}

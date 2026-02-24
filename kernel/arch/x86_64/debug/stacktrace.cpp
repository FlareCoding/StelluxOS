#include "debug/stacktrace.h"
#include "mm/paging.h"

namespace stacktrace {

static bool is_valid_fp(uint64_t fp, uint64_t prev_fp, pmm::phys_addr_t root) {
    if (fp == 0) return false;
    if (fp < 0xFFFF800000000000ULL) return false;
    if ((fp & 0xF) != 0) return false;
    if (prev_fp != 0 && fp <= prev_fp) return false;
    if (!paging::is_mapped(fp, root)) return false;
    return true;
}

int walk(uint64_t initial_fp, frame* out, int max_frames) {
    pmm::phys_addr_t root = paging::get_kernel_pt_root();
    int count = 0;
    uint64_t fp = initial_fp;
    uint64_t prev_fp = 0;
    while (count < max_frames && is_valid_fp(fp, prev_fp, root)) {
        auto* frame_base = reinterpret_cast<const uint64_t*>(fp);
        uint64_t saved_fp  = frame_base[0];
        uint64_t saved_rip = frame_base[1];
        if (saved_rip == 0) break;
        if (saved_rip < 0xFFFF800000000000ULL) break;
        out[count].frame_ptr   = fp;
        out[count].return_addr = saved_rip;
        count++;
        prev_fp = fp;
        fp = saved_fp;
    }
    return count;
}

} // namespace stacktrace

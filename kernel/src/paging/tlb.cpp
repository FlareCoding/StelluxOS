#include "tlb.h"

namespace paging {

__PRIVILEGED_CODE
void flushTlbPage(void* vaddr) {
    asm volatile("invlpg (%0)" : : "r" (vaddr) : "memory");
}

__PRIVILEGED_CODE
void flushTlbAll() {
    PageTable* pml4 = getCurrentTopLevelPageTable();
    setCurrentTopLevelPageTable(pml4);
}

} // namespace paging

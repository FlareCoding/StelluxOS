#include "tlb.h"

namespace paging {

void flushTlbAll() {
    PageTable* pml4 = getCurrentTopLevelPageTable();
    setCurrentTopLevelPageTable(pml4);
}

} // namespace paging

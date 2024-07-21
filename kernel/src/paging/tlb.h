#ifndef TLB_H
#define TLB_H
#include "page.h"

namespace paging {

__PRIVILEGED_CODE
void flushTlbPage(void* vaddr);

__PRIVILEGED_CODE
void flushTlbAll();

} // namespace paging
#endif

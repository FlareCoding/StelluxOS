#include "ggml-threading.h"

#if defined(__stellux__)
#include <stlx/mutex.h>
static stlx_mutex_t ggml_critical_section_mutex = STLX_MUTEX_INIT;
void ggml_critical_section_start() { stlx_mutex_lock(&ggml_critical_section_mutex); }
void ggml_critical_section_end(void) { stlx_mutex_unlock(&ggml_critical_section_mutex); }
#else
#include <mutex>
std::mutex ggml_critical_section_mutex;
void ggml_critical_section_start() { ggml_critical_section_mutex.lock(); }
void ggml_critical_section_end(void) { ggml_critical_section_mutex.unlock(); }
#endif

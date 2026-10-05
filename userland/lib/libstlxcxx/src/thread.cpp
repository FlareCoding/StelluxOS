#include <stlxstd/thread.h>
#include <stdlib.h>

namespace stlxstd::detail {

extern "C" void* stlxstd_thread_entry(void* arg) {
    auto* ctx = static_cast<thread_context*>(arg);
    ctx->invoke(ctx);
    ctx->destroy(ctx);
    free(ctx);
    return nullptr;
}

} // namespace stlxstd::detail

#include <stlxstd/thread.h>
#include <stdlib.h>
#include <unistd.h>

namespace stlxstd::detail {

extern "C" void stlxstd_thread_entry(void* arg) {
    auto* ctx = static_cast<thread_context*>(arg);
    ctx->invoke(ctx);
    ctx->destroy(ctx);
    free(ctx);
    _exit(0);
}

} // namespace stlxstd::detail

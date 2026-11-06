#ifndef GPUTEST_VIRTIO_GPU_H
#define GPUTEST_VIRTIO_GPU_H

#include <stdint.h>
#include <stlx/gpu_abi.h>

// Runs the checks of virtio-gpu with venus, whose facts `info` holds
void test_virtio_gpu(int fd, const struct gpu_info* info);

#endif // GPUTEST_VIRTIO_GPU_H

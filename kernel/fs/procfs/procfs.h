#ifndef STELLUX_FS_PROCFS_PROCFS_H
#define STELLUX_FS_PROCFS_PROCFS_H

#include "common/types.h"

namespace sched { struct task; }

/**
 * procfs describes processes as files under /proc, where ported programs
 * expect to find them. Entries under self always describe the process
 * reading them.
 */
namespace procfs {

/**
 * @brief Register the procfs driver with the VFS.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Write the absolute path of the program the task's process runs,
 * which /proc/self/exe names for it. Once the program file is unlinked, the
 * path the process started from is written with a " (deleted)" suffix. The
 * path is truncated to `size` bytes and not null-terminated, as readlink
 * reports it.
 * @return fs::OK, fs::ERR_NOENT for a task whose process runs no program,
 *   or another fs error.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t exe_path(sched::task* task, char* buf, size_t size, size_t* out_len);

} // namespace procfs

#endif // STELLUX_FS_PROCFS_PROCFS_H

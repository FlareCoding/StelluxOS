// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/process/launch_stellux.h"

#include <fcntl.h>
#include <poll.h>
#include <stlx/proc.h>
#include <unistd.h>

#include <map>

#include "base/compiler_specific.h"
#include "base/containers/heap_array.h"
#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/numerics/safe_conversions.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/environment_internal.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/threading/scoped_blocking_call.h"

namespace base::internal {

namespace {

struct ChildRecord {
  int handle = -1;  // Given up to proc_wait once the child exits.
  bool exited = false;
  int status = 0;
};

class ChildTable {
 public:
  static ChildTable& Get() {
    static NoDestructor<ChildTable> table;
    return *table;
  }

  // A new child can reuse the pid of one that exited unreaped, whose handle
  // is closed here since nothing can wait for it by pid anymore.
  void Add(ProcessId pid, int handle) {
    AutoLock lock(lock_);

    ChildRecord& child = children_[pid];
    if (child.handle >= 0) {
      close(child.handle);
    }

    child = ChildRecord{handle, false, 0};
  }

  // Hands out a duplicate of a running child's handle to poll without the
  // lock. It stays pollable even if another thread reaps the child first.
  StelluxChildState Watch(ProcessId pid,
                          ScopedFD* watch,
                          int* handle,
                          int* status) {
    AutoLock lock(lock_);

    auto it = children_.find(pid);
    if (it == children_.end()) {
      return StelluxChildState::kUnknown;
    }

    if (it->second.exited) {
      *status = it->second.status;
      return StelluxChildState::kExited;
    }

    *handle = it->second.handle;
    watch->reset(HANDLE_EINTR(dup(it->second.handle)));

    return StelluxChildState::kRunning;
  }

  // Collects the status of the exited child behind `handle`, so proc_wait
  // returns at once. A newer child that took over the pid is left alone.
  int Reap(ProcessId pid, int handle) {
    AutoLock lock(lock_);

    auto it = children_.find(pid);
    if (it == children_.end()) {
      return 0;
    }

    ChildRecord& child = it->second;
    if (!child.exited && child.handle == handle) {
      if (proc_wait(child.handle, &child.status) != 0) {
        PLOG(ERROR) << "proc_wait(" << pid << ")";
      }

      child.handle = -1;
      child.exited = true;
    }

    return child.status;
  }

 private:
  Lock lock_;
  std::map<ProcessId, ChildRecord> children_ GUARDED_BY(lock_);
};

// Creates the unstarted child, with the parent's environment unless the
// options change it
int CreateChild(const std::string& program,
                const char** argv,
                const LaunchOptions& options) {
  if (options.environment.empty() && !options.clear_environment) {
    return proc_create(program.c_str(), argv);
  }

  span<const char* const> old_environ = GetEnvironment();
  if (options.clear_environment) {
    old_environ = span<const char* const>();
  }

  // SAFETY: GetEnvironment() promises NUL-terminated strings, which is
  // what AlterEnvironment() requires of its input.
  HeapArray<char*> new_environ =
      UNSAFE_BUFFERS(AlterEnvironment(old_environ, options.environment));

  return proc_create_with_env(program.c_str(), argv,
                              const_cast<const char**>(new_environ.data()));
}

}  // namespace

Process LaunchStelluxProcess(const std::vector<std::string>& argv,
                             const LaunchOptions& options) {
  if (argv.empty()) {
    return Process();
  }

  std::vector<const char*> argv_cstr;
  argv_cstr.reserve(argv.size() + 1);
  for (const std::string& argument : argv) {
    argv_cstr.push_back(argument.c_str());
  }

  argv_cstr.push_back(nullptr);

  const std::string& program =
      options.real_path.empty() ? argv[0] : options.real_path.value();
  const int child = CreateChild(program, argv_cstr.data(), options);
  if (child < 0) {
    PLOG(ERROR) << "proc_create(" << program << ")";
    return Process();
  }

  // Closing the handle of a child that never started destroys it, so every
  // early return below leaves nothing behind.
  ScopedFD child_handle(child);

  // Like a forked child, the new process reads nothing from the terminal.
  ScopedFD null_fd(HANDLE_EINTR(open("/dev/null", O_RDONLY)));
  if (!null_fd.is_valid() ||
      proc_set_handle(child, STDIN_FILENO, null_fd.get()) != 0) {
    PLOG(WARNING) << "child keeps the parent's stdin";
  }

  for (const auto& [source, target] : options.fds_to_remap) {
    if (proc_set_handle(child, target, source) != 0) {
      PLOG(ERROR) << "proc_set_handle(" << source << " -> " << target << ")";
      return Process();
    }
  }

  DLOG_IF(WARNING, !options.current_directory.empty())
      << "children start in the parent's directory";
  DLOG_IF(WARNING, options.pre_exec_delegate)
      << "no code runs in a child before it starts";

  process_info info = {};
  if (proc_info(child, &info) != 0) {
    PLOG(ERROR) << "proc_info(" << program << ")";
    return Process();
  }

  if (proc_start(child) != 0) {
    PLOG(ERROR) << "proc_start(" << program << ")";
    return Process();
  }

  const ProcessId pid = info.pid;
  ChildTable::Get().Add(pid, child_handle.release());

  if (options.wait) {
    // Waiting on another process is blocking work ThreadRestrictions guards
    ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                            BlockingType::MAY_BLOCK);
    int status = 0;
    WaitForStelluxChild(pid, TimeDelta::Max(), &status);
  }

  return Process(pid);
}

StelluxChildState WaitForStelluxChild(ProcessId pid,
                                      TimeDelta timeout,
                                      int* status) {
  ChildTable& table = ChildTable::Get();
  ScopedFD watch;
  int handle = -1;
  StelluxChildState state = table.Watch(pid, &watch, &handle, status);
  if (state != StelluxChildState::kRunning) {
    return state;
  }

  if (!watch.is_valid()) {
    PLOG(ERROR) << "dup(" << handle << ")";
    return StelluxChildState::kRunning;
  }

  const int timeout_ms =
      timeout.is_max() ? -1
                       : saturated_cast<int>(timeout.InMillisecondsRoundedUp());
  pollfd exit_ready = {watch.get(), POLLIN, 0};
  if (HANDLE_EINTR(poll(&exit_ready, 1, timeout_ms)) <= 0) {
    return StelluxChildState::kRunning;
  }

  *status = table.Reap(pid, handle);
  return StelluxChildState::kExited;
}

}  // namespace base::internal

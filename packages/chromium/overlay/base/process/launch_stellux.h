// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_PROCESS_LAUNCH_STELLUX_H_
#define BASE_PROCESS_LAUNCH_STELLUX_H_

#include <string>
#include <vector>

#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/time/time.h"

// Stellux creates processes without fork(): a parent creates a child from an
// executable, installs the handles it inherits, and starts it. The child's
// status can only be collected through the process handle, which polls
// readable on exit, so the launcher keeps every child's handle keyed by pid.

namespace base::internal {

// Starts argv[0], or options.real_path when set, as a child process.
Process LaunchStelluxProcess(const std::vector<std::string>& argv,
                             const LaunchOptions& options);

enum class StelluxChildState {
  kUnknown,  // Not a child this process launched.
  kRunning,
  kExited,
};

// Waits up to `timeout` for the child `pid` to exit and fills `status` with
// its wait status, which stays recorded for every later caller.
StelluxChildState WaitForStelluxChild(ProcessId pid,
                                      TimeDelta timeout,
                                      int* status);

}  // namespace base::internal

#endif  // BASE_PROCESS_LAUNCH_STELLUX_H_

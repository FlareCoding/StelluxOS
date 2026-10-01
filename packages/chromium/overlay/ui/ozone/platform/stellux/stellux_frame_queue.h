// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_STELLUX_STELLUX_FRAME_QUEUE_H_
#define UI_OZONE_PLATFORM_STELLUX_STELLUX_FRAME_QUEUE_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/thread_annotations.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_ui_types.h"

class SkPixmap;

namespace ui {

// Carries frames from the compositor thread, which paints them, to the UI
// thread, which owns the display manager connection. The queue keeps a copy
// of the latest frame and the damage accumulated since the UI thread last
// presented, so a slow presenter drops frames instead of falling behind.
class StelluxFrameQueue
    : public base::RefCountedThreadSafe<StelluxFrameQueue> {
 public:
  // |present| runs on |ui_task_runner| whenever a new frame is waiting.
  StelluxFrameQueue(scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner,
                    base::RepeatingClosure present);

  StelluxFrameQueue(const StelluxFrameQueue&) = delete;
  StelluxFrameQueue& operator=(const StelluxFrameQueue&) = delete;

  // Compositor thread. Copies the damaged part of |canvas| into the queue.
  void Submit(const SkPixmap& canvas, const gfx::Rect& damage);

  // UI thread. True when a frame arrived since the last Take(). Also
  // re-arms the notification, so the next Submit() posts |present| again.
  bool HasNewFrame();

  // UI thread. True once any frame has arrived.
  bool HasAnyFrame();

  // UI thread. Copies the latest frame into |pixels|, a buffer of |size|
  // with |stride| bytes per row, and returns the damage to report in
  // buffer coordinates. Areas the frame does not cover are cleared.
  gfx::Rect Take(uint32_t* pixels, const gfx::Size& size, size_t stride);

  // UI thread. Drops later submissions, the window is going away.
  void Close();

 private:
  friend class base::RefCountedThreadSafe<StelluxFrameQueue>;
  ~StelluxFrameQueue();

  const scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner_;
  const base::RepeatingClosure present_;

  base::Lock lock_;
  std::vector<uint32_t> pixels_ GUARDED_BY(lock_);
  gfx::Size size_ GUARDED_BY(lock_);
  gfx::Rect damage_ GUARDED_BY(lock_);
  bool has_new_frame_ GUARDED_BY(lock_) = false;
  bool present_posted_ GUARDED_BY(lock_) = false;
  bool closed_ GUARDED_BY(lock_) = false;
};

// Maps widgets to their queues, so a canvas created on the compositor thread
// can find the window it paints for.
void RegisterStelluxFrameQueue(gfx::AcceleratedWidget widget,
                               scoped_refptr<StelluxFrameQueue> queue);
void UnregisterStelluxFrameQueue(gfx::AcceleratedWidget widget);
scoped_refptr<StelluxFrameQueue> FindStelluxFrameQueue(
    gfx::AcceleratedWidget widget);

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_STELLUX_STELLUX_FRAME_QUEUE_H_

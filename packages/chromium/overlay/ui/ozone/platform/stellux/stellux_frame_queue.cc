// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/stellux/stellux_frame_queue.h"

#include <string.h>

#include <algorithm>
#include <map>
#include <utility>

#include "base/location.h"
#include "base/no_destructor.h"
#include "third_party/skia/include/core/SkPixmap.h"

namespace ui {

namespace {

constexpr size_t kBytesPerPixel = 4;

struct FrameQueueRegistry {
  base::Lock lock;
  std::map<gfx::AcceleratedWidget, scoped_refptr<StelluxFrameQueue>> queues
      GUARDED_BY(lock);
};

FrameQueueRegistry& GetRegistry() {
  static base::NoDestructor<FrameQueueRegistry> registry;
  return *registry;
}

}  // namespace

StelluxFrameQueue::StelluxFrameQueue(
    scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner,
    base::RepeatingClosure present)
    : ui_task_runner_(std::move(ui_task_runner)),
      present_(std::move(present)) {}

StelluxFrameQueue::~StelluxFrameQueue() = default;

void StelluxFrameQueue::Submit(const SkPixmap& canvas,
                               const gfx::Rect& damage) {
  const gfx::Size size(canvas.width(), canvas.height());
  gfx::Rect copy_area = damage;

  base::AutoLock hold(lock_);

  if (closed_ || size.IsEmpty()) {
    return;
  }

  if (size != size_) {
    size_ = size;
    pixels_.assign(static_cast<size_t>(size.GetArea()), 0);
    copy_area = gfx::Rect(size);
    damage_ = gfx::Rect(size);
  }

  copy_area.Intersect(gfx::Rect(size));
  for (int y = copy_area.y(); y < copy_area.bottom(); y++) {
    memcpy(&pixels_[static_cast<size_t>(y) * size.width() + copy_area.x()],
           canvas.addr32(copy_area.x(), y), copy_area.width() * kBytesPerPixel);
  }

  damage_.Union(copy_area);
  has_new_frame_ = true;
  if (!present_posted_) {
    present_posted_ = true;
    ui_task_runner_->PostTask(FROM_HERE, present_);
  }
}

bool StelluxFrameQueue::HasNewFrame() {
  base::AutoLock hold(lock_);

  present_posted_ = false;

  return has_new_frame_;
}

bool StelluxFrameQueue::HasAnyFrame() {
  base::AutoLock hold(lock_);
  return !pixels_.empty();
}

gfx::Rect StelluxFrameQueue::Take(uint32_t* pixels,
                                  const gfx::Size& size,
                                  size_t stride) {
  base::AutoLock hold(lock_);

  const int width = std::min(size.width(), size_.width());
  const int height = std::min(size.height(), size_.height());
  for (int y = 0; y < size.height(); y++) {
    auto* row = reinterpret_cast<uint8_t*>(pixels) + y * stride;
    int copied = 0;
    if (y < height) {
      memcpy(row, &pixels_[static_cast<size_t>(y) * size_.width()],
             width * kBytesPerPixel);
      copied = width;
    }

    memset(row + copied * kBytesPerPixel, 0,
           (size.width() - copied) * kBytesPerPixel);
  }

  // A buffer of another size holds nothing of the previous frame
  gfx::Rect damage = size == size_ ? damage_ : gfx::Rect(size);
  damage.Intersect(gfx::Rect(size));
  damage_ = gfx::Rect();
  has_new_frame_ = false;

  return damage.IsEmpty() ? gfx::Rect(size) : damage;
}

void StelluxFrameQueue::Close() {
  base::AutoLock hold(lock_);
  closed_ = true;
}

void RegisterStelluxFrameQueue(gfx::AcceleratedWidget widget,
                               scoped_refptr<StelluxFrameQueue> queue) {
  FrameQueueRegistry& registry = GetRegistry();
  base::AutoLock hold(registry.lock);
  registry.queues[widget] = std::move(queue);
}

void UnregisterStelluxFrameQueue(gfx::AcceleratedWidget widget) {
  FrameQueueRegistry& registry = GetRegistry();
  base::AutoLock hold(registry.lock);
  registry.queues.erase(widget);
}

scoped_refptr<StelluxFrameQueue> FindStelluxFrameQueue(
    gfx::AcceleratedWidget widget) {
  FrameQueueRegistry& registry = GetRegistry();
  base::AutoLock hold(registry.lock);
  auto it = registry.queues.find(widget);
  return it == registry.queues.end() ? nullptr : it->second;
}

}  // namespace ui

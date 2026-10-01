// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/stellux/stellux_surface_factory.h"

#include <utility>

#include "base/logging.h"
#include "skia/ext/legacy_display_globals.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "ui/gfx/vsync_provider.h"
#include "ui/ozone/platform/stellux/stellux_frame_queue.h"
#include "ui/ozone/public/surface_ozone_canvas.h"

namespace ui {

namespace {

// Paints into memory that persists between frames, since the compositor
// redraws only damaged areas, and hands finished frames to the queue. N32 is
// BGRA in memory on little endian targets, the XRGB8888 layout stlxdm expects.
class StelluxCanvasSurface : public SurfaceOzoneCanvas {
 public:
  explicit StelluxCanvasSurface(scoped_refptr<StelluxFrameQueue> queue)
      : queue_(std::move(queue)) {}

  StelluxCanvasSurface(const StelluxCanvasSurface&) = delete;
  StelluxCanvasSurface& operator=(const StelluxCanvasSurface&) = delete;

  ~StelluxCanvasSurface() override = default;

  // SurfaceOzoneCanvas:
  void ResizeCanvas(const gfx::Size& viewport_size, float scale) override {
    SkSurfaceProps props = skia::LegacyDisplayGlobals::GetSkSurfaceProps();
    surface_ = SkSurfaces::Raster(
        SkImageInfo::MakeN32Premul(viewport_size.width(),
                                   viewport_size.height()),
        &props);
  }

  SkCanvas* GetCanvas() override { return surface_->getCanvas(); }

  void PresentCanvas(const gfx::Rect& damage) override {
    SkPixmap pixmap;
    if (queue_ && surface_ && surface_->peekPixels(&pixmap)) {
      queue_->Submit(pixmap, damage);
    }
  }

  std::unique_ptr<gfx::VSyncProvider> CreateVSyncProvider() override {
    return nullptr;
  }

 private:
  const scoped_refptr<StelluxFrameQueue> queue_;
  sk_sp<SkSurface> surface_;
};

}  // namespace

StelluxSurfaceFactory::StelluxSurfaceFactory() = default;

StelluxSurfaceFactory::~StelluxSurfaceFactory() = default;

std::unique_ptr<SurfaceOzoneCanvas>
StelluxSurfaceFactory::CreateCanvasForWidget(gfx::AcceleratedWidget widget) {
  scoped_refptr<StelluxFrameQueue> queue = FindStelluxFrameQueue(widget);
  LOG_IF(ERROR, !queue) << "No stlxdm window for widget " << widget;

  return std::make_unique<StelluxCanvasSurface>(std::move(queue));
}

}  // namespace ui

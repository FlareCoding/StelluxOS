// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_STELLUX_STELLUX_SURFACE_FACTORY_H_
#define UI_OZONE_PLATFORM_STELLUX_STELLUX_SURFACE_FACTORY_H_

#include <memory>

#include "ui/ozone/public/surface_factory_ozone.h"

namespace ui {

// Software surfaces only: stlxdm composites plain pixel buffers, so there is
// no GL implementation to offer and Chromium composites on the CPU.
class StelluxSurfaceFactory : public SurfaceFactoryOzone {
 public:
  StelluxSurfaceFactory();

  StelluxSurfaceFactory(const StelluxSurfaceFactory&) = delete;
  StelluxSurfaceFactory& operator=(const StelluxSurfaceFactory&) = delete;

  ~StelluxSurfaceFactory() override;

  // SurfaceFactoryOzone:
  std::unique_ptr<SurfaceOzoneCanvas> CreateCanvasForWidget(
      gfx::AcceleratedWidget widget) override;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_STELLUX_STELLUX_SURFACE_FACTORY_H_

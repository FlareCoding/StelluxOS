// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_STELLUX_STELLUX_SCREEN_H_
#define UI_OZONE_PLATFORM_STELLUX_STELLUX_SCREEN_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "ui/display/display_list.h"
#include "ui/ozone/public/platform_screen.h"

namespace ui {

class StelluxConnection;

// The one display stlxdm drives, sized from its connection handshake.
class StelluxScreen : public PlatformScreen {
 public:
  explicit StelluxScreen(StelluxConnection* connection);

  StelluxScreen(const StelluxScreen&) = delete;
  StelluxScreen& operator=(const StelluxScreen&) = delete;

  ~StelluxScreen() override;

  // PlatformScreen:
  const std::vector<display::Display>& GetAllDisplays() const override;
  display::Display GetPrimaryDisplay() const override;
  display::Display GetDisplayForAcceleratedWidget(
      gfx::AcceleratedWidget widget) const override;
  gfx::Point GetCursorScreenPoint() const override;
  gfx::AcceleratedWidget GetAcceleratedWidgetAtScreenPoint(
      const gfx::Point& point_in_dip) const override;
  display::Display GetDisplayNearestPoint(
      const gfx::Point& point_in_dip) const override;
  display::Display GetDisplayMatching(
      const gfx::Rect& match_rect) const override;
  void AddObserver(display::DisplayObserver* observer) override;
  void RemoveObserver(display::DisplayObserver* observer) override;

 private:
  raw_ptr<StelluxConnection> connection_;
  display::DisplayList display_list_;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_STELLUX_STELLUX_SCREEN_H_

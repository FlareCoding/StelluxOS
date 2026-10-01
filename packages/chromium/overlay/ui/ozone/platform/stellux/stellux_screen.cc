// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/stellux/stellux_screen.h"

#include "ui/display/display.h"
#include "ui/ozone/platform/stellux/stellux_connection.h"

namespace ui {

namespace {

constexpr int64_t kDisplayId = 1;

// Used only if stlxdm reports no screen size
constexpr gfx::Size kFallbackSize(1280, 800);

}  // namespace

StelluxScreen::StelluxScreen(StelluxConnection* connection)
    : connection_(connection) {
  gfx::Size size = connection_->screen_size();
  if (size.IsEmpty()) {
    size = kFallbackSize;
  }

  display::Display display(kDisplayId, gfx::Rect(size));
  display_list_.AddDisplay(display, display::DisplayList::Type::PRIMARY);
}

StelluxScreen::~StelluxScreen() = default;

const std::vector<display::Display>& StelluxScreen::GetAllDisplays() const {
  return display_list_.displays();
}

display::Display StelluxScreen::GetPrimaryDisplay() const {
  return display_list_.displays().front();
}

display::Display StelluxScreen::GetDisplayForAcceleratedWidget(
    gfx::AcceleratedWidget widget) const {
  return GetPrimaryDisplay();
}

gfx::Point StelluxScreen::GetCursorScreenPoint() const {
  return connection_->cursor_screen_point();
}

gfx::AcceleratedWidget StelluxScreen::GetAcceleratedWidgetAtScreenPoint(
    const gfx::Point& point_in_dip) const {
  // stlxdm does not reveal where it placed windows
  return gfx::kNullAcceleratedWidget;
}

display::Display StelluxScreen::GetDisplayNearestPoint(
    const gfx::Point& point_in_dip) const {
  return GetPrimaryDisplay();
}

display::Display StelluxScreen::GetDisplayMatching(
    const gfx::Rect& match_rect) const {
  return GetPrimaryDisplay();
}

void StelluxScreen::AddObserver(display::DisplayObserver* observer) {
  display_list_.AddObserver(observer);
}

void StelluxScreen::RemoveObserver(display::DisplayObserver* observer) {
  display_list_.RemoveObserver(observer);
}

}  // namespace ui

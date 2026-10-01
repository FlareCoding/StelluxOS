// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_STELLUX_STELLUX_WINDOW_H_
#define UI_OZONE_PLATFORM_STELLUX_STELLUX_WINDOW_H_

#include <stlxwin/stlxwin.h>

#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "ui/events/platform/platform_event_dispatcher.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/platform_window/platform_window.h"
#include "ui/platform_window/platform_window_delegate.h"
#include "ui/platform_window/platform_window_init_properties.h"

namespace ui {

class StelluxConnection;
class StelluxFrameQueue;

// A Chromium window backed by an stlxdm toplevel or popup. stlxdm decorates
// and places toplevels itself and has no hide request, so the native window
// exists only while Chromium shows it. Frames arrive through a queue that
// the compositor fills, and are committed from the UI thread.
class StelluxWindow : public PlatformWindow, public PlatformEventDispatcher {
 public:
  StelluxWindow(PlatformWindowDelegate* delegate,
                StelluxConnection* connection,
                PlatformWindowInitProperties properties);

  StelluxWindow(const StelluxWindow&) = delete;
  StelluxWindow& operator=(const StelluxWindow&) = delete;

  ~StelluxWindow() override;

  gfx::AcceleratedWidget widget() const { return widget_; }
  gfx::AcceleratedWidget parent_widget() const { return parent_widget_; }
  stlxwin_window* native() const { return native_; }
  bool grabs_input() const { return grabs_input_; }
  bool waiting_for_buffer() const { return waiting_for_buffer_; }

  // Protocol events addressed to this window.
  void OnConfigure(const gfx::Size& size);
  void OnFocusChanged(bool focused);
  void OnCloseRequested();
  void OnDismissed();

  // Commits the newest frame, or answers a configure with the last one.
  void PresentFrame();

  // PlatformWindow:
  void Show(bool inactive) override;
  void Hide() override;
  void Close() override;
  bool IsVisible() const override;
  void PrepareForShutdown() override;
  void SetBoundsInPixels(const gfx::Rect& bounds) override;
  gfx::Rect GetBoundsInPixels() const override;
  void SetBoundsInDIP(const gfx::Rect& bounds) override;
  gfx::Rect GetBoundsInDIP() const override;
  void SetTitle(const std::u16string& title) override;
  void SetCapture() override;
  void ReleaseCapture() override;
  bool HasCapture() const override;
  void SetFullscreen(bool fullscreen, int64_t target_display_id) override;
  void Maximize() override;
  void Minimize() override;
  void Restore() override;
  PlatformWindowState GetPlatformWindowState() const override;
  void Activate() override;
  void Deactivate() override;
  void SetUseNativeFrame(bool use_native_frame) override;
  bool ShouldUseNativeFrame() const override;
  void SetCursor(scoped_refptr<PlatformCursor> cursor) override;
  void MoveCursorTo(const gfx::Point& location) override;
  void ConfineCursorToBounds(const gfx::Rect& bounds) override;
  void SetRestoredBoundsInDIP(const gfx::Rect& bounds) override;
  gfx::Rect GetRestoredBoundsInDIP() const override;
  void SetWindowIcons(const gfx::ImageSkia& window_icon,
                      const gfx::ImageSkia& app_icon) override;
  void SizeConstraintsChanged() override;

  // PlatformEventDispatcher:
  bool CanDispatchEvent(const PlatformEvent& event) override;
  uint32_t DispatchEvent(const PlatformEvent& event) override;

 private:
  void CreateNativeWindow();
  void DestroyNativeWindow();
  void ForgetNativeWindow();
  void Teardown();
  void ApplySizeConstraints();

  raw_ptr<PlatformWindowDelegate> delegate_;
  raw_ptr<StelluxConnection> connection_;
  const PlatformWindowType type_;
  const gfx::AcceleratedWidget parent_widget_;
  const bool grabs_input_;
  gfx::AcceleratedWidget widget_ = gfx::kNullAcceleratedWidget;

  // Owned C handle, freed by stlxwin_window_destroy()
  RAW_PTR_EXCLUSION stlxwin_window* native_ = nullptr;

  gfx::Rect bounds_;
  std::optional<gfx::Rect> restored_bounds_;
  std::string title_;
  stlxwin_cursor cursor_ = STLXWIN_CURSOR_ARROW;
  bool visible_ = false;
  bool closed_ = false;
  bool has_capture_ = false;
  bool needs_ack_ = false;
  bool waiting_for_buffer_ = false;

  scoped_refptr<StelluxFrameQueue> frame_queue_;

  base::WeakPtrFactory<StelluxWindow> weak_factory_{this};
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_STELLUX_STELLUX_WINDOW_H_

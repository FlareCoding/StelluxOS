// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_STELLUX_STELLUX_CONNECTION_H_
#define UI_OZONE_PLATFORM_STELLUX_STELLUX_CONNECTION_H_

#include <stdint.h>
#include <stlxwin/stlxwin.h>

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_pump_for_ui.h"
#include "base/time/time.h"
#include "ui/events/platform/platform_event_source.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_ui_types.h"

namespace ui {

class StelluxWindow;

// The UI thread's connection to stlxdm. It is the process's platform event
// source: it watches the connection fd on the UI message pump, drains
// protocol events, and turns them into ui::Events for the window they name.
class StelluxConnection : public PlatformEventSource,
                          public base::MessagePumpForUI::FdWatcher {
 public:
  // Returns null when no display manager is serving.
  static std::unique_ptr<StelluxConnection> Connect();

  StelluxConnection(const StelluxConnection&) = delete;
  StelluxConnection& operator=(const StelluxConnection&) = delete;

  ~StelluxConnection() override;

  stlxwin_conn* native() const { return conn_; }
  gfx::Size screen_size() const;
  gfx::Point cursor_screen_point() const { return cursor_; }

  // Windows register while they exist, and get their widget ids here.
  gfx::AcceleratedWidget AddWindow(StelluxWindow* window);
  void RemoveWindow(StelluxWindow* window);
  StelluxWindow* FindWindow(gfx::AcceleratedWidget widget) const;
  std::vector<StelluxWindow*> FindChildWindows(
      gfx::AcceleratedWidget parent) const;

  // Beginning a frame reads the connection to learn of released buffers, so
  // the events read along with them are dispatched from a task of their own.
  void ScheduleEventDispatch();

  // The window that owns the event currently being dispatched.
  StelluxWindow* event_target() const { return event_target_; }

 private:
  explicit StelluxConnection(stlxwin_conn* conn);

  // base::MessagePumpForUI::FdWatcher:
  void OnFileCanReadWithoutBlocking(int fd) override;
  void OnFileCanWriteWithoutBlocking(int fd) override;

  void DispatchEvents();
  void HandleEvent(const stlxwin_event& event);
  void HandleKey(StelluxWindow* window, const stlxwin_event& event);
  void HandlePointer(StelluxWindow* window, const stlxwin_event& event);
  void DismissPopups(StelluxWindow* parent);
  void CloseAllWindows();
  void DispatchToWindow(StelluxWindow* window, Event* event);
  StelluxWindow* FindWindowForNative(const stlxwin_window* native) const;

  // Owned C handle, freed by stlxwin_disconnect() outside PartitionAlloc's view
  RAW_PTR_EXCLUSION stlxwin_conn* conn_ = nullptr;
  base::MessagePumpForUI::FdWatchController watch_controller_{FROM_HERE};
  std::vector<raw_ptr<StelluxWindow>> windows_;
  gfx::AcceleratedWidget next_widget_ = 1;
  raw_ptr<StelluxWindow> event_target_ = nullptr;
  bool dispatch_scheduled_ = false;

  // Input state the protocol leaves to clients
  int modifier_flags_ = 0;
  int pressed_buttons_ = 0;
  gfx::Point cursor_;
  base::TimeTicks last_press_time_;
  gfx::Point last_press_location_;
  int last_press_button_ = 0;
  int click_count_ = 0;

  base::WeakPtrFactory<StelluxConnection> weak_factory_{this};
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_STELLUX_STELLUX_CONNECTION_H_

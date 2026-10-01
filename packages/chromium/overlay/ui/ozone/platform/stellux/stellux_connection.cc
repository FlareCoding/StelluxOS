// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/stellux/stellux_connection.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/notreached.h"
#include "base/task/current_thread.h"
#include "base/task/single_thread_task_runner.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/event_utils.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/ozone/platform/stellux/stellux_window.h"

namespace ui {

namespace {

constexpr char kAppId[] = "chromium";

// Key events carry HID usages from the keyboard page
constexpr uint32_t kUsbKeyboardPage = 0x070000;

// Printable characters start after the C0 control range and skip DEL
constexpr uint32_t kFirstPrintable = 0x20;
constexpr uint32_t kDelete = 0x7f;

// Presses this close in time and space count as one multi-click
constexpr base::TimeDelta kMultiClickTime = base::Milliseconds(500);
constexpr int kMultiClickDistance = 4;
constexpr int kMaxClickCount = 3;

int ModifierFlags(uint8_t modifiers) {
  int flags = 0;
  if (modifiers & STLXWIN_MOD_SHIFT) {
    flags |= EF_SHIFT_DOWN;
  }

  if (modifiers & STLXWIN_MOD_CTRL) {
    flags |= EF_CONTROL_DOWN;
  }

  if (modifiers & STLXWIN_MOD_ALT) {
    flags |= EF_ALT_DOWN;
  }

  if (modifiers & STLXWIN_MOD_SUPER) {
    flags |= EF_COMMAND_DOWN;
  }

  return flags;
}

int ButtonFlag(uint8_t button) {
  switch (button) {
    case STLXWIN_BTN_LEFT:
      return EF_LEFT_MOUSE_BUTTON;
    case STLXWIN_BTN_RIGHT:
      return EF_RIGHT_MOUSE_BUTTON;
    case STLXWIN_BTN_MIDDLE:
      return EF_MIDDLE_MOUSE_BUTTON;
    default:
      return 0;
  }
}

}  // namespace

// static
std::unique_ptr<StelluxConnection> StelluxConnection::Connect() {
  stlxwin_conn* conn = stlxwin_connect(kAppId);
  if (!conn) {
    return nullptr;
  }

  return base::WrapUnique(new StelluxConnection(conn));
}

StelluxConnection::StelluxConnection(stlxwin_conn* conn) : conn_(conn) {
  base::CurrentUIThread::Get()->WatchFileDescriptor(
      stlxwin_conn_fd(conn_), /*persistent=*/true,
      base::MessagePumpForUI::WATCH_READ, &watch_controller_, this);
}

StelluxConnection::~StelluxConnection() {
  watch_controller_.StopWatchingFileDescriptor();
  stlxwin_disconnect(conn_);
}

gfx::Size StelluxConnection::screen_size() const {
  uint32_t width = 0;
  uint32_t height = 0;
  stlxwin_screen_size(conn_, &width, &height);

  return gfx::Size(width, height);
}

gfx::AcceleratedWidget StelluxConnection::AddWindow(StelluxWindow* window) {
  windows_.push_back(window);

  return next_widget_++;
}

void StelluxConnection::RemoveWindow(StelluxWindow* window) {
  std::erase(windows_, window);
  if (event_target_ == window) {
    event_target_ = nullptr;
  }
}

StelluxWindow* StelluxConnection::FindWindow(
    gfx::AcceleratedWidget widget) const {
  for (StelluxWindow* window : windows_) {
    if (window->widget() == widget) {
      return window;
    }
  }

  return nullptr;
}

std::vector<StelluxWindow*> StelluxConnection::FindChildWindows(
    gfx::AcceleratedWidget parent) const {
  std::vector<StelluxWindow*> children;
  for (StelluxWindow* window : windows_) {
    if (window->parent_widget() == parent) {
      children.push_back(window);
    }
  }

  return children;
}

void StelluxConnection::ScheduleEventDispatch() {
  if (dispatch_scheduled_) {
    return;
  }

  dispatch_scheduled_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&StelluxConnection::DispatchEvents,
                                weak_factory_.GetWeakPtr()));
}

StelluxWindow* StelluxConnection::FindWindowForNative(
    const stlxwin_window* native) const {
  for (StelluxWindow* window : windows_) {
    if (native && window->native() == native) {
      return window;
    }
  }

  return nullptr;
}

void StelluxConnection::OnFileCanReadWithoutBlocking(int fd) {
  DispatchEvents();
}

void StelluxConnection::OnFileCanWriteWithoutBlocking(int fd) {
  NOTREACHED();
}

void StelluxConnection::DispatchEvents() {
  dispatch_scheduled_ = false;
  stlxwin_dispatch(conn_);

  stlxwin_event event;
  while (stlxwin_next_event(conn_, &event)) {
    HandleEvent(event);
  }

  // The drained messages may have released buffers, so frames that found
  // every buffer busy retry, looked up again since events may close windows
  std::vector<gfx::AcceleratedWidget> waiting;
  for (StelluxWindow* window : windows_) {
    if (window->waiting_for_buffer()) {
      waiting.push_back(window->widget());
    }
  }

  for (gfx::AcceleratedWidget widget : waiting) {
    if (StelluxWindow* window = FindWindow(widget)) {
      window->PresentFrame();
    }
  }
}

void StelluxConnection::HandleEvent(const stlxwin_event& event) {
  if (event.type == STLXWIN_EVT_DISCONNECTED) {
    LOG(ERROR) << "Lost the connection to stlxdm";
    watch_controller_.StopWatchingFileDescriptor();
    CloseAllWindows();
    return;
  }

  StelluxWindow* window = FindWindowForNative(event.window);
  if (!window) {
    return;
  }

  switch (event.type) {
    case STLXWIN_EVT_KEY_DOWN:
    case STLXWIN_EVT_KEY_UP:
    case STLXWIN_EVT_KEY_REPEAT:
      HandleKey(window, event);
      break;
    case STLXWIN_EVT_POINTER_MOTION:
    case STLXWIN_EVT_BUTTON_DOWN:
    case STLXWIN_EVT_BUTTON_UP:
    case STLXWIN_EVT_SCROLL:
    case STLXWIN_EVT_POINTER_ENTER:
    case STLXWIN_EVT_POINTER_LEAVE:
      HandlePointer(window, event);
      break;
    case STLXWIN_EVT_FOCUS_IN:
      window->OnFocusChanged(true);
      break;
    case STLXWIN_EVT_FOCUS_OUT:
      window->OnFocusChanged(false);
      break;
    case STLXWIN_EVT_CLOSE:
      window->OnCloseRequested();
      break;
    case STLXWIN_EVT_CONFIGURE:
      window->OnConfigure(
          gfx::Size(event.configure.width, event.configure.height));
      break;
    case STLXWIN_EVT_POPUP_DISMISSED:
      DismissPopups(window);
      break;
    default:
      break;
  }
}

void StelluxConnection::HandleKey(StelluxWindow* window,
                                  const stlxwin_event& event) {
  modifier_flags_ = ModifierFlags(event.key.modifiers);
  int flags = modifier_flags_;
  if (event.type == STLXWIN_EVT_KEY_REPEAT) {
    flags |= EF_IS_REPEAT;
  }

  DomCode code =
      KeycodeConverter::UsbKeycodeToDomCode(kUsbKeyboardPage | event.key.usage);
  DomKey key;
  KeyboardCode key_code;
  if (!DomCodeToUsLayoutDomKey(code, flags, &key, &key_code)) {
    key = DomKey::UNIDENTIFIED;
    key_code = VKEY_UNKNOWN;
  }

  // stlxdm translates characters with the active keymap, so its character
  // wins over the US layout guess for anything printable
  const uint32_t ch = event.key.ch;
  if (ch >= kFirstPrintable && ch != kDelete) {
    key = DomKey::FromCharacter(ch);
  }

  EventType type = event.type == STLXWIN_EVT_KEY_UP ? EventType::kKeyReleased
                                                    : EventType::kKeyPressed;
  KeyEvent key_event(type, key_code, code, flags, key, EventTimeForNow());
  DispatchToWindow(window, &key_event);
}

void StelluxConnection::HandlePointer(StelluxWindow* window,
                                      const stlxwin_event& event) {
  gfx::Point location;
  switch (event.type) {
    case STLXWIN_EVT_BUTTON_DOWN:
    case STLXWIN_EVT_BUTTON_UP:
      location = gfx::Point(event.button.x, event.button.y);
      break;
    case STLXWIN_EVT_SCROLL:
      location = gfx::Point(event.scroll.x, event.scroll.y);
      break;
    default:
      location = gfx::Point(event.motion.x, event.motion.y);
      break;
  }

  const gfx::Point root =
      location + window->GetBoundsInPixels().OffsetFromOrigin();
  const base::TimeTicks now = EventTimeForNow();
  cursor_ = root;

  switch (event.type) {
    case STLXWIN_EVT_POINTER_MOTION: {
      EventType type =
          pressed_buttons_ ? EventType::kMouseDragged : EventType::kMouseMoved;
      MouseEvent mouse(type, gfx::PointF(location), gfx::PointF(root), now,
                       modifier_flags_ | pressed_buttons_, 0);
      DispatchToWindow(window, &mouse);
      break;
    }
    case STLXWIN_EVT_BUTTON_DOWN: {
      const int changed = ButtonFlag(event.button.button);
      const bool repeat =
          changed == last_press_button_ &&
          now - last_press_time_ < kMultiClickTime &&
          (location - last_press_location_).LengthSquared() <=
              kMultiClickDistance * kMultiClickDistance;
      click_count_ = repeat ? std::min(click_count_ + 1, kMaxClickCount) : 1;
      last_press_time_ = now;
      last_press_location_ = location;
      last_press_button_ = changed;
      pressed_buttons_ |= changed;

      MouseEvent mouse(EventType::kMousePressed, gfx::PointF(location),
                       gfx::PointF(root), now,
                       modifier_flags_ | pressed_buttons_, changed);
      mouse.SetClickCount(click_count_);
      DispatchToWindow(window, &mouse);
      break;
    }
    case STLXWIN_EVT_BUTTON_UP: {
      // A release still reports its own button among the flags
      const int changed = ButtonFlag(event.button.button);
      MouseEvent mouse(EventType::kMouseReleased, gfx::PointF(location),
                       gfx::PointF(root), now,
                       modifier_flags_ | pressed_buttons_ | changed, changed);
      mouse.SetClickCount(click_count_);
      pressed_buttons_ &= ~changed;
      DispatchToWindow(window, &mouse);
      break;
    }
    case STLXWIN_EVT_SCROLL: {
      // Positive detents scroll toward the top, as a positive wheel delta does
      MouseWheelEvent wheel(
          gfx::Vector2d(0, event.scroll.dy * MouseWheelEvent::kWheelDelta),
          location, root, now, modifier_flags_ | pressed_buttons_, 0);
      DispatchToWindow(window, &wheel);
      break;
    }
    case STLXWIN_EVT_POINTER_ENTER:
    case STLXWIN_EVT_POINTER_LEAVE: {
      EventType type = event.type == STLXWIN_EVT_POINTER_ENTER
                           ? EventType::kMouseEntered
                           : EventType::kMouseExited;
      MouseEvent mouse(type, gfx::PointF(location), gfx::PointF(root), now,
                       modifier_flags_ | pressed_buttons_, 0);
      DispatchToWindow(window, &mouse);
      break;
    }
    default:
      break;
  }
}

void StelluxConnection::DismissPopups(StelluxWindow* parent) {
  std::vector<gfx::AcceleratedWidget> dismissed;
  for (StelluxWindow* window : windows_) {
    if (window->grabs_input() && window->parent_widget() == parent->widget()) {
      dismissed.push_back(window->widget());
    }
  }

  for (gfx::AcceleratedWidget widget : dismissed) {
    if (StelluxWindow* window = FindWindow(widget)) {
      window->OnDismissed();
    }
  }
}

void StelluxConnection::CloseAllWindows() {
  std::vector<gfx::AcceleratedWidget> widgets;
  for (StelluxWindow* window : windows_) {
    widgets.push_back(window->widget());
  }

  for (gfx::AcceleratedWidget widget : widgets) {
    if (StelluxWindow* window = FindWindow(widget)) {
      window->OnCloseRequested();
    }
  }
}

void StelluxConnection::DispatchToWindow(StelluxWindow* window, Event* event) {
  event_target_ = window;
  DispatchEvent(event);
  event_target_ = nullptr;
}

}  // namespace ui

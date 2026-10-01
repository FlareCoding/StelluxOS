// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/stellux/stellux_window.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/notimplemented.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "ui/base/cursor/platform_cursor.h"
#include "ui/events/event.h"
#include "ui/events/ozone/events_ozone.h"
#include "ui/events/platform/platform_event_source.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/ozone/common/bitmap_cursor.h"
#include "ui/ozone/platform/stellux/stellux_connection.h"
#include "ui/ozone/platform/stellux/stellux_frame_queue.h"

namespace ui {

namespace {

// Chromium sometimes creates a window before deciding its size
constexpr gfx::Size kDefaultSize(800, 600);

bool IsPopupType(PlatformWindowType type) {
  return type == PlatformWindowType::kPopup ||
         type == PlatformWindowType::kMenu ||
         type == PlatformWindowType::kTooltip ||
         type == PlatformWindowType::kBubble;
}

stlxwin_cursor CursorFor(mojom::CursorType type) {
  switch (type) {
    case mojom::CursorType::kIBeam:
    case mojom::CursorType::kVerticalText:
      return STLXWIN_CURSOR_IBEAM;
    case mojom::CursorType::kHand:
      return STLXWIN_CURSOR_HAND;
    case mojom::CursorType::kEastResize:
    case mojom::CursorType::kWestResize:
    case mojom::CursorType::kEastWestResize:
    case mojom::CursorType::kColumnResize:
      return STLXWIN_CURSOR_RESIZE_H;
    case mojom::CursorType::kNorthResize:
    case mojom::CursorType::kSouthResize:
    case mojom::CursorType::kNorthSouthResize:
    case mojom::CursorType::kRowResize:
      return STLXWIN_CURSOR_RESIZE_V;
    case mojom::CursorType::kNorthWestResize:
    case mojom::CursorType::kSouthEastResize:
    case mojom::CursorType::kNorthWestSouthEastResize:
      return STLXWIN_CURSOR_RESIZE_NWSE;
    case mojom::CursorType::kNorthEastResize:
    case mojom::CursorType::kSouthWestResize:
    case mojom::CursorType::kNorthEastSouthWestResize:
      return STLXWIN_CURSOR_RESIZE_NESW;
    case mojom::CursorType::kNone:
      return STLXWIN_CURSOR_NONE;
    default:
      return STLXWIN_CURSOR_ARROW;
  }
}

}  // namespace

StelluxWindow::StelluxWindow(PlatformWindowDelegate* delegate,
                             StelluxConnection* connection,
                             PlatformWindowInitProperties properties)
    : delegate_(delegate),
      connection_(connection),
      type_(properties.type),
      parent_widget_(properties.parent_widget),
      grabs_input_(properties.type == PlatformWindowType::kMenu),
      bounds_(properties.bounds) {
  if (bounds_.IsEmpty()) {
    bounds_.set_size(kDefaultSize);
  }

  widget_ = connection_->AddWindow(this);
  frame_queue_ = base::MakeRefCounted<StelluxFrameQueue>(
      base::SingleThreadTaskRunner::GetCurrentDefault(),
      base::BindRepeating(&StelluxWindow::PresentFrame,
                          weak_factory_.GetWeakPtr()));
  RegisterStelluxFrameQueue(widget_, frame_queue_);
  PlatformEventSource::GetInstance()->AddPlatformEventDispatcher(this);

  delegate_->OnAcceleratedWidgetAvailable(widget_);
}

StelluxWindow::~StelluxWindow() {
  Teardown();
}

void StelluxWindow::OnConfigure(const gfx::Size& size) {
  // Any configure obliges a commit that acknowledges it
  needs_ack_ = true;
  if (size != bounds_.size() && !size.IsEmpty()) {
    bounds_.set_size(size);
    delegate_->OnBoundsChanged({/*origin changed=*/false});
  }

  PresentFrame();
}

void StelluxWindow::OnFocusChanged(bool focused) {
  delegate_->OnActivationChanged(focused);
}

void StelluxWindow::OnCloseRequested() {
  delegate_->OnCloseRequest();
}

void StelluxWindow::OnDismissed() {
  // stlxdm has destroyed a dismissed popup already, so this frees the
  // client side before a frame could be committed to a window that is gone
  DestroyNativeWindow();
  delegate_->OnCloseRequest();
}

void StelluxWindow::PresentFrame() {
  const bool has_new_frame = frame_queue_->HasNewFrame();
  const bool can_ack = needs_ack_ && frame_queue_->HasAnyFrame();
  if (!native_ || (!has_new_frame && !can_ack)) {
    waiting_for_buffer_ = false;
    return;
  }

  stlxwin_buffer* buffer = stlxwin_try_begin_frame(native_);
  connection_->ScheduleEventDispatch();
  if (!buffer) {
    waiting_for_buffer_ = true;
    return;
  }

  waiting_for_buffer_ = false;
  needs_ack_ = false;

  const gfx::Rect damage = frame_queue_->Take(
      buffer->pixels, gfx::Size(buffer->width, buffer->height),
      buffer->stride);
  const stlxwin_rect rect = {damage.x(), damage.y(), damage.width(),
                             damage.height()};
  if (stlxwin_commit(native_, buffer, &rect, 1, 0) < 0) {
    LOG(ERROR) << "stlxdm refused a frame for widget " << widget_;
  }
}

void StelluxWindow::Show(bool inactive) {
  if (visible_) {
    return;
  }

  visible_ = true;
  CreateNativeWindow();
}

void StelluxWindow::Hide() {
  if (!visible_) {
    return;
  }

  visible_ = false;
  DestroyNativeWindow();
}

void StelluxWindow::Close() {
  if (closed_) {
    return;
  }

  Teardown();
  delegate_->OnClosed();
}

bool StelluxWindow::IsVisible() const {
  return visible_;
}

void StelluxWindow::PrepareForShutdown() {}

void StelluxWindow::SetBoundsInPixels(const gfx::Rect& bounds) {
  // The display manager places and sizes toplevels, so a shown toplevel
  // keeps its bounds until the next configure
  if (native_ && !IsPopupType(type_)) {
    delegate_->OnBoundsChanged({/*origin_changed=*/false});
    return;
  }

  const bool origin_changed = bounds.origin() != bounds_.origin();
  const bool moved_popup = native_ && bounds != bounds_;
  bounds_ = bounds;

  // Popups cannot move once created, so a moved popup is created anew
  if (moved_popup) {
    DestroyNativeWindow();
    CreateNativeWindow();
  }

  delegate_->OnBoundsChanged({origin_changed});
}

gfx::Rect StelluxWindow::GetBoundsInPixels() const {
  return bounds_;
}

void StelluxWindow::SetBoundsInDIP(const gfx::Rect& bounds) {
  SetBoundsInPixels(delegate_->ConvertRectToPixels(bounds));
}

gfx::Rect StelluxWindow::GetBoundsInDIP() const {
  return delegate_->ConvertRectToDIP(bounds_);
}

void StelluxWindow::SetTitle(const std::u16string& title) {
  title_ = base::UTF16ToUTF8(title);
  if (native_) {
    stlxwin_window_set_title(native_, title_.c_str());
  }
}

void StelluxWindow::SetCapture() {
  has_capture_ = true;
}

void StelluxWindow::ReleaseCapture() {
  has_capture_ = false;
}

bool StelluxWindow::HasCapture() const {
  return has_capture_;
}

void StelluxWindow::SetFullscreen(bool fullscreen, int64_t target_display_id) {
  if (!native_ || IsPopupType(type_)) {
    return;
  }

  stlxwin_window_set_fullscreen(native_, fullscreen ? 1 : 0);
}

void StelluxWindow::Maximize() {
  NOTIMPLEMENTED_LOG_ONCE();
}

void StelluxWindow::Minimize() {
  NOTIMPLEMENTED_LOG_ONCE();
}

void StelluxWindow::Restore() {
  SetFullscreen(false, /*target_display_id=*/-1);
}

PlatformWindowState StelluxWindow::GetPlatformWindowState() const {
  // The protocol reports no window state back, so a window is always normal
  return PlatformWindowState::kNormal;
}

void StelluxWindow::Activate() {}

void StelluxWindow::Deactivate() {}

void StelluxWindow::SetUseNativeFrame(bool use_native_frame) {}

bool StelluxWindow::ShouldUseNativeFrame() const {
  // stlxdm draws the title bar and borders of every toplevel
  return true;
}

void StelluxWindow::SetCursor(scoped_refptr<PlatformCursor> cursor) {
  scoped_refptr<BitmapCursor> bitmap =
      BitmapCursor::FromPlatformCursor(std::move(cursor));
  cursor_ = bitmap ? CursorFor(bitmap->type()) : STLXWIN_CURSOR_ARROW;
  if (native_) {
    stlxwin_window_set_cursor(native_, cursor_);
  }
}

void StelluxWindow::MoveCursorTo(const gfx::Point& location) {}

void StelluxWindow::ConfineCursorToBounds(const gfx::Rect& bounds) {}

void StelluxWindow::SetRestoredBoundsInDIP(const gfx::Rect& bounds) {
  restored_bounds_ = delegate_->ConvertRectToPixels(bounds);
}

gfx::Rect StelluxWindow::GetRestoredBoundsInDIP() const {
  return delegate_->ConvertRectToDIP(restored_bounds_.value_or(bounds_));
}

void StelluxWindow::SetWindowIcons(const gfx::ImageSkia& window_icon,
                                   const gfx::ImageSkia& app_icon) {}

void StelluxWindow::SizeConstraintsChanged() {
  ApplySizeConstraints();
}

bool StelluxWindow::CanDispatchEvent(const PlatformEvent& event) {
  return connection_->event_target() == this;
}

uint32_t StelluxWindow::DispatchEvent(const PlatformEvent& event) {
  DispatchEventFromNativeUiEvent(
      event, base::BindOnce(&PlatformWindowDelegate::DispatchEvent,
                            base::Unretained(delegate_.get())));

  return POST_DISPATCH_STOP_PROPAGATION;
}

void StelluxWindow::CreateNativeWindow() {
  if (native_ || closed_) {
    return;
  }

  stlxwin_window* native = nullptr;
  StelluxWindow* parent = connection_->FindWindow(parent_widget_);
  if (IsPopupType(type_) && parent && parent->native()) {
    // Popups sit in parent content coordinates, and both bounds share the
    // screen coordinates Chromium assigned
    const gfx::Vector2d offset =
        bounds_.origin() - parent->GetBoundsInPixels().origin();
    native = stlxwin_popup_create(parent->native(), offset.x(), offset.y(),
                                  bounds_.width(), bounds_.height(),
                                  grabs_input_ ? STLXWIN_PF_GRAB : 0);
  } else if (!IsPopupType(type_)) {
    native = stlxwin_window_create(connection_->native(), bounds_.width(),
                                   bounds_.height(), title_.c_str(),
                                   STLXWIN_WF_RESIZABLE);
  }

  if (!native) {
    LOG(ERROR) << "stlxdm could not create a window for widget " << widget_;
    return;
  }

  native_ = native;
  stlxwin_window_set_cursor(native_, cursor_);
  ApplySizeConstraints();

  // Popups shown while their parent had no native window appear with it
  for (StelluxWindow* child : connection_->FindChildWindows(widget_)) {
    if (child->visible_) {
      child->CreateNativeWindow();
    }
  }

  // A window appears with its first commit, so repeat the latest frame
  needs_ack_ = true;
  PresentFrame();
}

void StelluxWindow::DestroyNativeWindow() {
  if (!native_) {
    return;
  }

  // stlxwin destroys a window's popups along with it
  for (StelluxWindow* child : connection_->FindChildWindows(widget_)) {
    child->ForgetNativeWindow();
  }

  stlxwin_window_destroy(native_);
  native_ = nullptr;
  waiting_for_buffer_ = false;
}

void StelluxWindow::ForgetNativeWindow() {
  if (!native_) {
    return;
  }

  for (StelluxWindow* child : connection_->FindChildWindows(widget_)) {
    child->ForgetNativeWindow();
  }

  native_ = nullptr;
  waiting_for_buffer_ = false;
}

void StelluxWindow::ApplySizeConstraints() {
  if (!native_ || IsPopupType(type_)) {
    return;
  }

  if (std::optional<gfx::Size> min = delegate_->GetMinimumSizeForWindow()) {
    stlxwin_window_set_min_size(native_, min->width(), min->height());
  }

  if (std::optional<gfx::Size> max = delegate_->GetMaximumSizeForWindow()) {
    stlxwin_window_set_max_size(native_, max->width(), max->height());
  }
}

void StelluxWindow::Teardown() {
  if (closed_) {
    return;
  }

  closed_ = true;
  DestroyNativeWindow();
  frame_queue_->Close();
  UnregisterStelluxFrameQueue(widget_);
  PlatformEventSource::GetInstance()->RemovePlatformEventDispatcher(this);
  connection_->RemoveWindow(this);
}

}  // namespace ui

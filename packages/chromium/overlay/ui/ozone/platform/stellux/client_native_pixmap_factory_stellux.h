// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_STELLUX_CLIENT_NATIVE_PIXMAP_FACTORY_STELLUX_H_
#define UI_OZONE_PLATFORM_STELLUX_CLIENT_NATIVE_PIXMAP_FACTORY_STELLUX_H_

namespace gfx {
class ClientNativePixmapFactory;
}

namespace ui {

// stlxdm shares no GPU buffers, so no native pixmaps exist
gfx::ClientNativePixmapFactory* CreateClientNativePixmapFactoryStellux();

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_STELLUX_CLIENT_NATIVE_PIXMAP_FACTORY_STELLUX_H_

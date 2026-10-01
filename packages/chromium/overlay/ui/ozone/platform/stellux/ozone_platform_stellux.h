// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_STELLUX_OZONE_PLATFORM_STELLUX_H_
#define UI_OZONE_PLATFORM_STELLUX_OZONE_PLATFORM_STELLUX_H_

namespace ui {

class OzonePlatform;

// Constructor hook for use in ozone_platform_list.cc
OzonePlatform* CreateOzonePlatformStellux();

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_STELLUX_OZONE_PLATFORM_STELLUX_H_

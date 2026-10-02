// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/file_path.h"
#include "base/notimplemented.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/platform_util_internal.h"
#include "url/gurl.h"

// Files, folders, and external URLs go to other applications through D-Bus
// portals and xdg-open, which builds without D-Bus do not have.
namespace platform_util {

namespace internal {

void PlatformOpenVerifiedItem(const base::FilePath& path, OpenItemType type) {
  NOTIMPLEMENTED_LOG_ONCE();
}

}  // namespace internal

void ShowItemInFolder(Profile* profile, const base::FilePath& full_path) {
  NOTIMPLEMENTED_LOG_ONCE();
}

void OpenExternal(const GURL& url) {
  NOTIMPLEMENTED_LOG_ONCE();
}

}  // namespace platform_util

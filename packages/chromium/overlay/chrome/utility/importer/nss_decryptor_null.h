// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_UTILITY_IMPORTER_NSS_DECRYPTOR_NULL_H_
#define CHROME_UTILITY_IMPORTER_NSS_DECRYPTOR_NULL_H_

#include <vector>

namespace user_data_importer {
struct ImportedPasswordForm;
}

namespace base {
class FilePath;
}

// The decryptor of builds without NSS. It never initializes, so the Firefox
// importer brings over everything except saved passwords.
class NSSDecryptor {
 public:
  NSSDecryptor() = default;

  NSSDecryptor(const NSSDecryptor&) = delete;
  NSSDecryptor& operator=(const NSSDecryptor&) = delete;

  ~NSSDecryptor() = default;

  bool Init(const base::FilePath& dll_path, const base::FilePath& db_path) {
    return false;
  }

  bool ReadAndParseLogins(
      const base::FilePath& json_file,
      std::vector<user_data_importer::ImportedPasswordForm>* forms) {
    return false;
  }
};

#endif  // CHROME_UTILITY_IMPORTER_NSS_DECRYPTOR_NULL_H_

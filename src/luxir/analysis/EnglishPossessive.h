// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>

namespace luxir {

// Lucene EnglishPossessiveFilter semantics: remove one trailing apostrophe +
// s/S, accepting U+0027, U+2019 and U+FF07. A bare trailing apostrophe stays.
// Only shorten the borrowed view; source bytes and token offsets stay intact.
inline std::string_view removeEnglishPossessive(std::string_view term) {
  size_t size = term.size();
  if (size < 2 || (term.back() != 's' && term.back() != 'S')) return term;
  if (term[size - 2] == '\'') return term.substr(0, size - 2);
  if (size >= 4) {
    auto apostrophe = term.substr(size - 4, 3);
    if (apostrophe == "\xe2\x80\x99" || apostrophe == "\xef\xbc\x87") {
      return term.substr(0, size - 4);
    }
  }
  return term;
}

} // namespace luxir

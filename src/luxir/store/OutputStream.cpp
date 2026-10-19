// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/OutputStream.h"

namespace luxir {

bool File::isRelocatable(const OutputStream& os) const {
  return !os.hasFlushedBytes();
}

void File::appendRelocatableTo(OutputStream& source,
                               OutputStream& target) {
  assert(isRelocatable(source));
  target.write(source.start, source.buffered());
  source.pos = source.start;
}

} // namespace luxir

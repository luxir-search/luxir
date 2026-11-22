// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "OwnershipShape.h"
#include "SegmentCore.h"

#include <stdexcept>

namespace luxir {
OwnershipShape::OwnershipShape(SegmentCore& core, const OwnershipDescriptor& descriptor,
                               int32_t parentCount, int32_t childCount)
    : nParents(parentCount), nChildren(childCount), ends(core, descriptor.ends, parentCount) {
  SegmentReadContext endsContext{&core, PackedTerm(nullptr), descriptor.ends.location.filenum()};
  auto firstEnd = nParents ? ends.checkedValueAt(0, endsContext) : 0;
  if (nParents == 0 ? nChildren != 0
      : firstEnd < 0 || firstEnd > nChildren || ends.checkedValueAt(nParents - 1, endsContext) != nChildren) {
    endsContext.invalid("ownership ends do not cover the child space");
  }
  if (descriptor.owners) {
    owners = MonoReader(core, descriptor.owners, childCount);
    if (nChildren > 0) {
      SegmentReadContext ownersContext{&core, PackedTerm(nullptr), descriptor.owners.location.filenum()};
      auto first = owners.checkedValueAt(0, ownersContext);
      auto last = owners.checkedValueAt(nChildren - 1, ownersContext);
      if (first < 0 || first >= nParents || last < 0 || last >= nParents) {
        ownersContext.invalid("ownership reverse map exceeds the parent space");
      }
    }
  }
}

} // namespace luxir

// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "luxir/reader/NestedBlocks.h"

#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/util/MemPool.h"

namespace luxir {

std::shared_ptr<const NestedBlocks> NestedBlocks::build(PostingsReader& reader) {
  const auto* info = reader.fieldInfo(PATH_FIELD);
  if (info == nullptr || info->nTerms == 0) return nullptr;
  auto blocks = std::make_shared<NestedBlocks>(reader.maxDoc());
  auto guard = MemPool::threadLocalPoolGuard();
  TermsEnum terms(guard.pool(), reader, *info);
  while (terms.nextTerm()) {
    auto& path = blocks->paths.try_emplace(std::string((std::string_view)terms.term()), reader.maxDoc()).first->second;
    DocsOnlyEnum docs(terms);
    for (int32_t doc = docs.next(); doc != DocsEnumMeta::END; doc = docs.next()) {
      path.set(doc);
      blocks->children.set(doc);
    }
  }
  return blocks;
}

}  // namespace luxir

namespace luxir {

std::shared_ptr<const NestedBlocks> PostingsReader::nestedBlocks() {
  std::call_once(nestedBlocksOnce, [this] { nestedBlocksValue = NestedBlocks::build(*this); });
  return nestedBlocksValue;
}

}  // namespace luxir

// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/ParseView.h"
#include "luxir/search/SearchRequest.h"

namespace luxir {

ParseView::ParseView(Schema& schema, std::vector<std::string>* explanations,
                    const std::shared_ptr<IndexReader>* reader, SearchRequest* request)
    : fields(schema, explanations, reader ? (*reader)->path() : std::string_view{}), request(request), current(reader) {}

ParseView::Scope::Scope(ParseView& view, std::string_view target)
    : view(view), previous(view.current), path(view.fields.path()) {
  auto* reader = view.request ? &view.request->pinView(target) : nullptr;
  view.enter(reader, target);
}

} // namespace luxir

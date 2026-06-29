#include "solux_descriptors.h"

#include "solux_fds_data.h" // generated: solux::detail::kSoluxFds / kSoluxFdsLen

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

#include <string>
#include <vector>

namespace solux {

void registerSoluxDescriptors() {
  google::protobuf::FileDescriptorSet fds;
  if (!fds.ParseFromArray(detail::kSoluxFds, static_cast<int>(detail::kSoluxFdsLen))) {
    return;
  }
  // InternalAddGeneratedFile keeps the pointer for lazy parsing, so the encoded
  // bytes must outlive use: stash them in a function-local static.
  static std::vector<std::string> kept;
  for (const auto &file : fds.file()) {
    if (google::protobuf::DescriptorPool::generated_pool()->FindFileByName(file.name()) != nullptr) {
      continue; // well-known types (and re-entrant calls) already registered
    }
    kept.emplace_back();
    file.SerializeToString(&kept.back());
    google::protobuf::DescriptorPool::InternalAddGeneratedFile(kept.back().data(),
                                                               static_cast<int>(kept.back().size()));
  }
}

bool soluxSchemaResolves() {
  const auto *pool = google::protobuf::DescriptorPool::generated_pool();
  const auto *svc = pool->FindServiceByName("solux.Searcher");
  return svc != nullptr && svc->FindMethodByName("Search") != nullptr &&
         pool->FindServiceByName("solux.Indexer") != nullptr &&
         pool->FindServiceByName("solux.Admin") != nullptr &&
         pool->FindMessageTypeByName("solux.proto.SearchRequest") != nullptr;
}

} // namespace solux

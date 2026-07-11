#include <cstdio>
#include <chrono>
#include <filesystem>
#include <string>
#include <sys/resource.h>

#include "solux/index/BlockBoundsBuilder.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/BlockBounds.h"
#include "solux/search/IndexReader.h"
#include "solux/store/FSDirectory.h"
#include "solux/util/MemPool.h"

using namespace solux;

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "usage: solux_block_bounds <index-directory> [field|--open-only]\n");
    return 1;
  }
  fprintf(stderr, "offline builder: concurrent writers for this index are unsupported\n");
  try {
    FSDirectory dir{std::filesystem::path(argv[1])};
    auto openStart = std::chrono::steady_clock::now();
    IndexReader reader(dir);
    double openWall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - openStart).count();
    uint64_t mappedBytes = 0;
    for (auto& segment : reader.segments()) {
      MemPool pool;
      FieldReader fields(pool, segment.postingsReader());
      while (fields.readNextField()) {
        std::string field((std::string_view) fields.name());
        if (const BlockBounds* bounds = segment.blockBounds(field)) {
          mappedBytes += (uint64_t) bounds->mappedBytes();
        }
      }
    }
    printf("OPEN\twall_s=%.9f\tmapped_bytes=%llu\tvalidation_failures=%llu\n",
           openWall, (unsigned long long) mappedBytes,
           (unsigned long long) BlockBounds::validationFailures.load());
    if (argc == 3 && std::string_view(argv[2]) == "--open-only") return 0;
    std::string selected = argc == 3 ? argv[2] : "";
    uint64_t totalBytes = 0;
    double totalWall = 0.0;
    for (auto& segment : reader.segments()) {
      MemPool pool;
      FieldReader fields(pool, segment.postingsReader());
      while (fields.readNextField()) {
        SegFieldInfo info{};
        fields.readFieldInfo(info);
        std::string field((std::string_view) info.fieldname);
        if ((!selected.empty() && field != selected) || !BlockBoundsBuilder::eligible(info)) continue;
        auto result = BlockBoundsBuilder::build(
            dir, segment.segInfo.seg_id, segment.postingsReader(), info);
        totalBytes += result.bytes;
        totalWall += result.wallSeconds;
        printf("SIDECAR\tseg=%llu\tfield=%s\tfile=%s\tbytes=%llu\tterms=%llu"
               "\tgroups=%llu\tblocks=%llu\tavgdl=%.9g\tE=%.9g\twall_s=%.6f"
               "\tscratch_bytes=%llu\n",
               (unsigned long long) segment.segInfo.seg_id, field.c_str(),
               result.fileName.c_str(), (unsigned long long) result.bytes,
               (unsigned long long) result.admittedTerms,
               (unsigned long long) result.groups, (unsigned long long) result.blocks,
               result.avgdl, result.envelope, result.wallSeconds,
               (unsigned long long) result.scratchBytes);
      }
    }
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    printf("TOTAL\tbytes=%llu\twall_s=%.6f\tpeak_rss_kb=%ld\n",
           (unsigned long long) totalBytes, totalWall, usage.ru_maxrss);
  } catch (const std::exception& e) {
    fprintf(stderr, "solux_block_bounds: %s\n", e.what());
    return 2;
  }
  return 0;
}

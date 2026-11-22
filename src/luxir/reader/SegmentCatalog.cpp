// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SegmentCatalog.h"

#include "CatalogIO.h"
#include "VersionRuns.h"
#include "luxir/codec/NumColumnFormat.h"

namespace luxir {
namespace {

using namespace catalog_io;
constexpr char CATALOG_ERROR[] = "invalid segment catalog";
constexpr char CATALOG_OUTPUT_ERROR[] = "invalid segment catalog output";
constexpr size_t MIN_SPACE_BYTES = 5;  // Path length, doc count and three field table vints.

constexpr uint64_t HAS_OWNERS = 1;

void checkCatalogTail(uint64_t offset, uint64_t length, uint64_t fileSize, const Errors& errors) {
  if (!length || offset > fileSize || length != fileSize - offset) {
    errors.invalid("catalog range must reach the end of file 0");
  }
}

void checkCatalogStart(uint64_t offset, const Errors& errors, std::optional<uint64_t> expected = std::nullopt) {
  if (expected && offset != *expected) errors.invalid("directory start does not match catalog range");
  if (offset < IndexFormat::HEADER.size()) errors.invalid("catalog starts inside file header");
  if (offset % MAX_ALIGN) errors.invalid("misaligned catalog start");
}

std::span<const char> directoryBytes(std::span<const char> file, const Errors& errors) {
  if (file.size() < sizeof(uint64_t)) errors.invalid("missing directory length");
  auto footer = file.size() - sizeof(uint64_t);
  auto length = loadUnaligned<uint64_t>(file.data() + footer);
  if (!length || length > footer) errors.invalid("directory length exceeds the catalog or is zero");
  return file.subspan(footer - length, length);
}

std::vector<uint32_t> readFileNumbers(Reader& in) {
  auto limit = std::min((uint64_t)in.remaining().size(), (uint64_t)seg_location::maxFilenum() + 1);
  auto count = in.readVarUInt(limit);
  if (!count) in.invalid("catalog inventory has no files");
  std::vector<uint32_t> numbers;
  numbers.reserve(count);
  for (uint64_t i = 0; i < count; ++i) {
    auto file = (uint32_t)in.readVarUInt(seg_location::maxFilenum());
    if (i == 0 && file != Postings::PRIMARY_FILE) in.invalid("file inventory must start with file 0");
    if (i != 0 && file <= numbers.back()) in.invalid("file numbers are not strictly increasing");
    numbers.push_back(file);
  }
  return numbers;
}

field_parts::Slice readOwnershipIndex(Reader& in, FileSizes fileSizes, int32_t count) {
  auto slice = readSlice(in);
  checkSlice(in, slice, fileSizes, "directory slice outside data area");
  if (!field_parts::monotonicBlocksBytes(slice, count)) in.invalid("ownership index layout");
  return slice;
}

void checkFileNumbers(Reader& in, std::span<const uint32_t> fileNumbers, FileSizes fileSizes) {
  if (fileNumbers.size() != (size_t)std::count_if(fileSizes.begin(), fileSizes.end(), [](uint64_t size) {
    return size != 0;
  })) in.invalid("inventory differs from segment files");
  for (auto number : fileNumbers) {
    if (number >= fileSizes.size() || !fileSizes[number]) in.invalid("inventory contains an unknown file");
  }
}

VersionRunsDescriptor readVersionRuns(Reader& in, FileSizes fileSizes, int32_t maxDoc) {
  if (!maxDoc) return {};
  auto slice = readSlice(in);
  checkSlice(in, slice, fileSizes, "directory slice outside data area");
  auto trailer = in.readVarUInt(slice.bytes);
  auto count = (uint32_t)in.readVarUInt(maxDoc);
  if (!count || !VersionRuns::validSlice(slice.location.offset(), slice.bytes, trailer)) in.invalid("version runs layout");
  return VersionRunsDescriptor{slice, trailer, count};
}

FieldTable readFieldTable(Reader& in, std::span<const char> tail, uint64_t directoryStart, uint64_t& endOfTables) {
  auto recordsStart = in.readVarUInt(directoryStart);
  auto offsetsStart = in.readVarUInt(directoryStart);
  auto numFields = in.readVarUInt(INT32_MAX);
  auto tableBytes = (numFields + 1) * sizeof(FieldOffset);
  if (recordsStart != endOfTables || offsetsStart < recordsStart
      || tableBytes > directoryStart - offsetsStart) in.invalid("field table layout");
  auto offsets = tail.subspan(offsetsStart, tableBytes);
  auto recordBytes = loadUnaligned<FieldOffset>(offsets.data() + numFields * sizeof(FieldOffset));
  if (recordBytes != offsetsStart - recordsStart) in.invalid("record area length");
  endOfTables = offsetsStart + tableBytes;
  return FieldTable(tail.subspan(recordsStart, recordBytes), offsets, in);
}

OwnershipDescriptor readOwnership(Reader& in, FileSizes fileSizes, int32_t parentMaxDoc, int32_t maxDoc) {
  OwnershipDescriptor ownership{readOwnershipIndex(in, fileSizes, parentMaxDoc), {}};
  if (in.readVarUInt(HAS_OWNERS)) ownership.owners = readOwnershipIndex(in, fileSizes, maxDoc);
  return ownership;
}

} // namespace

FieldTable::FieldTable(std::span<const char> records, std::span<const char> offsets, const Errors& errors)
    : records(records), offsets(offsets) {
  if (offsets.size() < sizeof(FieldOffset) || offsets.size() % sizeof(FieldOffset)
      || offsets.size() / sizeof(FieldOffset) - 1 > INT32_MAX) errors.invalid("field offset table size");
  if (offset(0) != 0 || offset(size()) != records.size()) errors.invalid("field area endpoints");
  std::string_view previous;
  for (int32_t i = 0; i < size(); ++i) {
    auto start = offset(i);
    auto end = offset(i + 1);
    if (end <= start || end > records.size()) errors.invalid("field offset order");
    Reader record(records.subspan(start, end - start), errors);
    auto current = std::string_view(record.readPackedTerm("field name outside record"));
    if (i && current <= previous) errors.invalid("field name order");
    previous = current;
  }
}

FieldOffset FieldTable::offset(int32_t ordinal) const noexcept {
  return loadUnaligned<FieldOffset>(offsets.data() + (size_t)ordinal * sizeof(FieldOffset));
}

std::span<const char> FieldTable::record(int32_t ordinal) const {
  if (ordinal < 0 || ordinal >= size()) throw std::out_of_range("field ordinal");
  auto start = offset(ordinal);
  return records.subspan(start, offset(ordinal + 1) - start);
}

PackedTerm FieldTable::name(int32_t ordinal) const noexcept {
  if (ordinal < 0 || ordinal >= size()) return PackedTerm(nullptr);
  // A record begins with a PackedTerm: the name length byte and then the name.
  return catalog_io::packedTerm(records.data() + offset(ordinal));
}

int32_t FieldTable::find(std::string_view target) const noexcept {
  int32_t lo = 0, hi = size();
  while (lo < hi) {
    int32_t mid = lo + (hi - lo) / 2;
    if (name(mid) < target) lo = mid + 1;
    else hi = mid;
  }
  return lo < size() && name(lo) == target ? lo : -1;
}

SegmentCatalog::DirectoryHead SegmentCatalog::readDirectory(std::span<const char> file,
                                                            SegmentReadContext context) {
  Errors errors(CATALOG_ERROR, context);
  auto head = readDirectoryRegion(file, 0, errors);
  checkCatalogStart(head.offset, errors);
  return head;
}

SegmentCatalog::DirectoryHead SegmentCatalog::readDirectory(std::span<const char> file, CatalogRange range,
                                                            SegmentReadContext context) {
  Errors errors(CATALOG_ERROR, context);
  checkCatalogTail(range.offset, range.length, file.size(), errors);
  auto head = readDirectoryRegion(file.subspan(range.offset), range.offset, errors);
  checkCatalogStart(head.offset, errors, range.offset);
  return head;
}

SegmentCatalog::DirectoryHead SegmentCatalog::readDirectory(std::span<const char> catalog,
                                                            uint64_t offset, uint64_t fileSize,
                                                            SegmentReadContext context) {
  Errors errors(CATALOG_ERROR, context);
  checkCatalogTail(offset, catalog.size(), fileSize, errors);
  auto head = readDirectoryRegion(catalog, offset, errors);
  checkCatalogStart(head.offset, errors, offset);
  return head;
}

SegmentCatalog::DirectoryHead SegmentCatalog::readDirectoryRegion(std::span<const char> region,
                                                                  uint64_t offset, const Errors& errors) {
  auto bytes = directoryBytes(region, errors);
  Reader in(bytes, errors);
  auto startInRegion = (uint64_t)(bytes.data() - region.data());
  DirectoryHead head;
  head.maxDoc = (int32_t)in.readVarUInt(INT32_MAX);
  if (!validSpaceDocCount(head.maxDoc)) in.invalid("root doc count out of range");
  head.directoryStart = in.readVarUInt();
  if (head.directoryStart > startInRegion) {
    in.invalid("directory start");
  }
  auto start = startInRegion - head.directoryStart;
  head.offset = offset + start;
  head.tail = region.subspan(start);
  head.numbers = readFileNumbers(in);
  head.entries = in.remaining();
  return head;
}

SegmentCatalog SegmentCatalog::decode(const DirectoryHead& head, FileSizes fileSizes, SegmentReadContext context) {
  Reader in(head.entries, Errors(CATALOG_ERROR, context));
  if (fileSizes.empty() || !fileSizes[Postings::PRIMARY_FILE]) in.invalid("missing primary file size");
  auto primarySize = fileSizes[Postings::PRIMARY_FILE];
  if (primarySize != head.offset + head.tail.size()) in.invalid("primary file size differs from inventory");
  checkFileNumbers(in, head.fileNumbers(), fileSizes);

  SegmentCatalog result;
  result.dataSizes.assign(fileSizes.begin(), fileSizes.end());
  result.dataSizes[Postings::PRIMARY_FILE] = head.offset;
  result.versions = readVersionRuns(in, result.dataSizes, head.maxDoc);
  auto numSpaces = in.readVarUInt(UINT32_MAX);
  if (!numSpaces || numSpaces > in.remaining().size() / MIN_SPACE_BYTES) in.invalid("row space count");
  result.spaces.reserve(numSpaces);
  uint64_t endOfTables = 0;
  for (uint64_t i = 0; i < numSpaces; ++i) {
    auto path = in.readString(in.readVarUInt(in.remaining().size()));
    auto maxDoc = (int32_t)in.readVarUInt(INT32_MAX);
    auto table = readFieldTable(in, head.tail, head.directoryStart, endOfTables);
    OwnershipDescriptor ownership;
    if (i == 0) {
      if (!path.empty() || maxDoc != head.maxDoc) in.invalid("root row space");
    } else {
      if (!validChildSpacePath(path)) in.invalid("child row space path");
      if (path <= result.spaces.back().path) in.invalid("child row space order");
      if (!maxDoc || !validSpaceDocCount(maxDoc)) in.invalid("child doc count out of range");
      auto parent = result.findSpace(spaceParentPath(path));
      if (!parent || !result.spaces[(size_t)*parent].maxDoc) in.invalid("missing row space parent");
      ownership = readOwnership(in, result.dataSizes, result.spaces[(size_t)*parent].maxDoc, maxDoc);
    }
    result.spaces.push_back({{path, maxDoc, ownership}, table});
  }
  if (!in.remaining().empty() || endOfTables != head.directoryStart) in.invalid("trailing catalog bytes");
  return result;
}

void SegmentCatalogWriter::addOffset() {
  auto offset = output.size() - recordsStart;
  if (offset > std::numeric_limits<FieldOffset>::max()) throw std::length_error("field records exceed field offsets");
  offsets.push_back((FieldOffset)offset);
}

void SegmentCatalogWriter::beginSpace(const RowSpace& space) {
  endSpace();
  if (!root) root = &space;
  current = &space;
  recordsStart = output.size();
  ++numSpaces;
}

void SegmentCatalogWriter::addField(const IndexFieldInfo& fieldInfo) {
  assert(current);
  if (offsets.size() == INT32_MAX) throw std::length_error("too many fields in row space");
  addOffset();
  FieldRecordCodec::encode(output, fieldInfo, current->maxDoc);
}

void SegmentCatalogWriter::endSpace() {
  if (!current) return;
  auto numFields = offsets.size();
  addOffset();  // Sentinel marks the end of the last record.
  auto offsetsStart = output.size();
  output.write(offsets.data(), offsets.size() * sizeof(FieldOffset));

  Writer entry(&directory, CATALOG_OUTPUT_ERROR);
  entry.writeVarUInt(current->path.size());
  entry.writeBytes(current->path);
  entry.writeVarUInt(current->maxDoc);
  entry.writeVarUInt(recordsStart);
  entry.writeVarUInt(offsetsStart);
  entry.writeVarUInt(numFields);
  if (current->ownership.ends) {
    writeSlice(entry, current->ownership.ends);
    entry.writeVarUInt(current->ownership.owners ? HAS_OWNERS : 0);
    if (current->ownership.owners) writeSlice(entry, current->ownership.owners);
  }
  offsets.clear();
  current = nullptr;
}

void SegmentCatalogWriter::finish(std::span<const uint32_t> fileNumbers,
                                  const VersionRunsDescriptor& versions) {
  endSpace();
  directory.close();
  Writer out(&output, CATALOG_OUTPUT_ERROR);
  if (!root || !root->path.empty()) out.invalid("missing root space");
  if (!validSpaceDocCount(root->maxDoc)) out.invalid("root doc count out of range");
  if ((root->maxDoc != 0) != bool(versions.slice)) out.invalid("version runs presence");
  auto directoryStart = output.size();
  out.writeVarUInt(root->maxDoc);
  out.writeVarUInt(directoryStart);
  out.writeVarUInt(fileNumbers.size());
  for (auto number : fileNumbers) out.writeVarUInt(number);
  if (versions.slice) {
    writeSlice(out, versions.slice);
    out.writeVarUInt(versions.trailerOffset);
    out.writeVarUInt(versions.count);
  }
  out.writeVarUInt(numSpaces);
  output.appendFile(spaceEntries);
  out.writeLittleEndian((uint64_t)(output.size() - directoryStart));
}

std::optional<SpaceOrdinal> SegmentCatalog::findSpace(std::string_view path) const noexcept {
  auto found = std::lower_bound(spaces.begin(), spaces.end(), path,
                                [](const auto& space, std::string_view key) { return space.path < key; });
  if (found == spaces.end() || found->path != path) return {};
  return (SpaceOrdinal)(found - spaces.begin());
}

FieldRecordHeader SegmentCatalog::fieldHeader(SpaceOrdinal space, int32_t ordinal, const SegmentCore* core) const {
  const auto& descriptor = spaces.at((size_t)space);
  auto bytes = descriptor.fields.record(ordinal);
  return FieldRecordCodec::fieldHeader(bytes, descriptor.maxDoc, nullptr,
      {core, descriptor.fields.name(ordinal), Postings::PRIMARY_FILE});
}

FieldRecordPtr SegmentCatalog::field(SpaceOrdinal space, int32_t ordinal, const SegmentCore* core) const {
  const auto& descriptor = spaces.at((size_t)space);
  auto bytes = descriptor.fields.record(ordinal);
  return FieldRecordCodec::decode(bytes, descriptor.maxDoc, dataSizes, ordinal,
      {core, descriptor.fields.name(ordinal), Postings::PRIMARY_FILE});
}

} // namespace luxir

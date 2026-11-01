// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <boost/container/small_vector.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <optional>
#include <type_traits>
#include <vector>

#include "luxir/api/build.h"
#include "luxir/search/IndexReader.h"
#include "luxir/search/DateOutput.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/search/OrdMapStr.h"
#include "luxir/search/ops/DomainIter.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

// Types are fixed by the resolved key specification. Null is tagged separately
// from every value, including numeric extrema and string ordinal zero.
struct CollapseKey {
  struct Component {
    bool present = false;
    int64_t value = 0;
    bool operator==(const Component&) const = default;
  };
  // Singleton and component tags are identities, never result-order keys.
  std::optional<uint64_t> singleton;
  boost::container::small_vector<Component, 2> components;
  size_t ramBytesUsed() const {
    return components.size() > 2 ? components.capacity() * sizeof(Component) : 0;
  }
  bool operator==(const CollapseKey& other) const {
    return singleton == other.singleton && components == other.components;
  }
};

struct CollapseKeyHash {
  template <typename Key> size_t operator()(const Key& key) const {
    size_t hash = 0;
    boost::hash_combine(hash, key.singleton.has_value());
    if (key.singleton) boost::hash_combine(hash, *key.singleton);
    for (auto component : key.components) {
      boost::hash_combine(hash, component.present);
      boost::hash_combine(hash, component.value);
    }
    return hash;
  }
};

// Tagged channels leave every int64 pattern available to ordinary keys.
struct GroupIdentity {
  enum Kind { VALUE, NULL_KEY, SINGLETON, DROP } kind;
  int64_t value = 0;
};

struct CollapsePlan {
  struct Field {
    std::string name;
    luxir::api::GroupComponent_::Type type;
    std::shared_ptr<OrdMap> ords;
    IndexReader::FieldSegments segments;
    ValueNature nature = ValueNature::NUMBER;
    bool ordinal() const { return type == luxir::api::GroupComponent_::Type::STRING
                               || type == luxir::api::GroupComponent_::Type::ID; }
  };
  std::vector<Field> fields;
  luxir::api::Collapse_::MissingPolicy missing;
  IndexReader& reader;
  bool getNumber;

  CollapsePlan(IndexReader& reader, luxir::api::Collapse_::MissingPolicy missing,
               bool getNumber) : missing(missing), reader(reader), getNumber(getNumber) {}

  GroupIdentity missingIdentity(int32_t segment, int32_t doc) const {
    using Missing = luxir::api::Collapse_::MissingPolicy;
    if (missing == Missing::GROUP) return {GroupIdentity::NULL_KEY};
    if (missing == Missing::DROP) return {GroupIdentity::DROP};
    return {GroupIdentity::SINGLETON,
        (int64_t)(((uint64_t)(uint32_t)segment << 32) | (uint32_t)doc)};
  }

  const CollapseKey* finishKey(CollapseKey& key, bool absent,
                                int32_t segment, int32_t doc) const {
    key.singleton.reset();
    if (absent) {
      auto identity = missingIdentity(segment, doc);
      if (identity.kind == GroupIdentity::DROP) return nullptr;
      if (identity.kind == GroupIdentity::SINGLETON) {
        key.components.clear();
        key.singleton = (uint64_t)identity.value;
      }
    }
    return &key;
  }

  void renderKeys(std::span<const api::GroupKey> keys, uint64_t flags,
                  std::pmr::memory_resource& mr) const {
    if (RequestFlags::dateMode(flags) == RequestFlags::DateMode::NATIVE) return;
    for (size_t i = 0; i < fields.size(); i++) {
      withDateOutput(fields[i].nature, flags, mr, [&] { return fields[i].name; }, [&](auto output) {
        if constexpr (!std::is_same_v<decltype(output), NativeDateOutput>) {
          for (const auto& key : keys) {
            if (key.singleton) continue;
            auto& component = const_cast<api::GroupComponent&>(key.component[i]);
            if (auto* value = std::get_if<int64_t>(&component.value)) component.value = output(*value);
          }
        }
      });
    }
  }

  void emit(luxir::api::GroupKey& out, const CollapseKey& key,
            std::pmr::memory_resource& mr, MemPool& pool) const {
    if (key.singleton) { out.singleton = key.singleton; return; }
    auto* components = luxir::api::build::allocArray(out.component, fields.size(), mr);
    for (size_t i = 0; i < fields.size(); i++) {
      components[i].type = fields[i].type;
      if (!key.components[i].present) {
        components[i].value = google::protobuf::NullValue::NULL_VALUE;
      } else if (fields[i].ordinal()) {
        OrdMapStr strings(pool, fields[i].ords.get(), reader, fields[i].segments);
        components[i].value = luxir::api::build::arenaStr(
            mr, strings.ordToStr(key.components[i].value));
      } else components[i].value = key.components[i].value;
    }
  }
};

// Shared segment-local decode state. Facet walks use global ords and actual
// int64 values. Collapse can keep local ords until admission, then translate
// retained keys for output and cross-worker merge. No interner lock.
class CollapseKeyReader {
  struct Column {
    std::optional<OrdColReader> ords;
    OrdMap::SegToGlobal mapping;
    std::optional<IntColReader> numbers;
    std::optional<IntColReader::Iterator> values;
    bool dense = false;
  };
  const CollapsePlan& plan;
  std::vector<Column> columns;
  CollapseKey scratch;
  int32_t lastDoc = -1;

public:
  explicit CollapseKeyReader(const CollapsePlan& plan) : plan(plan) {}

  // Kept out of line, like the collectors' group admission below: inlined
  // into their document loops, these cost the loops registers.
  LUXIR_NOINLINE void setSegment(int32_t segment) {
    lastDoc = -1;
    columns.clear();
    columns.resize(plan.fields.size());
    scratch.components.resize(plan.fields.size());
    auto& postings = plan.reader.segments()[(size_t)segment].postingsReader();
    for (size_t i = 0; i < columns.size(); i++) {
      auto* info = plan.fields[i].segments.at((size_t)segment);
      if (!info) continue;
      columns[i].dense = info->docsWithField == postings.maxDoc();
      if (plan.fields[i].ordinal()) {
        columns[i].ords.emplace(postings, *info);
        columns[i].mapping = plan.fields[i].ords->getSegToGlobal(segment);
      } else {
        columns[i].numbers.emplace(postings, *info);
        columns[i].values.emplace(*columns[i].numbers);
      }
    }
  }

  LUXIR_NOINLINE const CollapseKey* read(int32_t segment, int32_t doc, bool localOrds = false) {
    // Bound-order and seeded field drivers may rewind within a segment.
    // Reset presence cursors only on an actual backward read; looking ahead
    // over a missing value must retain the future position.
    if (doc < lastDoc) {
      for (auto& column : columns) {
        if (column.values) column.values->reset();
      }
    }
    lastDoc = doc;
    scratch.singleton.reset();
    scratch.components.resize(columns.size());
    bool missing = false;
    for (size_t i = 0; i < columns.size(); i++) {
      auto& column = columns[i];
      auto& component = scratch.components[i];
      component = {};
      if (column.ords) {
        int32_t ord = column.ords->ordAt(doc);
        if (ord != 0) {
          int64_t value = (int64_t)ord - 1;
          if (!localOrds && column.mapping.bits != 0) {
            value = column.mapping.globalOrd(value);
          }
          component = {true, value};
        }
      } else if (column.values) {
        // A missing doc can leave the sparse iterator on a future value.
        // Its forward-seek contract requires target > current.
        if (column.values->docId() < doc) column.values->advance(doc);
        if (column.values->docId() == doc) component = {true, column.values->value()};
      }
      missing |= !component.present;
    }
    return plan.finishKey(scratch, missing, segment, doc);
  }

  // Invert only the retained ords, in global order. Nothing proportional to
  // maxDoc or the segment dictionary is allocated. An absent component means
  // that retained tuple cannot occur in this segment, not that it is null.
  void remapRetained(std::span<const CollapseKey> keys,
                     std::vector<CollapseKey>& local,
                     std::vector<uint8_t>& available) const {
    local.assign(keys.begin(), keys.end());
    available.assign(keys.size(), true);
    std::vector<size_t> order;
    order.reserve(keys.size());
    for (size_t component = 0; component < columns.size(); component++) {
      if (!plan.fields[component].ordinal()) continue;
      const auto& column = columns[component];
      order.clear();
      for (size_t slot = 0; slot < keys.size(); slot++) {
        if (available[slot] && !keys[slot].singleton
            && keys[slot].components[component].present) order.push_back(slot);
      }
      std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return keys[a].components[component].value < keys[b].components[component].value;
      });
      int64_t cursor = 0;
      const auto& mapping = column.mapping;
      auto globalAt = [&](int64_t ord) {
        skipCount(SkipStats::collapseRemapProbes);
        return mapping.globalOrd(ord);
      };
      for (size_t slot : order) {
        int64_t global = keys[slot].components[component].value;
        cursor = mapping.bits == 0 ? std::min(global, mapping.numOrds)
            : screaming::gallopLowerBound(cursor, mapping.numOrds, global, globalAt);
        if (cursor == mapping.numOrds || globalAt(cursor) != global) available[slot] = false;
        else local[slot].components[component].value = cursor;
      }
    }
  }

  // Price L inverse lookups against the documents this segment can supply.
  // Deep heaps use direct translation; identity maps need no remap at all.
  bool remapPays(int64_t depth, int64_t sourceCost) const {
    if (sourceCost <= 0) return false;
    int64_t work = 0;
    bool encoded = false;
    for (const auto& column : columns) {
      if (column.mapping.bits == 0) continue;
      encoded = true;
      int64_t probes = 1 + (int64_t)std::bit_width((uint64_t)depth)
          + (int64_t)std::bit_width((uint64_t)(column.mapping.numOrds
              / std::max<int64_t>(depth, 1)));
      // Include sorting the retained ords; cap before multiplying, including
      // unbounded limits whose depth spans the whole reader.
      if (depth > (sourceCost - work) / probes) return false;
      work += depth * probes;
    }
    return encoded && work < sourceCost;
  }

  void globalize(CollapseKey& key) const {
    if (key.singleton) return;
    for (size_t i = 0; i < columns.size(); i++) {
      auto& component = key.components[i];
      if (component.present && columns[i].mapping.bits != 0) {
        component.value = columns[i].mapping.globalOrd(component.value);
      }
    }
  }

private:
  // Bounded, forward-only cursors. The ordinary ordinal match is just a
  // two-pointer join; refill/seeks stay outside that per-document fast path.
  class OrdWalk {
    OrdValueBatches values;
    OrdValueBatches::Batch batch;
    int32_t position = 0;
    int32_t previous = -1;
    int64_t saved = -1;
    bool ended = false;

    int64_t seek(int32_t doc) {
      if (ended) return -1;
      while (position == batch.size || batch.docs[batch.size - 1] < doc) {
        if (!values.next(batch)) { ended = true; return -1; }
        position = 0;
      }
      position = (int32_t)(screaming::gallopLowerBound(
          batch.docs + position, batch.docs + batch.size, doc) - batch.docs);
      return batch.docs[position] == doc ? batch.ords[position++] : -1;
    }
  public:
    OrdWalk(Column& column, DocSet* domain, int32_t maxDoc)
        : values(domain, *column.ords, column.mapping, maxDoc, false, nullptr) {}
    int64_t read(int32_t doc) LUXIR_INLINE {
      assert(doc >= previous);
      if (doc == previous) return saved;
      previous = doc;
      if (position < batch.size && batch.docs[position] == doc) return saved = batch.ords[position++];
      return saved = seek(doc);
    }
  };

  template <typename Iterator>
  class NumberWalk {
    Iterator values;
    int32_t previous = -1;
    CollapseKey::Component saved;
  public:
    explicit NumberWalk(Column& column) : values(*column.numbers) {}
    CollapseKey::Component read(int32_t doc) LUXIR_INLINE {
      assert(doc >= previous);
      if (doc == previous) return saved;
      previous = doc;
      saved = {};
      if (values.docId() < doc) values.advance(doc);
      if (values.docId() == doc) saved = {true, values.value()};
      return saved;
    }
  };

  template <typename Iterator>
  class WalkColumn {
    std::optional<OrdWalk> ords;
    std::optional<NumberWalk<Iterator>> numbers;
  public:
    WalkColumn(Column& column, DocSet* domain, int32_t maxDoc) {
      if (column.ords) ords.emplace(column, domain, maxDoc);
      else if (column.numbers) numbers.emplace(column);
    }
    CollapseKey::Component read(int32_t doc) {
      if (ords) {
        int64_t value = ords->read(doc);
        return value >= 0 ? CollapseKey::Component{true, value} : CollapseKey::Component{};
      }
      return numbers ? numbers->read(doc) : CollapseKey::Component{};
    }
  };

public:
  // Facet value streams are in document order. One key cursor serves all of
  // a document's values. A second pass (missing or another query bucket) opens
  // a new walk, so numeric iterators never rewind within a walk.
  template <typename Visit>
  void withWalk(int32_t segment, DocSet* domain, Visit&& visit) {
    setSegment(segment);
    int32_t maxDoc = plan.reader.segments()[(size_t)segment].maxDoc();
    if (columns.size() == 1) {
      auto& info = columns[0];
      if (info.ords) {
        OrdWalk column(info, domain, maxDoc);
        if (info.dense) visit([&](int32_t doc) LUXIR_INLINE { return column.read(doc); });
        else visit([&](int32_t doc) LUXIR_INLINE -> GroupIdentity {
          int64_t value = column.read(doc);
          return value >= 0 ? GroupIdentity{GroupIdentity::VALUE, value} : plan.missingIdentity(segment, doc);
        });
      } else if (info.numbers) {
        withIntColIteratorType(domain, [&]<typename Iterator>() {
          NumberWalk<Iterator> column(info);
          if (info.dense) visit([&](int32_t doc) LUXIR_INLINE { return column.read(doc).value; });
          else visit([&](int32_t doc) LUXIR_INLINE -> GroupIdentity {
            auto value = column.read(doc);
            return value.present ? GroupIdentity{GroupIdentity::VALUE, value.value} : plan.missingIdentity(segment, doc);
          });
        });
      } else visit([&](int32_t doc) { return plan.missingIdentity(segment, doc); });
      return;
    }
    withIntColIteratorType(domain, [&]<typename Iterator>() {
      std::vector<WalkColumn<Iterator>> cursors;
      cursors.reserve(columns.size());
      for (auto& column : columns) cursors.emplace_back(column, domain, maxDoc);
      int32_t previous = -1;
      const CollapseKey* saved = nullptr;
      visit([&](int32_t doc) -> const CollapseKey* {
        if (doc == previous) return saved;
        previous = doc;
        scratch.components.resize(columns.size());
        bool absent = false;
        for (size_t i = 0; i < cursors.size(); i++) {
          scratch.components[i] = cursors[i].read(doc);
          absent |= !scratch.components[i].present;
        }
        return saved = plan.finishKey(scratch, absent, segment, doc);
      });
    });
  }

  // The same forward walk, exposing structural keys to bounded admission.
  // Scalar values keep their full width; missing handling stays in finishKey.
  template <typename Visit>
  void withKeyWalk(int32_t segment, DocSet* domain, Visit&& visit) {
    withWalk(segment, domain, [&](auto read) {
      using Identity = std::invoke_result_t<decltype(read), int32_t>;
      if constexpr (std::is_same_v<Identity, const CollapseKey*>) visit(read);
      else {
        CollapseKey key;
        visit([&](int32_t doc) -> const CollapseKey* {
          auto identity = read(doc);
          key.components.resize(1);
          if constexpr (std::is_same_v<Identity, int64_t>) key.components[0] = {true, identity};
          else key.components[0] = {identity.kind == GroupIdentity::VALUE,
              identity.kind == GroupIdentity::VALUE ? identity.value : 0};
          return plan.finishKey(key, !key.components[0].present, segment, doc);
        });
      }
    });
  }

};

// Only retained identities are interned for ranking: state is O(topCount).
// Exhaustive totals have a separate bitmap (one ordinal key) or exact set.
template <bool Single = true, bool Ordered = false>
class GroupAdmission {
  // The lookup table owns only slot indexes. Hash/equality read the one key
  // owned by each stable slot, including after the vector's storage grows.
  struct SlotHash {
    using is_transparent = void;
    const std::vector<CollapseKey>* keys;
    size_t operator()(size_t slot) const { return (*this)((*keys)[slot]); }
    size_t operator()(const CollapseKey& key) const { return CollapseKeyHash{}(key); }
  };
  struct SlotEqual {
    using is_transparent = void;
    const std::vector<CollapseKey>* keys;
    bool operator()(size_t a, size_t b) const { return (*keys)[a] == (*keys)[b]; }
    bool operator()(size_t slot, const CollapseKey& key) const { return (*keys)[slot] == key; }
    bool operator()(const CollapseKey& key, size_t slot) const { return (*this)(slot, key); }
  };
  struct CappedState {
    size_t perKey = 1;
    std::vector<boost::container::small_vector<size_t, 4>> members;
    std::vector<size_t> entryKeys;
    std::vector<size_t> freeKeys;
  };
  struct SingleState {};
  [[no_unique_address]] std::conditional_t<Single, SingleState, CappedState> caps;

  template <typename Queue>
  void repairMembers(size_t key, size_t pos, Queue& heap) requires Ordered {
    auto& members = caps.members[key];
    auto slot = members[pos];
    size_t child;
    while ((child = pos * 2 + 1) < members.size()) {
      if (child + 1 < members.size()
          && heap.precedes(heap.atSlot(members[child]), heap.atSlot(members[child + 1]))) child++;
      if (!heap.precedes(heap.atSlot(slot), heap.atSlot(members[child]))) break;
      members[pos] = members[child];
      pos = child;
    }
    members[pos] = slot;
  }
  bool segmentLocal = false;
  std::vector<CollapseKey> localKeys;
  std::vector<uint8_t> localAvailable;
public:
  const CollapsePlan& plan;
  CollapseKeyReader reader;
  std::vector<CollapseKey> keys;
  boost::unordered_flat_set<size_t, SlotHash, SlotEqual> retained;
  explicit GroupAdmission(const CollapsePlan& plan, size_t perKey = 1)
      : plan(plan), reader(plan), retained(0, SlotHash{&keys}, SlotEqual{&keys}) {
    if constexpr (Single) assert(perKey == 1);
    else { assert(perKey > 0); caps.perKey = perKey; }
  }

  const CollapseKey& keyAt(size_t slot) const {
    if constexpr (Single) return keys[slot];
    else return keys[caps.entryKeys[slot]];
  }

  // Hash/equality callbacks point to the key vectors, whose addresses are stable.
  GroupAdmission(const GroupAdmission&) = delete;
  GroupAdmission& operator=(const GroupAdmission&) = delete;

  void setSegment(int32_t segment, int64_t depth, int64_t sourceCost = -1) {
    reader.setSegment(segment);
    // Runtime caps use one global key per retained key record. The existing
    // cap-one collector alone uses retained-key segment remapping.
    if constexpr (!Single) return;
    if (sourceCost < 0) sourceCost = plan.reader.segments()[(size_t)segment].maxDoc();
    // Exact totals need every global identity; remapping just retained groups
    // cannot replace that work. Facet walks also keep their global key space.
    bool nextLocal = !plan.getNumber && reader.remapPays(depth, sourceCost);
    if (!segmentLocal && !nextLocal) return;
    if (segmentLocal == nextLocal) retained.clear();
    else {
      // Rebind once on a key-space transition, keeping hash/equality as the
      // original direct slot-to-key lookup on every hit. Ordinary global
      // segments retain their table across switches, without a rebuild.
      auto* indexed = nextLocal ? &localKeys : &keys;
      retained = decltype(retained)(0, SlotHash{indexed}, SlotEqual{indexed});
    }
    segmentLocal = nextLocal;
    if (segmentLocal) reader.remapRetained(keys, localKeys, localAvailable);
    for (size_t slot = 0; slot < keys.size(); slot++) {
      if (!segmentLocal || localAvailable[slot]) retained.insert(slot);
    }
  }

  const CollapseKey* read(int32_t segment, int32_t doc) {
    return reader.read(segment, doc, segmentLocal);
  }

  void finishSegment() {
    if (!segmentLocal) return;
    retained = decltype(retained)(0, SlotHash{&keys}, SlotEqual{&keys});
    segmentLocal = false;
    for (size_t slot = 0; slot < keys.size(); slot++) retained.insert(slot);
  }

  // Capped top-N is mergeable, including perKey=1 collapse. Fix a document d:
  // for each key, the number of locally admissible documents better than d is
  // at most the global number. A partition's capped top-N therefore contains
  // every document of the global capped top-N lying in it. Union then recut
  // is exact and associative. Rank ties must use the same document order.
  // Callers reject against the global floor BEFORE decoding the key: a hit
  // that cannot beat that floor cannot improve a retained key either. Every
  // retained member ranks at or above the floor. A capped key may reject a hit
  // above it; that only leaves a conservative floor, never an unsafe skip.
  template <typename Queue, typename Item, typename Better, typename Write,
            typename SameEntry = decltype([](const auto&) { return false; })>
  bool admit(const CollapseKey& key, Queue& heap, Item candidate,
             Better&& better, Write&& write, SameEntry sameEntry = {}) {
    if constexpr (Single) {
      // The collapse collectors' original shape: improve in place, fill, or
      // replace the floor through top()/updateTop().
      auto found = retained.find(key);
      if (found != retained.end()) {
        size_t slot = *found;
        auto& member = heap.atSlot(slot);
        if (!better(member)) return false;
        write(slot, true);
        candidate.slot = slot;
        member = candidate;
        heap.updateSlot(slot);
      } else if (heap.size() < heap.capacity()) {
        size_t slot = heap.size();
        retain(key, slot);
        write(slot, false);
        candidate.slot = slot;
        heap.insert(candidate);
      } else {
        if (heap.capacity() == 0) return false;
        auto& bottom = heap.top();
        if (!better(bottom)) return false;
        size_t slot = (size_t)bottom.slot;
        retain(key, slot);
        write(slot, false);
        candidate.slot = slot;
        bottom = candidate;
        heap.updateTop();
      }
      return true;
    } else {
      if (heap.capacity() == 0 ||
          (heap.size() == heap.capacity() && !better(heap.top()))) return false;
      auto found = retained.find(key);
      size_t keyIndex = found == retained.end() ? SIZE_MAX : *found;
      size_t slot = SIZE_MAX;
      size_t memberPosition = 0;
      if (keyIndex != SIZE_MAX) {
        auto& members = caps.members[keyIndex];
        // Repeated vector candidates/merge rounds may name the same document.
        // Updating it must not consume another place under the key's cap.
        for (size_t i = 0; i < members.size(); i++) {
          if (sameEntry(heap.atSlot(members[i]))) { slot = members[i]; memberPosition = i; break; }
        }
        if (slot == SIZE_MAX && members.size() == caps.perKey) {
          slot = members.front();
          if constexpr (!Ordered) for (size_t member : members) {
            if (heap.precedes(heap.atSlot(slot), heap.atSlot(member))) slot = member;
          }
          memberPosition = 0;
        }
        if (slot != SIZE_MAX) {
          if (!better(heap.atSlot(slot))) return false;
          write(slot, true);
          candidate.slot = slot;
          heap.atSlot(slot) = candidate;
          heap.updateSlot(slot);
          if constexpr (Ordered) repairMembers(keyIndex, memberPosition, heap);
          return true;
        }
      }
      slot = heap.size() < heap.capacity() ? heap.size() : (size_t)heap.top().slot;
      if (slot < caps.entryKeys.size()) {
        size_t oldKey = caps.entryKeys[slot];
        auto& members = caps.members[oldKey];
        if constexpr (Ordered) {
          // The global floor is also its own key's worst member.
          assert(members.front() == slot);
          std::pop_heap(members.begin(), members.end(), [&](size_t a, size_t b) {
            return heap.precedes(heap.atSlot(a), heap.atSlot(b));
          });
          members.pop_back();
          // Key records are recycled. Bound historical vector capacity too,
          // so many formerly large keys cannot retain O(topCount * perKey).
          if (members.capacity() > members.internal_capacity()
              && members.size() < members.capacity() / 2) members.shrink_to_fit();
        } else {
          auto member = std::find(members.begin(), members.end(), slot);
          assert(member != members.end());
          *member = members.back();
          members.pop_back();
        }
        if (members.empty()) {
          retained.erase(oldKey);
          caps.freeKeys.push_back(oldKey);
          if (oldKey == keyIndex) keyIndex = SIZE_MAX;
        }
      } else caps.entryKeys.push_back(SIZE_MAX);
      if (keyIndex == SIZE_MAX) {
        if (caps.freeKeys.empty()) {
          keyIndex = keys.size();
          keys.push_back(key);
          caps.members.emplace_back();
        } else {
          keyIndex = caps.freeKeys.back();
          caps.freeKeys.pop_back();
          keys[keyIndex] = key;
        }
        retained.insert(keyIndex);
      }
      caps.entryKeys[slot] = keyIndex;
      caps.members[keyIndex].push_back(slot);
      write(slot, false);
      candidate.slot = slot;
      if (slot == heap.size()) heap.insert(candidate);
      else { heap.atSlot(slot) = candidate; heap.updateSlot(slot); }
      if constexpr (Ordered) {
        auto& members = caps.members[keyIndex];
        std::push_heap(members.begin(), members.end(), [&](size_t a, size_t b) {
          return heap.precedes(heap.atSlot(a), heap.atSlot(b));
        });
      }
      return true;
    }
  }

  size_t retain(const CollapseKey& key, size_t slot) requires Single {
    if (slot < keys.size() && (!segmentLocal || localAvailable[slot])) retained.erase(slot);
    if (slot == keys.size()) keys.push_back(key);
    else keys[slot] = key;
    if (segmentLocal) {
      if (slot == localKeys.size()) {
        localKeys.push_back(key);
        localAvailable.push_back(true);
      } else {
        localKeys[slot] = key;
        localAvailable[slot] = true;
      }
      // Local hits of retained groups never translate. Only admission of a
      // new group needs a global identity for output and cross-worker merge.
      reader.globalize(keys[slot]);
    }
    retained.insert(slot);
    return slot;
  }
};

// Exact totals belong to document collapse collection, not to bounded group
// admission itself. Vector search uses GroupAdmission without allocating an
// unused ordinal-count bitmap just because its enclosing list requests totals.
class CollapseAdmission : public GroupAdmission<> {
public:
  using Plan = CollapsePlan;
  static constexpr bool isDiversified = false;
  std::vector<uint64_t> seenOrds;
  boost::unordered_flat_set<CollapseKey, CollapseKeyHash> seenKeys;
  int64_t singletons = 0;

  explicit CollapseAdmission(const CollapsePlan& plan) : GroupAdmission<>(plan) {
    if (plan.getNumber && plan.fields.size() == 1 && plan.fields[0].ordinal()) {
      seenOrds.resize((size_t)(((plan.fields[0].ords ? plan.fields[0].ords->numOrds() : 0) + 1 + 63) / 64));
    }
  }

  void count(const CollapseKey& key) {
    if (!plan.getNumber) return;
    if (key.singleton) { singletons++; return; }
    if (!seenOrds.empty()) {
      // Bit zero is the null key; present ord zero occupies bit one.
      int64_t ord = key.components[0].present ? key.components[0].value + 1 : 0;
      seenOrds[(size_t)(ord >> 6)] |= 1ULL << (ord & 63);
    } else seenKeys.insert(key);
  }

  int64_t totalGroups() const {
    int64_t count = singletons + (int64_t)seenKeys.size();
    for (uint64_t word : seenOrds) count += std::popcount(word);
    return count;
  }

  void mergeCounts(const CollapseAdmission& other) {
    singletons += other.singletons;
    for (size_t i = 0; i < seenOrds.size(); i++) seenOrds[i] |= other.seenOrds[i];
    seenKeys.insert(other.seenKeys.begin(), other.seenKeys.end());
  }

};

} // namespace luxir

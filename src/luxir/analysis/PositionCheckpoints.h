// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/analysis/Analyzer.h"

namespace luxir {

// Position checkpoints let a reader analyze a stored text value from near a
// position instead of from the start of the value: mapping a match at
// position N back to text costs about one checkpoint interval of analysis,
// whatever the value's length.
//
// Indexing a stored text field records, per document and per physical text
// field (a variant included, since its chain has its own positions), a
// checkpoint at the first token, at least INTERVAL tokens after the previous
// checkpoint (or the field's first token), where analysis can resume: the
// first token of a value always, and a token inside a value when
// TokenChain::resumeOffset() says the whole chain can restart there. They are
// stored as one extra stored-field entry, storedName(field), in the same
// stored resource and document as the field's stored value. A document whose
// field has at most INTERVAL tokens stores nothing.
//
// Reading: find the entry's id once per segment, then per document
//
//   int32_t id = stored.fieldId(PositionCheckpoints::storedName(field));
//   std::vector<PositionCheckpoint> list;
//   PositionCheckpoints::read(stored, doc, id, list);       // empty if none
//   ... read the document's stored values into `values` ...
//   const PositionCheckpoint* from = PositionCheckpoints::atOrBefore(list, n);
//   FieldTokenStream stream(chain, values);
//   if (from) stream.start(*from); else stream.start();
//   while (stream.next()) { ... stream.position, stream.startOffset ... }
//
// `chain` must come from the analyzer of the physical field the checkpoints
// were recorded for, and `values` are that document's stored values (the
// logical field's entry), kept alive while the stream runs: StoredFieldsReader
// views are valid only during their read, so copy them or analyze inside the
// callback.
//
// Mid-value checkpoints need resume points: with the built-in tokenizers, a
// token right after an ASCII space, tab or line break. Text without them
// (CJK, Thai) gets checkpoints only at value starts.

// Where analysis of a document's stored values can resume.
struct PositionCheckpoint {
  uint32_t value = 0;    // which value of a multi-valued field (0 for a single value)
  uint32_t offset = 0;   // byte offset in that stored value where analysis resumes
  int32_t position = 0;  // field position of the first token analyzed from `offset`

  bool operator==(const PositionCheckpoint&) const = default;
};

struct PositionCheckpoints {
  // Tokens between checkpoints, at least (so positions too).
  static constexpr int32_t INTERVAL = 128;

  // The stored-field entry holding a physical field's checkpoints. `$` never
  // occurs in a field name, so the entry cannot collide with a stored field.
  static std::string storedName(std::string_view field) { return std::string(field) + "$checkpoints"; }
  // Whether a stored-field entry name is a checkpoints entry, for readers
  // that walk every entry of a document (StoredFieldsReader::readDoc).
  static bool isEntry(std::string_view name) { return name.ends_with("$checkpoints"); }

  // Entry encoding: per checkpoint, in increasing position order,
  // [vint value delta][vint position delta][vint offset], where offset is a
  // delta from the previous checkpoint's offset within the same value and
  // absolute in a new value.
  static void encode(std::span<const PositionCheckpoint> list, std::string& out);
  // Throws std::runtime_error on malformed bytes.
  static void decode(std::string_view bytes, std::vector<PositionCheckpoint>& out);

  // Decodes the checkpoints of `doc` from a StoredFieldsReader; `id` is
  // fieldId(storedName(field)), negative when the segment has none. Leaves
  // `out` empty (and returns false) when the document has none. `out` is a
  // copy: it stays valid after later reads.
  template <class StoredReader>
  static bool read(StoredReader& reader, int32_t doc, int32_t id, std::vector<PositionCheckpoint>& out) {
    out.clear();
    if (id < 0) return false;
    reader.readFieldById(doc, id, [&](std::span<const std::string_view> values) {
      if (values.size() != 1) throw std::runtime_error("corrupt position checkpoints");
      decode(values[0], out);
    });
    return !out.empty();
  }

  // The last checkpoint at or before `position`, or nullptr when analysis must
  // start from the first value (FieldTokenStream::start()).
  static const PositionCheckpoint* atOrBefore(std::span<const PositionCheckpoint> list, int32_t position);
  // The first checkpoint after `position`, or nullptr: where analysis covering
  // `position` can stop (FieldTokenStream::stopAt).
  static const PositionCheckpoint* after(std::span<const PositionCheckpoint> list, int32_t position);
};

// Analyzes a document's values for one text field the way the indexer did,
// reporting each token's field position (values are POSITION_INCREMENT_GAP
// apart) and its byte offsets in its stored value. Started at a checkpoint,
// it yields exactly the tokens a start() stream yields from that checkpoint's
// token on, with the same positions and offsets.
//
// The chain is borrowed and reset per value; `values` must outlive the
// stream. Not thread-safe (nor is the chain).
class FieldTokenStream {
  TokenChain& chain;
  std::span<const std::string_view> values;
  size_t origin = 0;               // where analysis of the current value began
  uint32_t stopValue = UINT32_MAX; // analysis ends at (stopValue, stopOffset)
  size_t stopOffset = 0;
  int32_t pinned = 0;              // the resumed first token's position
  bool pin = false;
  bool open = false;
  bool done = false;
public:
  // Matches the indexer's gap between the values of a multi-valued field.
  static constexpr int32_t POSITION_INCREMENT_GAP = 100;

  // The current token, after next() returned true.
  uint32_t value = 0;     // its value's index
  int32_t position = -1;  // its field position
  int startOffset = 0;    // its byte span in values[value]
  int endOffset = 0;

  FieldTokenStream(TokenChain& chain, std::span<const std::string_view> values)
    : chain(chain), values(values) {}

  // Analyze from the first token of the first value.
  void start();
  // Analyze from a checkpoint of these values.
  void start(const PositionCheckpoint& from);
  // End before the token `until` resumes at: the stream then yields exactly
  // the tokens before that checkpoint's position. Lets a reader analyze one
  // span between checkpoints without filtering or scanning the rest of the
  // value. Call after start().
  void stopAt(const PositionCheckpoint& until);

  // Advance to the next token; false at the end of the field (or stopAt).
  bool next();

  // The current token's text and attributes (transient, see Token).
  const Token& token() { return chain.getToken(); }

private:
  void openValue();
};

} // namespace luxir

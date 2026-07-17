#include "JsonResponse.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <variant>
#include <vector>

// glaze's SIMD/scalar string-escape scan kernels + its two-char escape table
// (used by the appendJsonString bulk path below).
#include <glaze/simd/simd.hpp>
#include <glaze/simd/avx.hpp>
#include <glaze/simd/neon.hpp>
#include <glaze/simd/sse.hpp>
#include <glaze/util/parse.hpp>

namespace solux {

namespace {

// Scalar escape loop: the reference implementation, the short-string path,
// and the only path that renders exotic control characters (\u00xx).
// Force-inlined so the hot short-string case stays as cheap as it was when
// this WAS appendJsonString (the wrapper indirection measurably cost the
// id-sized-string benchmark otherwise).
[[gnu::always_inline]] inline void appendJsonStringScalar(std::string& out, std::string_view s) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':  out += R"(\")"; break;
      case '\\': out += R"(\\)"; break;
      case '\n': out += R"(\n)"; break;
      case '\r': out += R"(\r)"; break;
      case '\t': out += R"(\t)"; break;
      case '\b': out += R"(\b)"; break;
      case '\f': out += R"(\f)"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), R"(\u%04x)", (unsigned)(unsigned char)c);
          out += buf;
        } else {
          // Pass UTF-8 bytes through unescaped; JSON permits raw UTF-8.
          out += c;
        }
    }
  }
  out += '"';
}

void appendJsonString(std::string& out, std::string_view s) {
  // Short strings stay scalar: below one SIMD block the kernels do nothing
  // and the resize bookkeeping costs more than it saves.
  if (s.size() < 32) {
    appendJsonStringScalar(out, s);
    return;
  }

  // Bulk path: glaze's escape-scan kernels (AVX2/SSE2/NEON blocks, flagging
  // '"', '\\', and <0x20 while memcpy-ing clean runs) with this renderer's
  // escaping as the write_escape callback.  glz::char_escape_table's
  // two-char set is exactly ours (\b \t \n \f \r \" \\).  Control chars
  // OUTSIDE that set need the six-byte \u00xx form, but this path reserves
  // only two output bytes per input byte - so they flag `exotic` and the
  // whole string restarts on the scalar path (they essentially never occur
  // in real text; correctness over speed there).
  const size_t base = out.size();
  bool exotic = false;
  out.resize_and_overwrite(base + 2 + 2 * s.size(), [&](char* p, size_t) -> size_t {
    const char* c = s.data();
    const char* const e = c + s.size();
    char* data = p + base;
    *data++ = '"';
    auto writeEscape = [&]() {
      const uint16_t escaped = glz::char_escape_table[(uint8_t)*c];
      if (escaped) {
        std::memcpy(data, &escaped, 2);
        data += 2;
      } else {
        exotic = true;  // \u00xx case; result is discarded and re-rendered
      }
      ++c;
    };
#if defined(GLZ_USE_AVX2)
    glz::detail::avx2_string_escape(c, e, data, s.size(), writeEscape);
#endif
#if defined(GLZ_USE_SSE2)
    glz::detail::sse2_string_escape(c, e, data, s.size(), writeEscape);
#elif defined(GLZ_USE_NEON)
    glz::detail::neon_string_escape(c, e, data, s.size(), writeEscape);
#endif
    for (; c < e; ++c) {  // tail: less than one SIMD block remains
      const uint16_t escaped = glz::char_escape_table[(uint8_t)*c];
      if (escaped) {
        std::memcpy(data, &escaped, 2);
        data += 2;
      } else if ((uint8_t)*c < 0x20) {
        exotic = true;
      } else {
        *data++ = *c;
      }
    }
    *data++ = '"';
    return (size_t)(data - p);
  });
  if (exotic) [[unlikely]] {
    out.resize(base);
    appendJsonStringScalar(out, s);
  }
}

// to_chars into a stack buffer, not std::format_to(back_inserter): identical
// bytes, but format's per-call machinery measured ~2x slower for whole-doc
// rendering (see BM_JsonRender).
void appendInt(std::string& out, int64_t v) {
  char buf[24];
  auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, p);
}

template <typename F>
void appendFloating(std::string& out, F v) {
  // JSON has no NaN/Inf literals; emit null for the non-finite cases.
  if (!std::isfinite(v)) { out += "null"; return; }
  char buf[40];
  auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, p);
}

template <typename Repeated, typename Emit>
void appendArray(std::string& out, const Repeated& v, Emit&& emit) {
  out += '[';
  bool first = true;
  for (const auto& e : v) {
    if (!first) out += ',';
    first = false;
    emit(e);
  }
  out += ']';
}

// Per-cell renderers, one per column kind.  Missing renders as null: scalar
// columns by the exact missing_val sentinel, multi-valued columns by empty
// array (a present-but-empty list is indistinguishable from missing and also
// maps to null), vectors by the unset f32 oneof.  Out-of-range slots render
// null.  resolveCellFn dispatches a column's variant ONCE; the row walk then
// calls the resolved function per cell.
using CellFn = void (*)(std::string&, const solux::api::Column&, size_t);

namespace cell {
void str(std::string& out, const solux::api::Column& col, size_t i) {
  auto& c = std::get<solux::api::ColStr>(col.kind);
  if (i < c.v.size() && c.v[i] != c.missing_val) appendJsonString(out, c.v[i]);
  else out += "null";
}
void int64(std::string& out, const solux::api::Column& col, size_t i) {
  auto& c = std::get<solux::api::ColInt>(col.kind);
  if (i < c.v.size() && c.v[i] != c.missing_val) appendInt(out, c.v[i]);
  else out += "null";
}
void flt(std::string& out, const solux::api::Column& col, size_t i) {
  auto& c = std::get<solux::api::ColFloat>(col.kind);
  if (i < c.v.size() && c.v[i] != c.missing_val) appendFloating(out, c.v[i]);
  else out += "null";
}
void dbl(std::string& out, const solux::api::Column& col, size_t i) {
  auto& c = std::get<solux::api::ColDouble>(col.kind);
  if (i < c.v.size() && c.v[i] != c.missing_val) appendFloating(out, c.v[i]);
  else out += "null";
}
void arrStr(std::string& out, const solux::api::Column& col, size_t i) {
  auto& c = std::get<solux::api::ArrArrStr>(col.kind);
  if (i < c.v.size() && !c.v[i].v.empty())
    appendArray(out, c.v[i].v, [&out](const auto& s){ appendJsonString(out, s); });
  else out += "null";
}
void arrInt(std::string& out, const solux::api::Column& col, size_t i) {
  auto& c = std::get<solux::api::ArrArrInt>(col.kind);
  if (i < c.v.size() && !c.v[i].v.empty())
    appendArray(out, c.v[i].v, [&out](int64_t x){ appendInt(out, x); });
  else out += "null";
}
void arrFlt(std::string& out, const solux::api::Column& col, size_t i) {
  auto& c = std::get<solux::api::ArrArrFloat>(col.kind);
  if (i < c.v.size() && !c.v[i].v.empty())
    appendArray(out, c.v[i].v, [&out](float x){ appendFloating(out, x); });
  else out += "null";
}
void arrDbl(std::string& out, const solux::api::Column& col, size_t i) {
  auto& c = std::get<solux::api::ArrArrDouble>(col.kind);
  if (i < c.v.size() && !c.v[i].v.empty())
    appendArray(out, c.v[i].v, [&out](double x){ appendFloating(out, x); });
  else out += "null";
}
void vec(std::string& out, const solux::api::Column& col, size_t i) {
  auto& c = std::get<solux::api::ColVector>(col.kind);
  if (i < c.v.size() && c.v[i].f32.has_value())
    appendArray(out, c.v[i].f32->v, [&out](float x){ appendFloating(out, x); });
  else out += "null";
}
void multiVec(std::string& out, const solux::api::Column& col, size_t i) {
  auto& c = std::get<solux::api::MultiVector>(col.kind);
  if (i < c.v.size() && !c.v[i].v.empty()) {
    appendArray(out, c.v[i].v, [&out](const solux::api::Vector& v){
      if (v.f32.has_value()) appendArray(out, v.f32->v, [&out](float x){ appendFloating(out, x); });
      else out += "null";
    });
  } else out += "null";
}
void nul(std::string& out, const solux::api::Column&, size_t) { out += "null"; }
} // namespace cell

CellFn resolveCellFn(const solux::api::Column& col) {
  if (std::holds_alternative<solux::api::ColStr>(col.kind)) return cell::str;
  if (std::holds_alternative<solux::api::ColInt>(col.kind)) return cell::int64;
  if (std::holds_alternative<solux::api::ColFloat>(col.kind)) return cell::flt;
  if (std::holds_alternative<solux::api::ColDouble>(col.kind)) return cell::dbl;
  if (std::holds_alternative<solux::api::ArrArrStr>(col.kind)) return cell::arrStr;
  if (std::holds_alternative<solux::api::ArrArrInt>(col.kind)) return cell::arrInt;
  if (std::holds_alternative<solux::api::ArrArrFloat>(col.kind)) return cell::arrFlt;
  if (std::holds_alternative<solux::api::ArrArrDouble>(col.kind)) return cell::arrDbl;
  if (std::holds_alternative<solux::api::ColVector>(col.kind)) return cell::vec;
  if (std::holds_alternative<solux::api::MultiVector>(col.kind)) return cell::multiVec;
  return cell::nul;
}

// Facet bucket IDs are always present. Unlike document columns, their Column
// storage does not use missing_val sentinels.
void appendBucketId(std::string& out, const solux::api::Column& col, size_t i) {
  if (auto* c = std::get_if<solux::api::ColInt>(&col.kind)) {
    if (i < c->v.size()) appendInt(out, c->v[i]);
    else out += "null";
  } else if (auto* c = std::get_if<solux::api::ColStr>(&col.kind)) {
    if (i < c->v.size()) appendJsonString(out, c->v[i]);
    else out += "null";
  } else if (auto* c = std::get_if<solux::api::ArrArrInt>(&col.kind)) {
    if (i < c->v.size() && !c->v[i].v.empty())
      appendArray(out, c->v[i].v, [&](int64_t x){ appendInt(out, x); });
    else out += "null";
  } else {
    out += "null";
  }
}

void appendOpVal(std::string& out, const solux::api::Val& val);

// Document i is the merge of columns row i and docs[i] (see the DocList
// contract); pure-rows and pure-columns are the degenerate cases.  Column
// slots keep the sentinel rendering (missing -> null); row maps signal
// missing structurally (key absent), so they are emitted as-is.
//
// Each column is resolved ONCE into a plan entry (pre-escaped ","key":"
// prefix + per-kind cell function); the row walk then just executes the
// plan, instead of re-dispatching the column variant and re-escaping the
// field name for every cell.  Row maps (docs[i]) are heterogeneous by
// nature and stay per-value.
struct ColPlan {
  std::string key;  // ","name":" (first column omits the comma)
  CellFn fn;
  const solux::api::Column* col;
};

std::vector<ColPlan> buildColPlan(const solux::api::DocList& docs) {
  std::vector<ColPlan> plan;
  plan.reserve(docs.columns.size());
  for (const auto& [name, col] : docs.columns) {
    auto& p = plan.emplace_back();
    if (plan.size() > 1) p.key += ',';
    appendJsonString(p.key, name);
    p.key += ':';
    p.fn = resolveCellFn(col);
    p.col = &col;
  }
  return plan;
}

// One document object: the merge of the planned dense columns and doc i's
// sparse row map (a field name never appears in both).
void appendDocObject(std::string& out, const std::vector<ColPlan>& plan,
                     const solux::api::DocList& docs, size_t i) {
  out += '{';
  for (const auto& p : plan) {
    out += p.key;
    p.fn(out, *p.col, i);
  }
  bool first = plan.empty();
  if (i < docs.docs.size()) {
    for (const auto& [name, val] : docs.docs[i].fields) {
      if (!first) out += ',';
      first = false;
      appendJsonString(out, name);
      out += ':';
      appendOpVal(out, *val);
    }
  }
  out += '}';
}

void appendDocs(std::string& out, const solux::api::DocList& docs) {
  size_t numDocs = (size_t)docs.row_count;
  out += '[';
  if (numDocs == 0) {
    out += ']';
    return;
  }
  auto plan = buildColPlan(docs);
  for (size_t i = 0; i < numDocs; i++) {
    if (i) out += ',';
    appendDocObject(out, plan, docs, i);
  }
  out += ']';
}

void appendWarnings(std::string& out, std::span<const solux::api::Warning> warnings) {
  out += '[';
  for (size_t i = 0; i < warnings.size(); i++) {
    if (i) out += ',';
    out += R"({"code":)";
    appendJsonString(out, warnings[i].code);
    out += R"(,"message":)";
    appendJsonString(out, warnings[i].message);
    out += '}';
  }
  out += ']';
}

void appendDocList(std::string& out, const solux::api::DocList& docs) {
  out += '{';
  if (docs.found.has_value()) {
    out += R"("found":)";
    appendInt(out, *docs.found);
    out += ',';
  }
  if (docs.max_score.has_value()) {
    out += R"("max_score":)";
    appendFloating(out, *docs.max_score);
    out += ',';
  }
  out += R"("docs":)";
  appendDocs(out, docs);
  if (!docs.ops.empty()) {
    out += R"(,"ops":{)";
    bool firstOp = true;
    for (const auto& [name, val] : docs.ops) {
      if (!firstOp) out += ',';
      firstOp = false;
      appendJsonString(out, name);
      out += ':';
      appendOpVal(out, *val);
    }
    out += '}';
  }
  out += '}';
}

void appendFacetSlot(std::string& out, const solux::api::Val& val, size_t i) {
  if (auto* arr = std::get_if<solux::api::ArrDouble>(&val.kind)) {
    if (i < arr->v.size()) appendFloating(out, arr->v[i]);
    else out += "null";
  } else if (auto* arr = std::get_if<solux::api::ArrFloat>(&val.kind)) {
    if (i < arr->v.size()) appendFloating(out, arr->v[i]);
    else out += "null";
  } else if (auto* arr = std::get_if<solux::api::ArrInt>(&val.kind)) {
    if (i < arr->v.size()) appendInt(out, arr->v[i]);
    else out += "null";
  } else if (auto* arr = std::get_if<solux::api::ArrStr>(&val.kind)) {
    if (i < arr->v.size()) appendJsonString(out, arr->v[i]);
    else out += "null";
  } else if (auto* arr = std::get_if<solux::api::ArrVal>(&val.kind)) {
    if (i < arr->v.size()) appendOpVal(out, arr->v[i]);
    else out += "null";
  } else {
    out += "null";
  }
}

void appendFacetResult(std::string& out, const solux::api::FacetResult& facet) {
  out += R"({"buckets":[)";
  for (size_t i = 0; i < facet.counts.size(); i++) {
    if (i) out += ',';
    out += R"({"val":)";
    if (facet.bucket_ids.has_value()) appendBucketId(out, *facet.bucket_ids, i);
    else out += "null";
    out += R"(,"count":)";
    appendInt(out, facet.counts[i]);
    for (const auto& [name, val] : facet.ops) {
      out += ',';
      appendJsonString(out, name);
      out += ':';
      appendFacetSlot(out, *val, i);
    }
    out += '}';
  }
  out += ']';
  if (facet.missing.has_value()) {
    out += R"(,"missing":)";
    appendInt(out, *facet.missing);
  }
  if (facet.total_buckets.has_value()) {
    out += R"(,"total_buckets":)";
    appendInt(out, *facet.total_buckets);
  }
  out += '}';
}

void appendOpVal(std::string& out, const solux::api::Val& val) {
  if (std::holds_alternative<std::monostate>(val.kind) ||
      std::holds_alternative<google::protobuf::NullValue>(val.kind)) {
    out += "null";
  } else if (auto* s = std::get_if<std::string_view>(&val.kind)) {
    appendJsonString(out, *s);
  } else if (auto* i = std::get_if<int64_t>(&val.kind)) {
    appendInt(out, *i);
  } else if (auto* d = std::get_if<double>(&val.kind)) {
    appendFloating(out, *d);
  } else if (auto* f = std::get_if<float>(&val.kind)) {
    appendFloating(out, *f);
  } else if (auto* b = std::get_if<bool>(&val.kind)) {
    out += *b ? "true" : "false";
  } else if (auto* docs = std::get_if<solux::api::DocList>(&val.kind)) {
    appendDocList(out, *docs);
  } else if (auto* facet = std::get_if<solux::api::FacetResult>(&val.kind)) {
    appendFacetResult(out, *facet);
  } else {
    // The generated Val writer starts at offset zero, so serialize into a
    // temporary before appending the canonical form of the remaining arms.
    std::string tmp;
    if (solux::api::write_json(val, tmp)) out += tmp;
    else out += "null";
  }
}

} // namespace

std::string renderSearchResponseLine(const solux::api::SearchResponse& resp) {
  std::string out;
  out += '{';
  if (!resp.error.empty()) {
    out += R"("error":)";
    appendJsonString(out, resp.error);
    out += "}\n";
    return out;
  }

  const solux::api::DocList* docs = nullptr;
  size_t promotedIndex = resp.ops.size();
  size_t opIndex = 0;
  for (const auto& [name, val] : resp.ops) {
    if (auto* docList = std::get_if<solux::api::DocList>(&val->kind)) {
      docs = docList;
      promotedIndex = opIndex;
      break;
    }
    opIndex++;
  }
  bool first = true;
  auto appendKey = [&](std::string_view name) {
    if (!first) out += ',';
    first = false;
    appendJsonString(out, name);
    out += ':';
  };
  if (docs) {
    // "found" is opt-in: set only when get_number was requested (an exact
    // count forgoes dynamic pruning).  Omit the key when absent rather than
    // rendering 0, so "not requested" is not confused with "zero found".
    if (docs->found.has_value()) {
      appendKey("found");
      appendInt(out, *docs->found);
    }
    if (docs->max_score.has_value()) {
      appendKey("max_score");
      appendFloating(out, *docs->max_score);
    }
    appendKey("docs");
    appendDocs(out, *docs);
  }
  // The promoted DocList's nested op results are hoisted into the same "ops"
  // object as the remaining sibling ops, mirroring the found/docs promotion.
  if (resp.ops.size() > (docs ? 1 : 0) || (docs && !docs->ops.empty())) {
    appendKey("ops");
    out += '{';
    bool firstOp = true;
    auto appendOp = [&](std::string_view name, const solux::api::Val& val) {
      if (!firstOp) out += ',';
      firstOp = false;
      appendJsonString(out, name);
      out += ':';
      appendOpVal(out, val);
    };
    if (docs) {
      for (const auto& [name, val] : docs->ops) appendOp(name, *val);
    }
    opIndex = 0;
    for (const auto& [name, val] : resp.ops) {
      if (opIndex++ == promotedIndex) continue;
      appendOp(name, *val);
    }
    out += '}';
  }
  if (!resp.warnings.empty()) {
    // declared degradations (the request was served, but not exactly as written)
    appendKey("warnings");
    appendWarnings(out, resp.warnings);
  }
  if (resp.more) {
    appendKey("more");
    out += "true";
  }
  out += "}\n";
  return out;
}

std::vector<DocRun> renderDocRuns(const solux::api::SearchResponse& resp) {
  std::vector<DocRun> runs;
  for (const auto& [name, val] : resp.ops) {
    if (const auto* dl = std::get_if<solux::api::DocList>(&val->kind)) {
      auto& run = runs.emplace_back(DocRun{name, dl, {}});
      if (dl->row_count > 0) {
        auto plan = buildColPlan(*dl);
        for (size_t i = 0; i < (size_t)dl->row_count; i++) {
          appendDocObject(run.body, plan, *dl, i);
          run.body += '\n';
        }
      }
    }
  }
  return runs;
}

bool frameDocRun(const DocRun& run, DocLinesState& state,
                 std::span<const solux::api::Warning> warnings, std::string& marker) {
  const solux::api::DocList& docs = *run.docs;
  bool firstForOp =
      std::find(state.headeredOps.begin(), state.headeredOps.end(), run.op) ==
      state.headeredOps.end();
  // Scalar DocList fields go out with the op's first run; request warnings
  // with the stream's first header.  Degraded execution must not be silent,
  // so warnings force a header even without get_number.  NOTE: this assumes
  // scalar fields are populated from the op's FIRST batch on (true for found;
  // when the engine starts setting max_score it must do the same, or a
  // late-arriving value would be suppressed here).
  bool haveFound = firstForOp && docs.found.has_value();
  bool haveMaxScore = firstForOp && docs.max_score.has_value();
  bool haveWarnings = !state.anyHeaderEmitted && !warnings.empty();
  // Multi-op framing: any change of op needs a marker for attribution.
  bool needMarker = state.multiOp && (firstForOp || run.op != state.currentOp);
  bool haveContent = haveFound || haveMaxScore || haveWarnings;
  if (run.body.empty() && !haveContent) return false;  // nothing to say

  if (haveContent || needMarker) {
    marker += R"({"_header_":{)";
    bool first = true;
    if (state.multiOp) {
      marker += R"("op":)";
      appendJsonString(marker, run.op);
      first = false;
    }
    if (haveFound) {
      if (!first) marker += ',';
      marker += R"("found":)";
      appendInt(marker, *docs.found);
      first = false;
    }
    if (haveMaxScore) {
      if (!first) marker += ',';
      marker += R"("max_score":)";
      appendFloating(marker, *docs.max_score);
      first = false;
    }
    if (haveWarnings) {
      if (!first) marker += ',';
      marker += R"("warnings":)";
      appendWarnings(marker, warnings);
    }
    marker += "}}\n";
    state.anyHeaderEmitted = true;
  }
  if (firstForOp) state.headeredOps.push_back(run.op);
  state.currentOp = run.op;
  return true;
}

std::string renderErrorBody(std::string_view message) {
  std::string out = R"({"error":)";
  appendJsonString(out, message);
  out += "}";
  return out;
}

} // namespace solux

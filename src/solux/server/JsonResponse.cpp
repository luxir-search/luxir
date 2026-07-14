#include "JsonResponse.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <variant>

namespace solux {

namespace {

void appendJsonString(std::string& out, std::string_view s) {
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

// Number of doc rows a column represents (the active oneof's repeated length).
size_t columnSize(const solux::api::Column& col) {
  if (auto* c = std::get_if<solux::api::ColStr>(&col.kind)) return c->v.size();
  if (auto* c = std::get_if<solux::api::ColInt>(&col.kind)) return c->v.size();
  if (auto* c = std::get_if<solux::api::ColFloat>(&col.kind)) return c->v.size();
  if (auto* c = std::get_if<solux::api::ColDouble>(&col.kind)) return c->v.size();
  if (auto* c = std::get_if<solux::api::ArrArrStr>(&col.kind)) return c->v.size();
  if (auto* c = std::get_if<solux::api::ArrArrInt>(&col.kind)) return c->v.size();
  if (auto* c = std::get_if<solux::api::ArrArrFloat>(&col.kind)) return c->v.size();
  if (auto* c = std::get_if<solux::api::ArrArrDouble>(&col.kind)) return c->v.size();
  if (auto* c = std::get_if<solux::api::ColVector>(&col.kind)) return c->v.size();
  if (auto* c = std::get_if<solux::api::MultiVector>(&col.kind)) return c->v.size();
  return 0;
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

// Emit doc i's value for this column, or null if the slot is missing.
void appendCell(std::string& out, const solux::api::Column& col, size_t i) {
  if (auto* c = std::get_if<solux::api::ColStr>(&col.kind)) {
    if (i < c->v.size() && c->v[i] != c->missing_val) appendJsonString(out, c->v[i]);
    else out += "null";
  } else if (auto* c = std::get_if<solux::api::ColInt>(&col.kind)) {
    if (i < c->v.size() && c->v[i] != c->missing_val) appendInt(out, c->v[i]);
    else out += "null";
  } else if (auto* c = std::get_if<solux::api::ColFloat>(&col.kind)) {
    if (i < c->v.size() && c->v[i] != c->missing_val) appendFloating(out, c->v[i]);
    else out += "null";
  } else if (auto* c = std::get_if<solux::api::ColDouble>(&col.kind)) {
    if (i < c->v.size() && c->v[i] != c->missing_val) appendFloating(out, c->v[i]);
    else out += "null";
  } else if (auto* c = std::get_if<solux::api::ArrArrStr>(&col.kind)) {
    // Multi-valued columns signal "missing" structurally as an empty list; render
    // that (and an out-of-range slot) as null, matching the scalar missing_val
    // contract.  A present but genuinely empty list is indistinguishable from
    // missing for these columns, so both map to null.
    if (i < c->v.size() && !c->v[i].v.empty())
      appendArray(out, c->v[i].v, [&](const auto& s){ appendJsonString(out, s); });
    else out += "null";
  } else if (auto* c = std::get_if<solux::api::ArrArrInt>(&col.kind)) {
    if (i < c->v.size() && !c->v[i].v.empty())
      appendArray(out, c->v[i].v, [&](int64_t x){ appendInt(out, x); });
    else out += "null";
  } else if (auto* c = std::get_if<solux::api::ArrArrFloat>(&col.kind)) {
    if (i < c->v.size() && !c->v[i].v.empty())
      appendArray(out, c->v[i].v, [&](float x){ appendFloating(out, x); });
    else out += "null";
  } else if (auto* c = std::get_if<solux::api::ArrArrDouble>(&col.kind)) {
    if (i < c->v.size() && !c->v[i].v.empty())
      appendArray(out, c->v[i].v, [&](double x){ appendFloating(out, x); });
    else out += "null";
  } else if (auto* c = std::get_if<solux::api::ColVector>(&col.kind)) {
    if (i < c->v.size() && c->v[i].f32.has_value())
      appendArray(out, c->v[i].f32->v, [&](float x){ appendFloating(out, x); });
    else out += "null";
  } else if (auto* c = std::get_if<solux::api::MultiVector>(&col.kind)) {
    if (i < c->v.size() && !c->v[i].v.empty()) {
      appendArray(out, c->v[i].v, [&](const solux::api::Vector& vec){
        if (vec.f32.has_value()) appendArray(out, vec.f32->v, [&](float x){ appendFloating(out, x); });
        else out += "null";
      });
    } else out += "null";
  } else {
    out += "null";
  }
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

void appendDocs(std::string& out, const solux::api::DocList& docs) {
  size_t numDocs = 0;
  for (const auto& [name, col] : docs.columns) {
    numDocs = columnSize(col);
    break;
  }
  out += '[';
  for (size_t i = 0; i < numDocs; i++) {
    if (i) out += ',';
    out += '{';
    bool first = true;
    for (const auto& [name, col] : docs.columns) {
      if (!first) out += ',';
      first = false;
      appendJsonString(out, name);
      out += ':';
      appendCell(out, col, i);
    }
    out += '}';
  }
  out += ']';
}

void appendOpVal(std::string& out, const solux::api::Val& val);

void appendDocList(std::string& out, const solux::api::DocList& docs) {
  out += '{';
  if (docs.matches.has_value()) {
    out += R"("found":)";
    appendInt(out, *docs.matches);
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
    // "found" is opt-in: matches is set only when get_number was requested (an
    // exact count forgoes dynamic pruning).  Omit the key when absent rather than
    // rendering 0, so "not requested" is not confused with "zero matches".
    if (docs->matches.has_value()) {
      appendKey("found");
      appendInt(out, *docs->matches);
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
    out += '[';
    for (size_t i = 0; i < resp.warnings.size(); i++) {
      if (i) out += ',';
      out += R"({"code":)";
      appendJsonString(out, resp.warnings[i].code);
      out += R"(,"message":)";
      appendJsonString(out, resp.warnings[i].message);
      out += '}';
    }
    out += ']';
  }
  if (resp.more) {
    appendKey("more");
    out += "true";
  }
  out += "}\n";
  return out;
}

std::string renderErrorBody(std::string_view message) {
  std::string out = R"({"error":)";
  appendJsonString(out, message);
  out += "}";
  return out;
}

} // namespace solux

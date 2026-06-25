#include "JsonResponse.h"

#include <charconv>
#include <cmath>
#include <cstdio>

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
size_t columnSize(const proto::Column& col) {
  if (col.has_col_s())    return col.col_s().v_size();
  if (col.has_col_i())    return col.col_i().v_size();
  if (col.has_col_f())    return col.col_f().v_size();
  if (col.has_col_d())    return col.col_d().v_size();
  if (col.has_multi_s())  return col.multi_s().v_size();
  if (col.has_multi_i())  return col.multi_i().v_size();
  if (col.has_multi_f())  return col.multi_f().v_size();
  if (col.has_multi_d())  return col.multi_d().v_size();
  if (col.has_col_vec())  return col.col_vec().v_size();
  if (col.has_multi_vec()) return col.multi_vec().v_size();
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
void appendCell(std::string& out, const proto::Column& col, size_t i) {
  int idx = (int)i;
  if (col.has_col_s()) {
    const auto& c = col.col_s();
    if (idx < c.v_size() && c.v(idx) != c.missing_val()) appendJsonString(out, c.v(idx));
    else out += "null";
  } else if (col.has_col_i()) {
    const auto& c = col.col_i();
    if (idx < c.v_size() && c.v(idx) != c.missing_val()) appendInt(out, c.v(idx));
    else out += "null";
  } else if (col.has_col_f()) {
    const auto& c = col.col_f();
    if (idx < c.v_size() && c.v(idx) != c.missing_val()) appendFloating(out, c.v(idx));
    else out += "null";
  } else if (col.has_col_d()) {
    const auto& c = col.col_d();
    if (idx < c.v_size() && c.v(idx) != c.missing_val()) appendFloating(out, c.v(idx));
    else out += "null";
  } else if (col.has_multi_s()) {
    // Multi-valued columns signal "missing" structurally as an empty list; render
    // that (and an out-of-range slot) as null, matching the scalar missing_val
    // contract.  A present but genuinely empty list is indistinguishable from
    // missing for these columns, so both map to null.
    const auto& c = col.multi_s();
    if (idx < c.v_size() && c.v(idx).v_size() > 0)
      appendArray(out, c.v(idx).v(), [&](const auto& s){ appendJsonString(out, s); });
    else out += "null";
  } else if (col.has_multi_i()) {
    const auto& c = col.multi_i();
    if (idx < c.v_size() && c.v(idx).v_size() > 0)
      appendArray(out, c.v(idx).v(), [&](int64_t x){ appendInt(out, x); });
    else out += "null";
  } else if (col.has_multi_f()) {
    const auto& c = col.multi_f();
    if (idx < c.v_size() && c.v(idx).v_size() > 0)
      appendArray(out, c.v(idx).v(), [&](float x){ appendFloating(out, x); });
    else out += "null";
  } else if (col.has_multi_d()) {
    const auto& c = col.multi_d();
    if (idx < c.v_size() && c.v(idx).v_size() > 0)
      appendArray(out, c.v(idx).v(), [&](double x){ appendFloating(out, x); });
    else out += "null";
  } else if (col.has_col_vec()) {
    const auto& c = col.col_vec();
    if (idx < c.v_size() && c.v(idx).has_f32())
      appendArray(out, c.v(idx).f32().v(), [&](float x){ appendFloating(out, x); });
    else out += "null";
  } else if (col.has_multi_vec()) {
    const auto& c = col.multi_vec();
    if (idx < c.v_size() && c.v(idx).v_size() > 0) {
      appendArray(out, c.v(idx).v(), [&](const proto::Vector& vec){
        if (vec.has_f32()) appendArray(out, vec.f32().v(), [&](float x){ appendFloating(out, x); });
        else out += "null";
      });
    } else out += "null";
  } else {
    out += "null";
  }
}

void appendDocs(std::string& out, const proto::DocList& docs) {
  size_t numDocs = 0;
  for (const auto& [name, col] : docs.columns()) {
    numDocs = columnSize(col);
    break;
  }
  out += '[';
  for (size_t i = 0; i < numDocs; i++) {
    if (i) out += ',';
    out += '{';
    bool first = true;
    for (const auto& [name, col] : docs.columns()) {
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

} // namespace

std::string renderSearchResponseLine(const proto::SearchResponse& resp) {
  std::string out;
  out += '{';
  if (!resp.error().empty()) {
    out += R"("error":)";
    appendJsonString(out, resp.error());
    out += "}\n";
    return out;
  }

  const proto::DocList* docs = nullptr;
  for (const auto& [name, val] : resp.ops()) {
    if (val.has_docs()) { docs = &val.docs(); break; }
  }
  if (docs) {
    out += R"("found":)";
    appendInt(out, docs->matches());
    out += R"(,"docs":)";
    appendDocs(out, *docs);
  } else {
    out += R"("docs":[])";
  }
  if (resp.more()) out += R"(,"more":true)";
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

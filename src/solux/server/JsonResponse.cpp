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
  for (const auto& [name, val] : resp.ops) {
    if (auto* docList = std::get_if<solux::api::DocList>(&val->kind)) {
      docs = docList;
      break;
    }
  }
  if (docs) {
    out += R"("found":)";
    appendInt(out, docs->matches.value_or(0));
    out += R"(,"docs":)";
    appendDocs(out, *docs);
  } else {
    out += R"("docs":[])";
  }
  if (!resp.warnings.empty()) {
    // declared degradations (the request was served, but not exactly as written)
    out += R"(,"warnings":[)";
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
  if (resp.more) out += R"(,"more":true)";
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

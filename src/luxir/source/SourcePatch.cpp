// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/source/SourcePatch.h"

#include <charconv>
#include <cmath>
#include <fmt/format.h>

#include "luxir/source/SourceJson.h"

namespace luxir::source {

namespace {

using detail::skipSpace;
using detail::skipString;
using detail::skipValue;

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

bool hex4(std::string_view s, size_t at, uint32_t& out) {
  if (at + 4 > s.size()) return false;
  auto res = std::from_chars(s.data() + at, s.data() + at + 4, out, 16);
  return res.ec == std::errc() && res.ptr == s.data() + at + 4;
}

// The text of a JSON string token (quotes included).
std::string decodeString(std::string_view token) {
  std::string out;
  std::string_view s = token.substr(1, token.size() - 2);
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c != '\\' || i + 1 >= s.size()) {
      out += c;
      continue;
    }
    char e = s[++i];
    switch (e) {
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        uint32_t cp = 0;
        if (!hex4(s, i + 1, cp)) {
          out += e;
          break;
        }
        i += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
          uint32_t low = 0;
          if (hex4(s, i + 3, low) && low >= 0xDC00 && low < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            i += 6;
          }
        }
        appendUtf8(out, cp);
        break;
      }
      default: out += e; break;  // '"', '\\', '/'
    }
  }
  return out;
}

std::string encodeString(std::string_view text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) out += fmt::format("\\u{:04x}", (unsigned)c);
        else out += (char)c;
    }
  }
  out += '"';
  return out;
}

bool keyIs(std::string_view token, std::string_view name) {
  std::string_view inner = token.substr(1, token.size() - 2);
  if (inner.find('\\') == std::string_view::npos) return inner == name;
  return decodeString(token) == name;
}

std::string_view trimJson(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && jsonSpace(s[b])) b++;
  while (e > b && jsonSpace(s[e - 1])) e--;
  return s.substr(b, e - b);
}

std::vector<std::string_view> splitPath(std::string_view path) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  for (;;) {
    size_t dot = path.find('.', start);
    parts.push_back(path.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return parts;
}

// Elements of an array value, or the value itself.
std::vector<std::string_view> elements(std::string_view json) {
  json = trimJson(json);
  std::vector<std::string_view> out;
  if (json.empty() || json.front() != '[') {
    out.push_back(json);
    return out;
  }
  size_t i = 1;
  for (;;) {
    skipSpace(json, i);
    if (i >= json.size() || json[i] == ']') break;
    if (json[i] == ',') { i++; continue; }
    size_t start = i;
    if (!skipValue(json, i)) throw PatchError("malformed JSON array");
    out.push_back(json.substr(start, i - start));
  }
  return out;
}

enum class JsonKind { STRING, NUMBER, TRUE_, FALSE_, NULL_, OBJECT, ARRAY, INVALID };

JsonKind kindOf(std::string_view json) {
  if (json.empty()) return JsonKind::INVALID;
  switch (json.front()) {
    case '"': return JsonKind::STRING;
    case '{': return JsonKind::OBJECT;
    case '[': return JsonKind::ARRAY;
    case 't': return JsonKind::TRUE_;
    case 'f': return JsonKind::FALSE_;
    case 'n': return JsonKind::NULL_;
    default: return JsonKind::NUMBER;
  }
}

struct Number {
  bool isInt = false;
  int64_t i = 0;
  double d = 0;
  double asDouble() const { return isInt ? (double)i : d; }
};

std::optional<Number> parseNumber(std::string_view text) {
  if (kindOf(text) != JsonKind::NUMBER) return std::nullopt;
  Number n;
  const char* end = text.data() + text.size();
  if (text.find_first_of(".eE") == std::string_view::npos) {
    auto res = std::from_chars(text.data(), end, n.i);
    if (res.ec == std::errc() && res.ptr == end) {
      n.isInt = true;
      return n;
    }
  }
  auto res = std::from_chars(text.data(), end, n.d);
  if (res.ec != std::errc() || res.ptr != end || !std::isfinite(n.d)) return std::nullopt;
  return n;
}

std::string renderNumber(const Number& n) {
  if (n.isInt) return std::to_string(n.i);
  if (!std::isfinite(n.d)) throw PatchError("the result is not a finite number");
  // A double stays spelled as one, so it keeps double arithmetic.
  std::string text = fmt::format("{}", n.d);
  if (text.find_first_of(".eE") == std::string::npos) text += ".0";
  return text;
}

Number requireNumber(std::string_view json, std::string_view what) {
  auto n = parseNumber(trimJson(json));
  if (!n) throw PatchError(fmt::format("{} is not a number", what));
  return *n;
}

std::string minified(std::string_view json) {
  std::string out;
  minifyJson(json, out);
  return out;
}

}  // namespace

std::optional<int> compareJson(std::string_view a, std::string_view b) {
  a = trimJson(a);
  b = trimJson(b);
  JsonKind ka = kindOf(a), kb = kindOf(b);
  if (ka == JsonKind::NUMBER && kb == JsonKind::NUMBER) {
    auto na = parseNumber(a), nb = parseNumber(b);
    if (!na || !nb) return std::nullopt;
    if (na->isInt && nb->isInt) return na->i < nb->i ? -1 : na->i > nb->i ? 1 : 0;
    double x = na->asDouble(), y = nb->asDouble();
    if (std::isnan(x) || std::isnan(y)) return std::nullopt;
    return x < y ? -1 : x > y ? 1 : 0;
  }
  if (ka == JsonKind::STRING && kb == JsonKind::STRING) {
    auto x = decodeString(a), y = decodeString(b);
    int c = x.compare(y);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
  }
  auto boolOf = [](JsonKind k) { return k == JsonKind::TRUE_ ? 1 : k == JsonKind::FALSE_ ? 0 : -1; };
  if (boolOf(ka) >= 0 && boolOf(kb) >= 0) return boolOf(ka) - boolOf(kb);
  if (ka == JsonKind::NULL_ && kb == JsonKind::NULL_) return 0;
  if ((ka == JsonKind::OBJECT || ka == JsonKind::ARRAY) && ka == kb) {
    if (minified(a) == minified(b)) return 0;
  }
  return std::nullopt;
}

bool equalJson(std::string_view a, std::string_view b) {
  auto c = compareJson(a, b);
  return c && *c == 0;
}

std::string valJson(const api::Val& val) {
  std::string out;
  if (!api::write_json(val, out)) throw PatchError("value cannot be written as JSON");
  return out;
}

std::optional<std::string> invalidPatchPath(std::string_view path) {
  if (path.empty()) return std::string("empty path");
  auto parts = splitPath(path);
  for (auto part : parts) {
    if (part.empty()) return fmt::format("path '{}' has an empty segment", path);
    if (part.size() >= 2 && part.front() == '_' && part.back() == '_') {
      return fmt::format("path '{}' names a reserved field", path);
    }
  }
  if (parts.front() == "id") return fmt::format("path '{}' would change the document id", path);
  return std::nullopt;
}

PatchDoc::PatchDoc(std::string_view json) {
  root.raw = own(std::string(trimJson(json)));
  if (root.raw.empty() || root.raw.front() != '{') throw PatchError("the document is not a JSON object");
  expand(root);
}

std::string_view PatchDoc::own(std::string text) {
  owned.push_back(std::make_unique<std::string>(std::move(text)));
  return *owned.back();
}

void PatchDoc::expand(Node& node) {
  if (node.kind != Node::RAW || node.raw.empty()) return;
  std::string_view s = node.raw;
  char open = s.front();
  if (open != '{' && open != '[') return;
  size_t i = 1;
  if (open == '{') {
    node.kind = Node::OBJECT;
    for (;;) {
      skipSpace(s, i);
      if (i >= s.size()) throw PatchError("malformed JSON object");
      if (s[i] == '}') break;
      if (s[i] == ',') { i++; continue; }
      if (s[i] != '"') throw PatchError("malformed JSON object");
      size_t keyStart = i;
      if (!skipString(s, i)) throw PatchError("malformed JSON object");
      std::string_view key = s.substr(keyStart, i - keyStart);
      skipSpace(s, i);
      if (i >= s.size() || s[i] != ':') throw PatchError("malformed JSON object");
      i++;
      skipSpace(s, i);
      size_t valueStart = i;
      if (!skipValue(s, i)) throw PatchError("malformed JSON object");
      Node child;
      child.raw = s.substr(valueStart, i - valueStart);
      node.members.emplace_back(key, std::move(child));
    }
  } else {
    node.kind = Node::ARRAY;
    for (;;) {
      skipSpace(s, i);
      if (i >= s.size()) throw PatchError("malformed JSON array");
      if (s[i] == ']') break;
      if (s[i] == ',') { i++; continue; }
      size_t valueStart = i;
      if (!skipValue(s, i)) throw PatchError("malformed JSON array");
      Node child;
      child.raw = s.substr(valueStart, i - valueStart);
      node.items.push_back(std::move(child));
    }
  }
  node.raw = {};
}

std::string PatchDoc::text(const Node& node) const {
  switch (node.kind) {
    case Node::RAW: return std::string(node.raw);
    case Node::OBJECT: {
      std::string out = "{";
      bool first = true;
      for (const auto& [key, value] : node.members) {
        if (!first) out += ',';
        first = false;
        out += key;
        out += ':';
        out += text(value);
      }
      out += '}';
      return out;
    }
    case Node::ARRAY: {
      std::string out = "[";
      bool first = true;
      for (const auto& item : node.items) {
        if (!first) out += ',';
        first = false;
        out += text(item);
      }
      out += ']';
      return out;
    }
  }
  return {};
}

namespace {
PatchDoc::Node* findMember(PatchDoc::Node& object, std::string_view key) {
  PatchDoc::Node* found = nullptr;
  for (auto& [name, value] : object.members) {
    if (keyIs(name, key)) found = &value;
  }
  return found;
}
}  // namespace

PatchDoc::Node* PatchDoc::parent(std::string_view path, bool create, std::string_view& key) {
  auto parts = splitPath(path);
  Node* node = &root;
  for (size_t p = 0; p + 1 < parts.size(); p++) {
    Node* next = findMember(*node, parts[p]);
    if (!next) {
      if (!create) return nullptr;
      Node object;
      object.kind = Node::OBJECT;
      putMember(*node, parts[p], std::move(object));
      next = findMember(*node, parts[p]);
    }
    expand(*next);
    if (next->kind != Node::OBJECT) {
      if (!create) return nullptr;
      throw PatchError(fmt::format("path '{}' crosses a value that is not an object", path));
    }
    node = next;
  }
  key = parts.back();
  return node;
}

void PatchDoc::putMember(Node& object, std::string_view key, Node value) {
  // The last duplicate keeps its place and takes the value; earlier ones go.
  ptrdiff_t last = -1;
  for (size_t i = 0; i < object.members.size(); i++) {
    if (keyIs(object.members[i].first, key)) last = (ptrdiff_t)i;
  }
  if (last < 0) {
    object.members.emplace_back(own(encodeString(key)), std::move(value));
    return;
  }
  object.members[(size_t)last].second = std::move(value);
  std::vector<std::pair<std::string_view, Node>> kept;
  kept.reserve(object.members.size());
  for (size_t i = 0; i < object.members.size(); i++) {
    if ((ptrdiff_t)i != last && keyIs(object.members[i].first, key)) continue;
    kept.push_back(std::move(object.members[i]));
  }
  object.members = std::move(kept);
}

const PatchDoc::Node* PatchDoc::get(std::string_view path) {
  std::string_view key;
  Node* object = parent(path, false, key);
  return object ? findMember(*object, key) : nullptr;
}

void PatchDoc::set(std::string_view path, std::string_view json) {
  std::string_view key;
  Node* object = parent(path, true, key);
  Node value;
  value.raw = own(std::string(trimJson(json)));
  putMember(*object, key, std::move(value));
}

void PatchDoc::unset(std::string_view path) {
  std::string_view key;
  Node* object = parent(path, false, key);
  if (!object) return;
  std::erase_if(object->members, [&](const auto& member) { return keyIs(member.first, key); });
}

void PatchDoc::mergeInto(Node& target, std::string_view patchJson) {
  Node patch;
  patch.raw = own(std::string(trimJson(patchJson)));
  expand(patch);
  for (auto& [keyToken, value] : patch.members) {
    std::string key = decodeString(keyToken);
    std::string_view raw = trimJson(value.raw);
    if (raw == "null") {
      std::erase_if(target.members, [&](const auto& member) { return keyIs(member.first, key); });
      continue;
    }
    if (!raw.empty() && raw.front() == '{') {
      Node* existing = findMember(target, key);
      if (existing) expand(*existing);
      if (existing && existing->kind == Node::OBJECT) {
        mergeInto(*existing, raw);
        continue;
      }
      Node object;
      object.kind = Node::OBJECT;
      mergeInto(object, raw);
      putMember(target, key, std::move(object));
      continue;
    }
    Node scalar;
    scalar.raw = raw;
    putMember(target, key, std::move(scalar));
  }
}

void PatchDoc::merge(std::string_view path, std::string_view json) {
  json = trimJson(json);
  if (json.empty() || json.front() != '{') throw PatchError("merge value is not an object");
  if (path.empty()) {
    mergeInto(root, json);
    return;
  }
  std::string_view key;
  Node* object = parent(path, true, key);
  Node* target = findMember(*object, key);
  if (target) expand(*target);
  if (!target || target->kind != Node::OBJECT) {
    Node fresh;
    fresh.kind = Node::OBJECT;
    putMember(*object, key, std::move(fresh));
    target = findMember(*object, key);
  }
  mergeInto(*target, json);
}

void PatchDoc::replace(std::string_view json) {
  json = trimJson(json);
  if (json.empty() || json.front() != '{') throw PatchError("replace value is not an object");
  Node fresh;
  fresh.raw = own(std::string(json));
  expand(fresh);
  root = std::move(fresh);
}

void PatchDoc::arith(Arith op, std::string_view path, std::string_view json) {
  std::string_view key;
  Node* object = parent(path, true, key);
  Node* current = findMember(*object, key);
  if (current && current->kind == Node::RAW && trimJson(current->raw) == "null") current = nullptr;
  std::string_view value = trimJson(json);
  std::string result;
  if (op == Arith::MIN || op == Arith::MAX) {
    if (!current) {
      result = std::string(value);
    } else {
      std::string now = text(*current);
      auto c = compareJson(now, value);
      if (!c) throw PatchError(fmt::format("field '{}' cannot be compared with the value", path));
      bool takeValue = op == Arith::MIN ? *c > 0 : *c < 0;
      if (!takeValue) return;
      result = std::string(value);
    }
  } else {
    Number delta = requireNumber(value, "the value");
    Number base;
    base.isInt = true;
    if (current) {
      std::string now = text(*current);
      if (kindOf(trimJson(now)) == JsonKind::ARRAY) {
        throw PatchError(fmt::format("field '{}' has several values", path));
      }
      base = requireNumber(now, fmt::format("field '{}'", path));
    }
    Number out;
    if (base.isInt && delta.isInt) {
      out.isInt = true;
      bool overflow = op == Arith::ADD ? __builtin_add_overflow(base.i, delta.i, &out.i)
                                       : __builtin_mul_overflow(base.i, delta.i, &out.i);
      if (overflow) throw PatchError(fmt::format("field '{}' would overflow a 64-bit integer", path));
    } else {
      out.d = op == Arith::ADD ? base.asDouble() + delta.asDouble() : base.asDouble() * delta.asDouble();
    }
    result = renderNumber(out);
  }
  Node fresh;
  fresh.raw = own(std::move(result));
  putMember(*object, key, std::move(fresh));
}

void PatchDoc::append(std::string_view path, std::string_view json, bool unique) {
  std::string_view key;
  Node* object = parent(path, true, key);
  Node* current = findMember(*object, key);
  std::string_view ownedJson = own(std::string(json));
  auto values = elements(ownedJson);
  Node list;
  list.kind = Node::ARRAY;
  if (current && current->kind == Node::RAW && trimJson(current->raw) == "null") current = nullptr;
  if (current) {
    expand(*current);
    if (current->kind == Node::ARRAY) list = std::move(*current);
    else list.items.push_back(std::move(*current));
  }
  for (auto value : values) {
    if (unique) {
      bool seen = false;
      for (const auto& item : list.items) {
        if (equalJson(text(item), value)) {
          seen = true;
          break;
        }
      }
      if (seen) continue;
    }
    Node item;
    item.raw = value;
    list.items.push_back(std::move(item));
  }
  putMember(*object, key, std::move(list));
}

void PatchDoc::removeValues(std::string_view path, std::string_view json) {
  std::string_view key;
  Node* object = parent(path, false, key);
  if (!object) return;
  Node* current = findMember(*object, key);
  if (!current) return;
  auto values = elements(json);
  auto matches = [&](const Node& node) {
    std::string now = text(node);
    for (auto value : values) {
      if (equalJson(now, value)) return true;
    }
    return false;
  };
  expand(*current);
  if (current->kind == Node::ARRAY) {
    std::erase_if(current->items, matches);
  } else if (matches(*current)) {
    unset(path);
  }
}

namespace {

using Status = api::PatchResult::Status;
using Op = api::PatchOp::Op;

// The id as the document's JSON would hold it.
bool idMatches(std::string_view json, std::string_view id) {
  json = trimJson(json);
  if (kindOf(json) == JsonKind::STRING) return decodeString(json) == id;
  // A numeric id: the same number, however it is spelled.
  return json == id || equalJson(json, id);
}

bool holds(PatchDoc& doc, const api::PatchCondition& cond) {
  using C = api::PatchCondition::Op;
  const PatchDoc::Node* node = doc.get(cond.path);
  if (cond.op == C::EXISTS) return node != nullptr;
  if (cond.op == C::MISSING) return node == nullptr;
  std::string subject;
  if (node) subject = doc.text(*node);
  else if (cond.missing.has_value()) subject = valJson(*cond.missing);
  else return cond.op == C::NE;
  if (!cond.value.has_value()) throw PatchError(fmt::format("condition on '{}' has no value", cond.path));
  auto c = compareJson(subject, valJson(*cond.value));
  switch (cond.op) {
    case C::EQ: return c && *c == 0;
    case C::NE: return !c || *c != 0;
    case C::LT: return c && *c < 0;
    case C::LTE: return c && *c <= 0;
    case C::GT: return c && *c > 0;
    case C::GTE: return c && *c >= 0;
    default: return false;
  }
}

void applyOp(PatchDoc& doc, const api::PatchOp& op) {
  auto value = [&]() {
    if (!op.value.has_value()) throw PatchError(fmt::format("op on '{}' has no value", op.path));
    return valJson(*op.value);
  };
  switch (op.op) {
    case Op::SET: doc.set(op.path, value()); break;
    case Op::UNSET: doc.unset(op.path); break;
    case Op::MERGE: doc.merge(op.path, value()); break;
    case Op::REPLACE: doc.replace(value()); break;
    case Op::INCREMENT: doc.arith(PatchDoc::Arith::ADD, op.path, value()); break;
    case Op::MULTIPLY: doc.arith(PatchDoc::Arith::MULTIPLY, op.path, value()); break;
    case Op::MIN: doc.arith(PatchDoc::Arith::MIN, op.path, value()); break;
    case Op::MAX: doc.arith(PatchDoc::Arith::MAX, op.path, value()); break;
    case Op::APPEND: doc.append(op.path, value(), false); break;
    case Op::APPEND_UNIQUE: doc.append(op.path, value(), true); break;
    case Op::REMOVE_VALUES: doc.removeValues(op.path, value()); break;
  }
}

}  // namespace

PatchOutcome applyPatch(const api::Patch& patch, const std::optional<std::string>& base, uint64_t baseVersion) {
  using E = api::Patch::Exists;
  PatchOutcome out;
  bool exists = base.has_value();
  if (patch.if_version && (!exists || baseVersion != *patch.if_version)) {
    out.status = Status::CONFLICT;
    return out;
  }
  if (patch.exists == E::MUST && !exists) {
    out.status = Status::NOT_FOUND;
    return out;
  }
  if (patch.exists == E::MUST_NOT && exists) {
    out.status = Status::CONFLICT;
    return out;
  }
  bool create = !exists;
  if (create && !(patch.upsert || patch.upsert_doc || patch.exists == E::MUST_NOT)) {
    out.status = Status::NOT_FOUND;
    return out;
  }
  if (create && patch.delete_doc) {
    out.status = Status::NOT_FOUND;
    return out;
  }
  try {
    std::string idJson = encodeString(patch.id);
    std::string start;
    if (!create) {
      start = minified(*base);
    } else if (patch.upsert_doc) {
      api::Val wrapper;
      wrapper.kind = *patch.upsert_doc;
      start = valJson(wrapper);
    } else {
      start = "{\"id\":" + idJson + "}";
    }
    PatchDoc doc(start);
    for (const auto& cond : patch.when) {
      if (!holds(doc, cond)) {
        out.status = Status::SKIPPED;
        return out;
      }
    }
    if (patch.delete_doc) {
      out.status = Status::DELETED;
      return out;
    }
    if (!(create && patch.upsert_doc)) {
      for (const auto& op : patch.ops) applyOp(doc, op);
    }
    if (const auto* id = doc.get("id")) {
      if (!idMatches(doc.text(*id), patch.id)) throw PatchError("the patch would change the document id");
    } else {
      doc.set("id", idJson);
    }
    out.body = doc.serialize();
    if (!create && out.body == start) {
      out.status = Status::NOOP;
      out.body.clear();
      return out;
    }
    out.status = create ? Status::CREATED : Status::UPDATED;
  } catch (const PatchError& e) {
    out.status = Status::ERROR;
    out.error = e.what();
    out.body.clear();
  }
  return out;
}

}  // namespace luxir::source

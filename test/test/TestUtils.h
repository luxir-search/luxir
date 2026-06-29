#pragma once

#include <array>
#include <map>
#include <string>
#include "SoluxTest.h"
#include "solux/index/IndexWriter.h"
#include "solux/util/Overloaded.h"

namespace solux::test {

// Idea: think of making a reference-version (i.e. std::string_view, std::span) of this class for use in main code.

using FieldVal = std::variant<bool, int64_t, float, double, std::string,
                              std::vector<bool>, std::vector<int64_t>, std::vector<float>, std::vector<double>, std::vector<std::string>,
                              std::vector<std::vector<float>>  // multi-valued vectors (one float-array per slot)
>;

struct NameVal {
  std::string name;
  FieldVal val;
};

using Doc = std::vector<NameVal>;

// Linear lookup for a field by name. Returns nullptr if not present.
// Docs are tiny (a handful of fields), so a scan beats hashing.
inline const FieldVal* find(const Doc& doc, std::string_view name) {
  for (const auto& nv : doc) {
    if (nv.name == name) return &nv.val;
  }
  return nullptr;
}

template <typename... Args>
constexpr auto arr(Args&&... args) {
  return std::to_array({std::forward<Args>(args)...});
}

// Make an array of int64_t from a list of any integral values.
// This can be necessary since there are no integer literals in C++ that are guaranteed to be int64_t.
template <typename... Args>
constexpr auto arr_i(Args... args) -> std::array<int64_t, sizeof...(Args)> {
  return { { static_cast<int64_t>(args)... } };
}

template<typename... Args>
auto vec(Args&&... args) {
  return std::vector{std::forward<Args>(args)...};
}

// Make a vec_i (vector of int64_t) from a list of any integral values.
// This can be necessary since there are no integer literals in C++ that are guaranteed to be int64_t.
template<typename... Args>
auto vec_i(Args&&... args) {
  return std::vector<int64_t>{static_cast<int64_t>(args)...};
}

// make a vector of string (as opposed to string_view or const char*)
template<typename... Args>
auto vecs(Args&&... args) {
  return std::vector<std::string>{std::forward<Args>(args)...};
}

// make a vector of string_view (as opposed to string or const char*)
template<typename... Args>
auto vecsv(Args&&... args) {
    return std::vector<std::string_view>{std::forward<Args>(args)...};
}

// allow construction of a Doc with just alternating names and values. Example:
// auto doc1 = flatdoc("name1", 1, "name2", 2.0, "name3", "hi");
template<typename T1, typename T2, typename... Args>
Doc flatdoc(T1 arg1, T2 arg2, Args... args) {
  std::vector<NameVal> vec;
  vec.push_back(NameVal{arg1, arg2});
  if constexpr (sizeof...(args) > 0) {
    auto otherPairs = flatdoc(args...);
    vec.insert(vec.end(), otherPairs.begin(), otherPairs.end());
  }
  return vec;
};


using solux::overloaded;  // the shared std::visit visitor (solux/util/Overloaded.h)

// Utility function to compare Doc objects for testing
inline bool docEquals(const Doc& doc1, const Doc& doc2) {
  if (doc1.size() != doc2.size()) {
    return false;
  }

  // Fast path assuming the docs have the same order of fields.
  // If they do, we can compare them directly without creating maps.
  // If they don't then create maps for comparison to avoid O(n^2) complexity.
  size_t i = 0;
  for (; i < doc1.size(); ++i) {
    if (doc1[i].name != doc2[i].name) {
      break;
    }
    if (doc1[i].val != doc2[i].val) {
      return false; // values don't match.
    }
  }
  if (i == doc1.size()) {
    return true; // All fields matched in order
  }
  
  // Create a map for easier comparison (field name -> value)
  boost::unordered_flat_map<std::string_view, const FieldVal*> map1, map2;
  for (auto& nv : doc1) {
    map1[nv.name] = &nv.val;
  }

  // iterate over the second doc, looking up in the first map
  for (const auto& [name,val] : doc2) {
    auto it = map1.find(name);
    if (it == map1.end()) {
      return false;
    }
    if (*(it->second) != val) {
      return false;
    }
  }
  return true;
}

// Function to find a Doc in a vector that matches the given doc
inline bool containsDoc(const std::vector<Doc>& docs, const Doc& target) {
  for (const auto& doc : docs) {
    if (docEquals(doc, target)) {
      return true;
    }
  }
  return false;
}

// Function to print a Doc for debugging
inline std::string docToString(const Doc& doc) {
  std::string result = "{";
  bool first = true;
  for (const auto& nv : doc) {
    if (!first) result += ", ";
    first = false;
    
    result += nv.name + ": ";
    std::visit(overloaded{
      [&](bool v) { result += (v ? "true" : "false"); },
      [&](int64_t v) { result += std::to_string(v); },
      [&](float v) { result += std::to_string(v); },
      [&](double v) { result += std::to_string(v); },
      [&](const std::string& v) { result += "\"" + v + "\""; },
      [&](const std::vector<bool>& v) { 
        result += "[";
        for (size_t i = 0; i < v.size(); ++i) {
          if (i > 0) result += ",";
          result += (v[i] ? "true" : "false");
        }
        result += "]";
      },
      [&](const std::vector<int64_t>& v) { 
        result += "[";
        for (size_t i = 0; i < v.size(); ++i) {
          if (i > 0) result += ",";
          result += std::to_string(v[i]);
        }
        result += "]";
      },
      [&](const std::vector<float>& v) { 
        result += "[";
        for (size_t i = 0; i < v.size(); ++i) {
          if (i > 0) result += ",";
          result += std::to_string(v[i]);
        }
        result += "]";
      },
      [&](const std::vector<double>& v) { 
        result += "[";
        for (size_t i = 0; i < v.size(); ++i) {
          if (i > 0) result += ",";
          result += std::to_string(v[i]);
        }
        result += "]";
      },
      [&](const std::vector<std::string>& v) {
        result += "[";
        for (size_t i = 0; i < v.size(); ++i) {
          if (i > 0) result += ",";
          result += "\"" + v[i] + "\"";
        }
        result += "]";
      },
      [&](const std::vector<std::vector<float>>& v) {
        result += "[";
        for (size_t i = 0; i < v.size(); ++i) {
          if (i > 0) result += ",";
          result += "[";
          for (size_t j = 0; j < v[i].size(); ++j) {
            if (j > 0) result += ",";
            result += std::to_string(v[i][j]);
          }
          result += "]";
        }
        result += "]";
      }
    }, nv.val);
  }
  result += "}";
  return result;
}

// Print a list of Docs (one per line) for debugging / assertion messages.
inline std::string docsToString(const std::vector<Doc>& docs) {
  std::string result;
  for (const auto& d : docs) {
    result += "  " + docToString(d) + "\n";
  }
  return result;
}

} // solux::test

// Expect that `docs` contains a doc matching `doc`; on failure prints the expected doc and
// the full actual doc list via docToString().
#define EXPECT_CONTAINS_DOC(docs, doc)                                                      \
  EXPECT_TRUE(::solux::test::containsDoc((docs), (doc)))                                    \
      << "expected doc: " << ::solux::test::docToString(doc) << "\nactual docs:\n"          \
      << ::solux::test::docsToString(docs)

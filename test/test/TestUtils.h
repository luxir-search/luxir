#pragma once

#include <array>
#include "SoluxTest.h"
#include "solux/index/IndexWriter.h"

namespace solux::test {

// Idea: think of making a reference-version (i.e. std::string_view, std::span) of this class for use in main code.

using FieldVal = std::variant<bool, int64_t, float, double, std::string,
                              std::vector<bool>, std::vector<int64_t>, std::vector<float>, std::vector<double>, std::vector<std::string>
>;

struct NameVal {
  std::string name;
  FieldVal val;
};

using Doc = std::vector<NameVal>;

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


template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

} // solux::test
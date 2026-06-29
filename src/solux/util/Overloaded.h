#pragma once

// The classic std::visit visitor: combine a set of lambdas into one overload set.
//
//   std::visit(solux::overloaded{
//     [&](const Match& m)  { ... },
//     [&](const KnnQuery& k) { ... },
//   }, query.kind);
//
// Prefer this over a std::get_if if-chain for dispatching a std::variant oneof: std::visit
// is EXHAUSTIVE - if a variant alternative has no matching lambda the code does not compile,
// so adding a new oneof arm forces every dispatch site to handle it (or explicitly ignore
// it) instead of silently falling through.

namespace solux {

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace solux

#pragma once

#include <algorithm>
#include <assert.h>
#include <cstring>
#include <initializer_list>
#include <stdint.h>
#include <string>
#include <string_view>

namespace solux {

// Byte-wise banded Levenshtein automaton for maxEdits <= 2.
class LevenshteinAutomaton final {
public:
  static constexpr int DEAD = -1;

  struct State {
    uint16_t j;
    uint8_t cells[5];
    bool prefixMatched;
    uint8_t bestPrefixDist;
  };

private:
  std::string_view query;
  int k;
  int n;
  bool prefixMode;

  uint8_t dead() const { return (uint8_t)(k + 1); }

  uint8_t clamp(int v) const {
    return (uint8_t)(v > k + 1 ? k + 1 : v);
  }

  uint8_t queryByte(int i) const {
    return (uint8_t)query[(size_t)i];
  }

  void fillDead(State& s) const {
    uint8_t v = dead();
    for (int i = 0; i < 5; i++) s.cells[i] = v;
  }

  int index(const State& s, int i) const {
    return i - ((int)s.j - k);
  }

  int cell(const State& s, int i) const {
    if (i < 0 || i > n) return k + 1;
    int idx = index(s, i);
    if (idx < 0 || idx > 2 * k) return k + 1;
    return (int)s.cells[idx];
  }

  void setCell(State& s, int i, int v) const {
    if (i < 0 || i > n) return;
    int idx = index(s, i);
    if (idx < 0 || idx > 2 * k) return;
    s.cells[idx] = clamp(v);
  }

  int minCell(const State& s) const {
    int best = k + 1;
    for (int i = 0; i <= 2 * k; i++) best = std::min(best, (int)s.cells[i]);
    return best;
  }

public:
  LevenshteinAutomaton(std::string_view query, int maxEdits, bool prefixMode = false)
    : query(query), k(maxEdits), n((int)query.size()), prefixMode(prefixMode) {
    assert(k >= 0 && k <= 2);
  }

  State start() const {
    State s;
    s.j = 0;
    fillDead(s);
    s.prefixMatched = false;
    s.bestPrefixDist = dead();
    for (int i = 0; i <= n && i <= k; i++) setCell(s, i, i);
    if (prefixMode) {
      int dist = cell(s, n);
      s.prefixMatched = dist <= k;
      s.bestPrefixDist = (uint8_t)dist;
    }
    return s;
  }

  State step(const State& s, uint8_t b) const {
    assert(s.j < UINT16_MAX);
    State d;
    d.j = (uint16_t)(s.j + 1);
    fillDead(d);
    d.prefixMatched = false;
    d.bestPrefixDist = dead();

    int lo = std::max(0, (int)d.j - k);
    int hi = std::min(n, (int)d.j + k);
    for (int i = lo; i <= hi; i++) {
      int v;
      if (i == 0) {
        v = (int)d.j;
      } else {
        int sub = cell(s, i - 1) + (b == queryByte(i - 1) ? 0 : 1);
        int ins = cell(s, i) + 1;
        int del = cell(d, i - 1) + 1;
        v = std::min({sub, ins, del});
      }
      setCell(d, i, v);
    }

    if (prefixMode) {
      int dist = cell(d, n);
      int best = std::min((int)s.bestPrefixDist, dist);
      d.prefixMatched = s.prefixMatched || dist <= k;
      d.bestPrefixDist = clamp(best);
    }
    return d;
  }

  bool canMatch(const State& s) const {
    return s.prefixMatched || minCell(s) <= k;
  }

  bool isMatch(const State& s) const {
    return prefixMode ? s.prefixMatched : cell(s, n) <= k;
  }

  int matchDistance(const State& s) const {
    return prefixMode ? (int)s.bestPrefixDist : cell(s, n);
  }

  int nextLiveByte(const State& s, int b0) const {
    if (b0 < 0) b0 = 0;
    if (b0 > 255 || !canMatch(s)) return DEAD;
    if (s.prefixMatched || minCell(s) < k) return b0;

    int answer = DEAD;
    int lo = std::max(0, (int)s.j - k);
    int hi = std::min(n, (int)s.j + k);
    for (int i = lo; i <= hi && i < n; i++) {
      if (cell(s, i) != k) continue;
      int b = (int)queryByte(i);
      if (b >= b0 && (answer == DEAD || b < answer)) answer = b;
    }
    return answer;
  }

};

} // namespace solux

#pragma once
#include "luxir/util/automaton/Automaton.h"
namespace luxir::automaton {
Automaton utf32ToUtf8(const Automaton& utf32, Budget& budget);
}

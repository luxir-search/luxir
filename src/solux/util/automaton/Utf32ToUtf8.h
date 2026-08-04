#pragma once
#include "solux/util/automaton/Automaton.h"
namespace solux::automaton {
Automaton utf32ToUtf8(const Automaton& utf32, Budget& budget);
}

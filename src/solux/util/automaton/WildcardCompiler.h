#pragma once
#include <string_view>
#include "solux/util/automaton/ByteDfa.h"
#include "solux/util/automaton/CodepointFolder.h"
#include "solux/util/automaton/Utf32ToUtf8.h"
namespace solux::automaton {
ByteDfa compileWildcard(std::string_view pattern, Budget& budget,
                        const CodepointFolder* folder = nullptr);
}

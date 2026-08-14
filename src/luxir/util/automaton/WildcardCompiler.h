#pragma once
#include <string_view>
#include "luxir/util/automaton/ByteDfa.h"
#include "luxir/util/automaton/CodepointFolder.h"
#include "luxir/util/automaton/Utf32ToUtf8.h"
namespace luxir::automaton {
ByteDfa compileWildcard(std::string_view pattern, Budget& budget,
                        const CodepointFolder* folder = nullptr);
}

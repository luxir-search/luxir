#pragma once

#include <string_view>

#include "solux/util/automaton/ByteDfa.h"

namespace solux::automaton {

// Compiles the restricted Lucene regular-expression subset to a whole-term
// byte DFA. Caret and dollar are ordinary literal codepoints. Bare braces are
// errors: escape `{` or `}` to use them literally.
ByteDfa compileRegex(std::string_view pattern, Budget& budget);

} // namespace solux::automaton

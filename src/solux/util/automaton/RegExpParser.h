#pragma once

#include <string_view>

#include "solux/util/automaton/ByteDfa.h"
#include "solux/util/automaton/CodepointFolder.h"

namespace solux::automaton {

// Compiles the restricted Lucene regular-expression subset to a whole-term
// byte DFA. Literal characters fold the way the field folds text; operator
// constructs, character classes, and ranges are codepoint-exact. Caret and
// dollar are ordinary literal codepoints. Bare braces are errors: escape `{`
// or `}` to use them literally.
ByteDfa compileRegex(std::string_view pattern, Budget& budget,
                     const CodepointFolder* folder = nullptr);

} // namespace solux::automaton

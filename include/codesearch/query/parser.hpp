#pragma once

#include <optional>
#include <stdexcept>
#include <string_view>

#include "codesearch/query/ast.hpp"

namespace codesearch {

// A query the user typed that we can't understand. The message says why.
class QueryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ParsedQuery {
    QueryNode root;
    std::optional<bool> caseSensitive;  // set by case:yes or case:no
};

// Parses the query language (see "Query language" in docs/ARCHITECTURE.md):
//
//   foo bar          both, anywhere in the same file
//   foo or bar       either one ("OR" works too)
//   -foo             files without foo
//   (a or b) c       parentheses group
//   "foo bar"        quotes keep spaces and special words; \" and \\ inside
//   \(  \-  \"       a backslash makes the next character plain text
//   repo:name  path:text  file:text  lang:go  case:yes  case:no
//
// Throws QueryError if the query is empty, unbalanced, nested too deeply,
// or uses a filter wrongly.
ParsedQuery parseQuery(std::string_view text);

}  // namespace codesearch

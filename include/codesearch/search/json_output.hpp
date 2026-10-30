#pragma once

#include <ostream>
#include <string>
#include <string_view>

#include "codesearch/search/repo_search.hpp"

namespace codesearch {

// "complete", "truncated" or "cancelled".
std::string_view statusName(SearchStatus status);

// One result file as a JSON object, on one line:
//
//   {"repo":"demo","path":"a.txt","lines":[
//     {"line":2,"text":"second Target line","matches":[{"start":7,"end":13}]}]}
//
// "start" and "end" are byte offsets inside the line's original bytes. JSON
// text must be valid UTF-8, so if a path or line isn't, its bad bytes become
// U+FFFD and "path_base64" or "text_base64" holds the exact bytes.
std::string resultFileJson(const ResultFile& file);

// Writes the results as one JSON document, a file at a time, so they never
// have to be held in memory:
//
//   {"files":[{...},{...}],"status":"complete"}
class JsonResultWriter {
public:
    explicit JsonResultWriter(std::ostream& out);

    void file(const ResultFile& file);
    void finish(SearchStatus status);

private:
    std::ostream* out_;  // a pointer, not a reference, so the class can be assigned
    bool first_ = true;
};

}  // namespace codesearch

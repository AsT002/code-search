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
//
// Before "status" may come "skipped" (repos left out) and "warnings"
// (things in the query that are probably mistakes), each only if not empty.
//
// If the search fails partway, fail() ends the document instead, so it's
// still whole JSON:
//
//   {"files":[{...}],"status":"error","error":"demo: index file is damaged: ..."}
class JsonResultWriter {
public:
    explicit JsonResultWriter(std::ostream& out);

    void file(const ResultFile& file);
    // A repo left out of the search because its index is damaged. Listed
    // in "skipped", after the files, when the document ends.
    void skipped(std::string_view repo, std::string_view message);
    // Something in the query that's probably a mistake. Listed in
    // "warnings", after "skipped", when the document ends.
    void warning(std::string_view message);
    void finish(SearchStatus status);
    // Ends the document with "status":"error" and the message. Bytes of the
    // message that aren't valid UTF-8 become U+FFFD.
    void fail(std::string_view message);

private:
    // Ends the "files" list, and writes "skipped" and "warnings" if there
    // are any.
    void endFiles();

    std::ostream* out_;  // a pointer, not a reference, so the class can be assigned
    bool first_ = true;
    std::string skipped_;   // the "skipped" entries so far, as JSON
    std::string warnings_;  // the "warnings" entries so far, as JSON
};

}  // namespace codesearch

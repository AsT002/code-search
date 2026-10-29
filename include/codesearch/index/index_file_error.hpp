#pragma once

#include <stdexcept>

namespace codesearch {

// An index file that is damaged, or in a format this version can't read.
class IndexFileError : public std::runtime_error {
public:
    // Reuse runtime_error's constructors, so IndexFileError("message") works.
    using std::runtime_error::runtime_error;
};

}  // namespace codesearch

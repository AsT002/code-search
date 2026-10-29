#pragma once

#include <optional>
#include <stdexcept>
#include <string>

#include "codesearch/util/object_id.hpp"

namespace codesearch::testing {

// A made-up object ID: 40 copies of `digit`, which must be 0-9 or a-f.
inline ObjectId fakeObjectId(char digit) {
    const std::optional<ObjectId> id = ObjectId::fromHex(std::string(40, digit));
    if (!id.has_value()) {
        throw std::invalid_argument("not a hex digit");
    }
    return id.value();
}

}  // namespace codesearch::testing

#include "codesearch/util/object_id.hpp"

namespace codesearch {

std::optional<ObjectId> ObjectId::fromHex(std::string_view hex) {
    if (hex.size() != 40) {
        return std::nullopt;
    }
    for (const char c : hex) {
        const bool digit = c >= '0' && c <= '9';
        const bool letter = c >= 'a' && c <= 'f';
        if (!digit && !letter) {
            return std::nullopt;
        }
    }
    return ObjectId(std::string(hex));
}

}  // namespace codesearch

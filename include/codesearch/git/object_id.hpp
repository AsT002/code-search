#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace codesearch {

// A full SHA-1 object ID from Git, stored as 40 lowercase hex characters.
class ObjectId {
public:
    // Returns nothing unless `hex` is exactly 40 lowercase hex characters.
    static std::optional<ObjectId> fromHex(std::string_view hex);

    const std::string& hex() const { return hex_; }

    friend bool operator==(const ObjectId&, const ObjectId&) = default;

private:
    explicit ObjectId(std::string hex) : hex_(std::move(hex)) {}

    std::string hex_;
};

}  // namespace codesearch

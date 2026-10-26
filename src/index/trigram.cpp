#include "codesearch/index/trigram.hpp"

#include <algorithm>

#include "codesearch/util/ascii.hpp"

namespace codesearch {

std::vector<Trigram> extractTrigrams(std::string_view text) {
    std::vector<Trigram> trigrams;
    if (text.size() < 3) {
        return trigrams;
    }

    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        trigrams.push_back(
            packTrigram(foldAscii(text[i]), foldAscii(text[i + 1]), foldAscii(text[i + 2])));
    }

    // Sort, then drop repeats. std::unique moves the repeats to the end and
    // returns where they start; erase removes them.
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
    return trigrams;
}

}  // namespace codesearch

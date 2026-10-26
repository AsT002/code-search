#include <iostream>
#include <span>
#include <string_view>

#include "codesearch/version.hpp"

namespace {

void printUsage(std::ostream& out) { out << "usage: code-search [--version] [--help]\n"; }

}  // namespace

int main(int argc, char* argv[]) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));

    if (args.size() == 2) {
        const std::string_view arg = args.back();
        if (arg == "--version") {
            std::cout << "code-search " << codesearch::version() << '\n';
            return 0;
        }
        if (arg == "--help") {
            printUsage(std::cout);
            return 0;
        }
    }

    printUsage(std::cerr);
    return 1;
}

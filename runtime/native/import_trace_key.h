#pragma once
#include <array>
#include <string>
#include <string_view>

namespace Doritos::Native {
// Borrowed only for the duration of one lookup. The set continues to own the
// exact library + '!' + name string used by the original logging identity.
struct ImportTraceKey {
    std::string_view library;
    std::string_view name;
};

struct ImportTraceKeyLess {
    using is_transparent = void;

    static int compare(ImportTraceKey left, std::string_view right) noexcept {
        size_t offset = 0;
        for (const auto part : std::array<std::string_view, 3>{left.library, "!", left.name}) {
            const auto prefix = right.substr(offset, part.size());
            const int order = part.substr(0, prefix.size()).compare(prefix);
            if (order) return order;
            if (prefix.size() < part.size()) return 1;
            // The preceding check proves this addition stays within right.
            offset += part.size();
        }
        return offset < right.size() ? -1 : 0;
    }
    bool operator()(const std::string& left, const std::string& right) const noexcept {
        return left < right;
    }
    bool operator()(ImportTraceKey left, const std::string& right) const noexcept {
        return compare(left, right) < 0;
    }
    bool operator()(const std::string& left, ImportTraceKey right) const noexcept {
        return compare(right, left) > 0;
    }
};
}

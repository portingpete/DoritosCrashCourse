#include "runtime/native/import_trace_key.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
std::atomic<bool> countAllocations{false};
std::atomic<size_t> allocationCount{0};
size_t checks = 0;
void require(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
}
void* operator new(size_t bytes) {
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { std::free(pointer); }

int main() {
    try {
        using namespace Doritos::Native;
        const ImportTraceKeyLess less;
        const std::vector<std::string> values{
            "", "!", "!!", "a", "a!", "!a", "ab", "abc", "abcd",
            "xboxkrnl.exe", "RtlEnterCriticalSection", std::string(130, 'x'),
            std::string("a\0b", 3), std::string(1, char(0x80)), std::string(1, char(0xff))
        };
        std::set<std::string, ImportTraceKeyLess> actual;
        std::set<std::string> oracle;
        // The independent oracle materializes the old exact concatenation.
        for (const auto& library : values) for (const auto& name : values) {
            const ImportTraceKey key{library, name};
            const std::string composite = library + "!" + name;
            for (const auto& candidate : values) {
                require(less(key, candidate) == (composite < candidate), "borrowed/owned lexicographic ordering");
                require(less(candidate, key) == (candidate < composite), "owned/borrowed lexicographic ordering");
            }
            const bool previouslyPresent = oracle.find(composite) != oracle.end();
            require((actual.find(key) != actual.end()) == previouslyPresent, "lookup preserves exact prior identity");
            actual.insert(composite);
            oracle.insert(composite);
        }
        require(actual.size() == oracle.size(), "delimiter collisions retain one owned identity");
        require(std::equal(actual.begin(), actual.end(), oracle.begin(), oracle.end()), "ordered owned values match old set");
        actual.insert("a!!b");
        const auto collision = actual.find(ImportTraceKey{"a!", "b"});
        require(collision != actual.end() && collision == actual.find(ImportTraceKey{"a", "!b"}), "delimiter collision equality");

        std::string library = "changing-library", name = "changing-name";
        actual.insert(library + "!" + name);
        library.assign("different-library"); name.assign("different-name");
        require(actual.find(ImportTraceKey{"changing-library", "changing-name"}) != actual.end(), "stored identity owns caller bytes");
        require(actual.find(ImportTraceKey{library, name}) == actual.end(), "lookup uses current values rather than pointer identity");

        const ImportTraceKey hot{"xboxkrnl.exe", "RtlEnterCriticalSection"};
        const ImportTraceKey absent{"never-added-library", "never-added-name"};
        allocationCount.store(0);
        countAllocations.store(true);
        bool consistent = true;
        for (size_t index = 0; index < 10000; ++index) {
            consistent &= actual.find(hot) != actual.end();
            consistent &= actual.find(absent) == actual.end();
        }
        countAllocations.store(false);
        require(consistent, "repeated present and absent borrowed lookups are stable");
        require(allocationCount.load() == 0, "steady lookups allocate no owned strings or nodes");
        std::printf("Native import trace contract passed: %zu checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        countAllocations.store(false);
        std::fprintf(stderr, "Native import trace contract failed: %s\n", error.what());
        return 1;
    }
}

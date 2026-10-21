#include "tests/test.hpp"
#include "oldbox/cpu_exception.hpp"

namespace oldbox::test {
int failures = 0;
void fail(const char* file, int line, const std::string& msg) {
    ++failures;
    std::fprintf(stderr, "%s:%d: FAIL: %s\n", file, line, msg.c_str());
}
std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
}

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : std::string();
    for (auto& t : oldbox::test::registry()) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
        std::printf("[ RUN ] %s\n", t.name);
        try {
            t.fn();
        } catch (const oldbox::CpuException& e) {
            oldbox::test::fail(__FILE__, __LINE__, std::format("{}: uncaught CPU exception {} err {}", t.name, e.vector, e.error));
        } catch (const std::exception& e) {
            oldbox::test::fail(__FILE__, __LINE__, std::string(t.name) + ": uncaught exception: " + e.what());
        }
    }
    if (oldbox::test::failures) {
        std::printf("%d failure(s)\n", oldbox::test::failures);
        return 1;
    }
    std::printf("all tests passed (%zu)\n", oldbox::test::registry().size());
    return 0;
}

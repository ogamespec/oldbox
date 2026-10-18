#pragma once
#include "oldbox/common.hpp"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

namespace oldbox::test {
struct TestCase { const char* name; void (*fn)(); };
std::vector<TestCase>& registry();
extern int failures;
void fail(const char* file, int line, const std::string& msg);
}

#define TEST(name) \
    static void name(); \
    static bool reg_##name = ([] { oldbox::test::registry().push_back({#name, name}); return true; })(); \
    static void name()

#define CHECK(cond) do { if (!(cond)) oldbox::test::fail(__FILE__, __LINE__, #cond); } while (0)
#define CHECK_EQ(a,b) do { auto va=(a); auto vb=(b); if (!(va==vb)) oldbox::test::fail(__FILE__, __LINE__, std::format("{} != {}", va, vb)); } while (0)

// 文件路径: tests/framework.cpp
// 职责: 测试框架实现 —— 注册表、失败记录、运行器(输出逐条结果与失败列表)。
// 设计要点: 全部用 stdio(printf)输出,不引入 iostream;失败信息集中打印,
//           main 返回失败数,配合 ctest 的退出码判定。
#include "framework.h"

#include <cstdio>
#include <cstring>
#include <thread>

namespace estest {
namespace {

// 失败记录(单条断言失败)
struct Failure
{
    std::string test;      // 所属用例名
    std::string file;      // 源文件
    int line = 0;          // 行号
    std::string expr;      // 断言表达式文本
    std::string msg;       // 详细描述(含期望/实际值)
};

std::vector<Failure>& failures()
{
    static std::vector<Failure> s_failures;
    return s_failures;
}

std::string& current()
{
    static std::string s_current = "(未开始)";
    return s_current;
}

} // namespace

std::vector<TestCase>& registry()
{
    static std::vector<TestCase> s_registry;
    return s_registry;
}

std::string currentTest()
{
    return current();
}

void reportFailure(const char* file, int line, const std::string& expr, const std::string& msg)
{
    failures().push_back(Failure{current(), file != nullptr ? file : "", line, expr, msg});
}

int runAll()
{
    const std::vector<TestCase>& cases = registry();
    int failed = 0;

    std::printf("==== EdgeGate 测试框架: 共 %zu 个用例 ====\n", cases.size());
    for (const TestCase& tc : cases)
    {
        current() = tc.name;
        const size_t before = failures().size();
        tc.fn();
        const bool ok = (failures().size() == before);
        if (!ok)
        {
            ++failed;
        }
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", tc.name);
    }

    // 输出失败明细(文件:行 | 用例 | 表达式 → 描述)
    if (!failures().empty())
    {
        std::printf("\n---- 失败列表(%zu 条断言失败) ----\n", failures().size());
        for (const Failure& f : failures())
        {
            std::printf("  %s:%d [%s] %s\n      -> %s\n", f.file.c_str(), f.line,
                        f.test.c_str(), f.expr.c_str(), f.msg.c_str());
        }
    }

    const size_t passed = cases.size() - static_cast<size_t>(failed);
    std::printf("\n结果: %zu 通过, %d 失败, 断言失败 %zu 条\n", passed, failed,
                failures().size());
    return failed; // 退出码 = 失败用例数(0 = 全绿,ctest 判定通过)
}

std::string toStr(bool v)
{
    return v ? "true" : "false";
}

std::string toStr(int v)
{
    return std::to_string(v);
}

std::string toStr(long v)
{
    return std::to_string(v);
}

std::string toStr(long long v)
{
    return std::to_string(v);
}

std::string toStr(unsigned v)
{
    return std::to_string(v);
}

std::string toStr(unsigned long v)
{
    return std::to_string(v);
}

std::string toStr(unsigned long long v)
{
    return std::to_string(v);
}

std::string toStr(float v)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return buf;
}

std::string toStr(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

std::string toStr(const std::string& v)
{
    return "\"" + v + "\"";
}

std::string toStr(const char* v)
{
    return v != nullptr ? std::string("\"") + v + "\"" : "nullptr";
}

std::string toStr(char v)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "'%c'", v);
    return buf;
}

bool waitUntil(uint64_t timeoutMs, const std::function<bool()>& predicate)
{
    if (predicate == nullptr)
    {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate(); // 最后一搏: 截止时刻恰好满足也算成功
}

void sleepMs(uint64_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace estest

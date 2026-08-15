// 文件路径: tests/framework.h
// 职责: 注册式极简测试框架(零外部依赖,禁异常)。
//       用法: ES_TEST(用例名) { ... CHECK / CHECK_EQ / CHECK_NEAR ... }
//       每个 test_*.cpp 编译为独立可执行文件,exit code = 失败数(0 = 全绿)。
//
// 设计要点(面试可讲):
// 1) 注册式: 静态对象 TestRegistrar 在 main 之前把用例登记进全局注册表
//    (registry() 用函数局部 static,懒初始化且线程安全;跨翻译单元的静态初始化
//    顺序无关 —— 我们只关心"main 之前都注册完")。
// 2) 失败收集而非立即 abort: 断言失败仅记录(文件/行号/表达式/期望与实际值),
//    继续执行后续断言 —— 一次运行暴露全部失败,不必反复编译重跑;
//    CHECK_EQ/CHECK_NEAR 用"求值一次 + 类型安全比较"实现,不吃宏二重求值的亏。
// 3) 禁异常: 断言失败不 throw,只记账;main 返回失败数,ctest 凭退出码判定红绿。
// 4) 值打印: 数字用 snprintf("%.9g") 最短可读形式,字符串带引号,避免 iostream
//    依赖(与工程 core 禁 iostream 的风格一致)。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace estest {

// 一个已注册的测试用例(名称 + 无参函数)
struct TestCase
{
    const char* name;
    void (*fn)();
};

// 全局注册表(懒初始化单例,跨翻译单元共享)
std::vector<TestCase>& registry();

// 当前正在运行的用例名(失败报告中显示上下文)
std::string currentTest();

// 记录一条断言失败(供 CHECK 系列宏调用)
void reportFailure(const char* file, int line, const std::string& expr, const std::string& msg);

// 运行全部用例;输出逐条 [PASS]/[FAIL] 与失败列表;返回失败数(0 = 全绿)
int runAll();

// 值 → 可读字符串(断言失败时打印期望/实际值)
std::string toStr(bool v);
std::string toStr(int v);
std::string toStr(long v);
std::string toStr(long long v);
std::string toStr(unsigned v);
std::string toStr(unsigned long v);
std::string toStr(unsigned long long v);
std::string toStr(float v);
std::string toStr(double v);
std::string toStr(const std::string& v);
std::string toStr(const char* v);
std::string toStr(char v);
// 兜底: 未知类型打印占位(不引入 iostream,编译期由重载优先选择具体类型)
template <typename T>
std::string toStr(const T&)
{
    return "<值>";
}

// 注册器: 静态对象构造时把用例加入注册表
class TestRegistrar
{
public:
    TestRegistrar(const char* name, void (*fn)())
    {
        registry().push_back(TestCase{name, fn});
    }
};

// 通用比较: a == b
template <typename A, typename B>
bool checkEqImpl(const A& a, const B& b, const char* aText, const char* bText,
                 const char* file, int line)
{
    if (!(a == b))
    {
        const std::string expr = std::string(aText) + " == " + bText;
        reportFailure(file, line, expr,
                      expr + " 失败: 左=" + toStr(a) + ", 右=" + toStr(b));
        return false;
    }
    return true;
}

// 近似比较: |a - b| <= eps(浮点)
template <typename A, typename B>
bool checkNearImpl(const A& a, const B& b, double eps, const char* aText, const char* bText,
                   const char* file, int line)
{
    const double diff = static_cast<double>(a) - static_cast<double>(b);
    if (!(diff <= eps && diff >= -eps))
    {
        const std::string expr = std::string(aText) + " ≈ " + bText;
        reportFailure(file, line, expr,
                      expr + " 失败: 左=" + toStr(a) + ", 右=" + toStr(b) +
                          ", 容差=" + toStr(eps));
        return false;
    }
    return true;
}

// 通用断言: 条件为假即失败
template <typename Cond>
bool checkImpl(const Cond& cond, const char* exprText, const char* file, int line)
{
    if (!cond)
    {
        reportFailure(file, line, exprText, "条件为假");
        return false;
    }
    return true;
}

// 异步测试辅助: 最多等待 timeoutMs 毫秒直到 predicate 为真(轮询睡眠 5ms)
bool waitUntil(uint64_t timeoutMs, const std::function<bool()>& predicate);

// 睡眠指定毫秒(测试中模拟等待/节奏控制)
void sleepMs(uint64_t ms);

} // namespace estest

// 注册 + 定义用例: ES_TEST(名称) { ... }
#define ES_TEST(name)                                                           \
    static void es_test_fn_##name();                                            \
    static ::estest::TestRegistrar es_test_reg_##name(#name, &es_test_fn_##name); \
    static void es_test_fn_##name()

// 断言宏: 失败时记录文件/行号/表达式与期望值,继续执行
#define CHECK(cond) ::estest::checkImpl((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::estest::checkEqImpl((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) ::estest::checkNearImpl((a), (b), (eps), #a, #b, __FILE__, __LINE__)

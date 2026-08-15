// 文件路径: tests/test_main.cpp
// 职责: 测试入口 —— 运行注册的全部用例,退出码 = 失败数(0 = 全绿)。
//       每个 test_*.cpp 与 framework.cpp + 本文件链接为独立可执行文件,
//       ctest 通过退出码判断红绿;输出失败列表见 framework.cpp runAll()。
#include "framework.h"

int main()
{
    return estest::runAll();
}

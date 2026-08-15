# 文件路径: cmake/es_options.cmake
# 职责: EdgeGate 构建选项集中定义(契约 §13):
#   ES_BUILD_TESTS   单元测试(默认 ON;交叉编译目标板无 ctest 环境,默认 OFF)
#   ES_BUILD_QT_GUI  Qt 监控界面(默认 OFF;探测到 Qt5>=5.12 / Qt6 Widgets 才启用)
#   ES_BUILD_SIM     PTY 虚拟串口闭环演示(pty_pair + ModbusSlave;交叉编译
#                    也可 ON —— ARM 板上可做回环自检,不依赖真实串口硬件)
# 设计要点: 交叉编译识别用 CMake 内置 CMAKE_CROSSCOMPILING(由传入工具链文件
#           时自动置位),无需手工判断编译器前缀字符串。
include_guard(GLOBAL)

option(ES_BUILD_TESTS "构建单元测试(ctest)" ON)
option(ES_BUILD_QT_GUI "构建 Qt 监控界面(需 Qt5>=5.12 或 Qt6 Widgets)" OFF)
option(ES_BUILD_SIM "构建 PTY 模拟链路(pty_pair + ModbusSlave)" ON)

if(CMAKE_CROSSCOMPILING)
    # 交叉编译默认关测试: 目标板无 ctest,且本机无法运行目标架构可执行文件
    set(ES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif()

# ---- Qt 探测(QUIET: 未安装时降级为无 GUI,不打断配置流程) ----
set(ES_QT_TARGET "" CACHE INTERNAL "Qt Widgets 链接目标(找到时填充)")
if(ES_BUILD_QT_GUI)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(Qt6_FOUND)
        set(ES_QT_TARGET "Qt6::Widgets" CACHE INTERNAL "" FORCE)
        message(STATUS "EdgeGate GUI: 使用 Qt6 Widgets")
    else()
        find_package(Qt5 5.12 QUIET COMPONENTS Widgets)
        if(Qt5_FOUND)
            set(ES_QT_TARGET "Qt5::Widgets" CACHE INTERNAL "" FORCE)
            message(STATUS "EdgeGate GUI: 使用 Qt5 Widgets")
        else()
            message(WARNING
                "ES_BUILD_QT_GUI=ON 但未找到 Qt6 或 Qt5(>=5.12) Widgets,"
                "GUI 将不参与构建;请安装 qtbase5-dev 或 qt6-base-dev 后重试")
            set(ES_BUILD_QT_GUI OFF)
        endif()
    endif()
endif()

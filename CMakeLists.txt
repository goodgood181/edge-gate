# 文件路径: CMakeLists.txt
# 职责: EdgeGate 顶层构建(契约 §13)。
#   静态库拆分: es_core(core+util) / es_modbus(hal+modbus+edge) / es_net(net),
#   可执行: edge-gate(app)。拆分原则: 按依赖方向分层 —— 上层只依赖下层,
#   测试/可执行按需链接;任一库改动只重编该库与直接依赖者,增量编译友好。
cmake_minimum_required(VERSION 3.16)
project(edge_gate VERSION 1.0.0 LANGUAGES CXX)

# ---- 构建选项(契约 §13): ES_BUILD_TESTS / ES_BUILD_QT_GUI / ES_BUILD_SIM ----
include(cmake/es_options.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)   # 默认 Release(嵌入式部署以 Release 为准)
endif()

# 严格告警(契约 §13): -Wall -Wextra;仅 GCC/Clang 系(MSVC 不支持这两个选项)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

# 线程: 全部模块使用 std::thread / std::mutex,统一链接 pthread
find_package(Threads REQUIRED)

# ================= es_core: core + util =================
add_library(es_core STATIC
    src/core/logger.cpp
    src/core/event_bus.cpp
    src/core/state_machine.cpp
    src/core/thread_utils.cpp
    src/core/time_utils.cpp
    src/util/json.cpp
    src/util/config.cpp)
target_include_directories(es_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(es_core PUBLIC Threads::Threads)

# ================= es_modbus: hal + modbus + edge =================
# ES_BUILD_SIM=ON 时追加 pty_pair 与 ModbusSlave(x86 闭环演示/ARM 回环自检)
set(ES_MODBUS_SOURCES
    src/hal/posix_serial.cpp
    src/modbus/modbus_crc.cpp
    src/modbus/modbus_rtu.cpp
    src/modbus/modbus_master.cpp
    src/edge/signal_filter.cpp
    src/edge/rule_engine.cpp)
if(ES_BUILD_SIM)
    list(APPEND ES_MODBUS_SOURCES src/hal/pty_pair.cpp src/modbus/modbus_slave.cpp)
endif()
add_library(es_modbus STATIC ${ES_MODBUS_SOURCES})
target_include_directories(es_modbus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(es_modbus PUBLIC Threads::Threads)
if(ES_BUILD_SIM)
    # PUBLIC 传播: edge-gate / 测试链接 es_modbus 时自动获得该宏,
    # 应用层据此条件编译从站引用(gateway.h/cpp)
    target_compile_definitions(es_modbus PUBLIC ES_BUILD_SIM=1)
endif()

# ================= es_net: net =================
add_library(es_net STATIC
    src/net/mqtt_packets.cpp
    src/net/mqtt_client.cpp
    src/net/cmd_server.cpp
    src/net/http_server.cpp)
target_include_directories(es_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(es_net PUBLIC Threads::Threads)

# ================= edge-gate 可执行(app) =================
set(EDGE_GATE_SOURCES
    src/app/main.cpp
    src/app/gateway.cpp
    src/app/cli.cpp)
if(ES_BUILD_QT_GUI AND ES_QT_TARGET)
    # Qt GUI 模块: 仅 ES_BUILD_QT_GUI=ON 且探测到 Qt 时编译;
    # CMAKE_AUTOMOC 处理 Q_OBJECT 头文件(Qt5/Qt6 通用)
    list(APPEND EDGE_GATE_SOURCES
        src/ui/qt/curve_widget.cpp
        src/ui/qt/main_window.cpp
        src/ui/qt/ui_worker.cpp)
    set(CMAKE_AUTOMOC ON)
endif()
add_executable(edge-gate ${EDGE_GATE_SOURCES})
target_link_libraries(edge-gate PRIVATE es_core es_modbus es_net)
if(ES_BUILD_QT_GUI AND ES_QT_TARGET)
    target_link_libraries(edge-gate PRIVATE ${ES_QT_TARGET})
    target_compile_definitions(edge-gate PRIVATE ES_BUILD_QT_GUI=1)
endif()

# ================= 测试(契约 §14;tests/ 由 R2-2 Agent 交付) =================
if(ES_BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(tests)
    else()
        # 测试目录尚未交付时跳过,不影响主程序构建
        message(STATUS "tests/CMakeLists.txt 尚未交付(R2-2),测试暂不加入构建")
    endif()
endif()

# ================= install 规则 =================
install(TARGETS edge-gate RUNTIME DESTINATION bin)
install(FILES
    config/edge-gate.json
    config/edge-gate-arm.json
    DESTINATION share/edge-gate/config)
install(FILES scripts/edge-gate.service DESTINATION share/edge-gate/scripts)
install(PROGRAMS
    scripts/build.sh
    scripts/run-demo.sh
    scripts/install-mosquitto.sh
    scripts/deploy-arm.sh
    DESTINATION share/edge-gate/scripts)

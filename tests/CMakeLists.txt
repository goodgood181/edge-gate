# 文件路径: tests/CMakeLists.txt
# 职责: 契约 §14 测试策略 —— 每个 test_*.cpp 一个可执行文件 + add_test,
#       与 framework.cpp + test_main.cpp 链接(framework 注册式框架)。
# 说明: test_gateway.cpp 依赖 app 层(契约 §14);app 目录不存在时自动跳过,
#       R2-1 落地后无需改本文件即自动纳入。
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME}
        ${TEST_SRC}
        ${CMAKE_CURRENT_SOURCE_DIR}/framework.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE es_core es_modbus es_net Threads::Threads)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

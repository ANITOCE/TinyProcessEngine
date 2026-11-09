include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)

# Prevent GTest from installing itself / building GMock; force shared CRT
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# 顶层 CMakeLists.txt 已 enable_testing()，此处不再重复（缺陷⑥）

# Test executable
set(TPE_TEST_SOURCES
  test_value_type.cpp
  test_matches.cpp
  test_scanner.cpp
  test_session.cpp
  test_cli_parser.cpp
  test_value_parser.cpp
  test_repl_state.cpp
  # T009:远程协议帧编解码契约用例(骨架期 RED,T014 实现后转绿)
  test_remote_protocol.cpp
  # T010:远程日志格式化契约用例(骨架期 RED,T015 实现后转绿)
  test_remote_log.cpp
  # T011:远程 TPE> 命令分类/执行契约用例(骨架期 RED,T017 实现后转绿)
  test_remote_command_executor.cpp
  # Windows 写路径用例(US2/缺陷③):整文件 _WIN32 门控,其它平台为空翻译单元
  test_windows_memory.cpp
  # US6(缺陷⑧):CLI 枚举失败信号用例(runCliWithEngine 注入接缝)
  test_startup_cli.cpp
  # US6(缺陷⑧):CLI 装配层以源文件形式编入测试目标(该文件同时被 exe 目标编译,
  # 两目标互不冲突;注入接缝 runCliWithEngine 的声明见 src/startup/startup_cli.hpp)
  ${CMAKE_SOURCE_DIR}/src/startup/startup_cli.cpp
  # T012:远程启动参数与入口分流契约用例(骨架期 RED,T018/T024 实现后转绿)
  test_startup_remote.cpp
  # T020:远程会话回环握手用例(真实回环 TCP;骨架期 RED,T022/T023 实现后转绿)
  test_remote_session.cpp
  # T012:启动装配层以源文件形式编入测试目标(同样为 exe 目标源文件,两目标互不冲突;
  # startup.cpp 依赖 runCli(startup_cli.cpp)与 CliParser(TinyProcessEngineLib),
  # startup_remote.cpp 提供 parseRemoteStartupArgs/runRemote/setRemoteEntryForTesting)
  ${CMAKE_SOURCE_DIR}/src/startup/startup.cpp
  ${CMAKE_SOURCE_DIR}/src/startup/startup_remote.cpp
)

# Linux 集成测试（缺陷⑥）：仅 Linux 构建纳入源列表；Windows 下整文件被 __linux__ 包裹
if(UNIX)
  list(APPEND TPE_TEST_SOURCES test_linux_memory.cpp test_linux_process.cpp)
endif()

add_executable(tpe_tests ${TPE_TEST_SOURCES})

target_link_libraries(tpe_tests PRIVATE
  TinyProcessEngineLib
  PlatformLib
  gtest_main
)

target_include_directories(tpe_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/include/TinyProcessEngine
  ${CMAKE_SOURCE_DIR}/include/Platform
  # US6(缺陷⑧):startup_cli.hpp 所在目录(runCliWithEngine 声明)
  ${CMAKE_SOURCE_DIR}/src/startup
)

add_test(NAME tpe_tests COMMAND tpe_tests)

# Linux 集成测试辅助进程（缺陷⑥）：
# test_linux_memory 以 ./tests/tpe_test_target → tpe_test_target 相对路径 exec；
# 输出到 ${CMAKE_BINARY_DIR}/tests 以匹配 ctest 默认工作目录（当前二进制目录）
if(UNIX)
  add_executable(tpe_test_target tpe_test_target.c)
  set_target_properties(tpe_test_target PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
  )
endif()

# 手工测试工具 Test.exe 门控（规范 §11.10）：
# 仅当 TPE_BUILD_TESTS=ON 时构建（默认 OFF；目标定义见 tests/tools/CMakeLists.txt）
if(TPE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools")
  add_subdirectory(tools)
endif()

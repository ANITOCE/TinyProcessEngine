# CMake 3.10+ compatible FetchContent usage
# FetchContent_MakeAvailable requires 3.14+, use manual populate+add_subdirectory for compatibility

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)

FetchContent_GetProperties(googletest)
if(NOT googletest_POPULATED)
  FetchContent_Populate(googletest)
  # Prevent GTest from installing itself
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  # Force GTest to use the same CRT as the project
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  add_subdirectory(${googletest_SOURCE_DIR} ${googletest_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

enable_testing()

# Test executable
add_executable(tpe_tests
  test_value_type.cpp
  test_matches.cpp
  test_scanner.cpp
  test_session.cpp
  test_cli_parser.cpp
  test_value_parser.cpp
  test_repl_state.cpp
)

target_link_libraries(tpe_tests PRIVATE
  TinyProcessEngineLib
  PlatformLib
  gtest_main
)

target_include_directories(tpe_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/include/TinyProcessEngine
  ${CMAKE_SOURCE_DIR}/include/Platform
)

add_test(NAME tpe_tests COMMAND tpe_tests)

# 手工测试工具 Test.exe 门控（规范 §11.10）：
# 仅当 TPE_BUILD_TESTS=ON 时构建（默认 OFF；目标定义见 tests/tools/CMakeLists.txt）
if(TPE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools")
  add_subdirectory(tools)
endif()

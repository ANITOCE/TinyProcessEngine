# 手工测试工具 Test.exe（规范 §11.10；Phase 04 US3 / T028）
# - 完全独立：不链接 TinyProcessEngine / Platform 库，零第三方依赖
# - 仅当 TPE_BUILD_TESTS=ON 时由上级 tests/CMakeLists.txt 引入本目录
add_executable(Test Test.cpp)

# 产物与 quickstart 一致：build/tests/<config>/Test.exe
# （多配置生成器会在指定目录后自动追加配置名子目录）
set_target_properties(Test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

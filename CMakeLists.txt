cmake_minimum_required(VERSION 3.14)

# 定义项目名称和语言
project(TinyProcessEngine LANGUAGES CXX)

# 源码/执行字符集统一 UTF-8（Phase 06 FR-002；修复 C4819）
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 测试基础设施（缺陷⑥）：顶层注册 ctest，使任意构建目录可运行 ctest
enable_testing()

# 测试辅助进程 tpe_test_target 为 C 语言源文件（仅 Linux 构建需要）
if(UNIX)
    enable_language(C)
endif()

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 手工测试工具 Test.exe 构建开关（规范 §11.10；默认 OFF，初次构建后保持关闭）
option(TPE_BUILD_TESTS "Build manual test tool (Test.exe)" OFF)

# 添加头文件
include_directories(${CMAKE_SOURCE_DIR}/include)

# 查找并链接第三方库（示例）  
# find_package(Boost 1.74 REQUIRED COMPONENTS filesystem)
# target_link_libraries(${PROJECT_NAME} PRIVATE Boost::filesystem)

# 添加子目录
add_subdirectory(src)

# 添加测试子目录
add_subdirectory(tests)

# 安装目标和头文件
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
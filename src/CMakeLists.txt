# 添加子目录
add_subdirectory(Platform)
add_subdirectory(TinyProcessEngine)

# 定义源文件（建议后续明确列出各模块源文件）
# file(GLOB_RECURSE SOURCES "*.cpp")
# 定义目标
add_executable(${PROJECT_NAME} main.cpp)

# 设置输出位置
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build
)

# 将头文件声明为PUBLIC属性
target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_SOURCE_DIR}/include # 项目根目录的include文件夹
    ${CMAKE_SOURCE_DIR}/include/Platform
    ${CMAKE_SOURCE_DIR}/include/TinyProcessEngine
)

# 使用 target_compile_features 指定 C++17 支持
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

# 设置编译选项
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W3)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Werror)
endif()

# 链接 模块库
target_link_libraries(${PROJECT_NAME} PUBLIC TinyProcessEngineLib PlatformLib)

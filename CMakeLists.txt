cmake_minimum_required(VERSION 3.10)

# 定义项目名称和语言
project(TinyProcessEngine LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
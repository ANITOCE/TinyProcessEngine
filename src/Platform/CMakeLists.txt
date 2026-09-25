# 公共源文件
add_library(PlatformLib STATIC Platform.cpp)

# 平台选择（用 UNIX 谓词：LINUX 变量为 CMake 3.25+ 引入，本项目基线 3.10 下恒为假）
if(WIN32)
    add_subdirectory(Windows)

    # 添加 Windows平台相关源文件
    file(GLOB WIN_SOURCES "Windows/*.cpp")
    target_sources(PlatformLib PRIVATE ${WIN_SOURCES})
    
    target_include_directories(PlatformLib PUBLIC ${CMAKE_SOURCE_DIR}/include/Platform/Windows)
    
elseif(UNIX)
    add_subdirectory(Linux)

    # 添加 Linux平台相关源文件
    file(GLOB LINUX_SOURCES "Linux/*.cpp")
    target_sources(PlatformLib PRIVATE ${LINUX_SOURCES})

    target_include_directories(PlatformLib PUBLIC ${CMAKE_SOURCE_DIR}/include/Platform/Linux)

endif()

target_include_directories(PlatformLib PUBLIC
    ${CMAKE_SOURCE_DIR}/include # 项目根目录的include文件夹
    ${CMAKE_SOURCE_DIR}/include/Platform
)

# 设置平台相关宏
# if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
#     target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_WINDOWS)
# elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
#     target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_LINUX)
# endif()

# 模块相互依赖（Phase 1 修复：PlatformLib 为最底层，不依赖任何上层库）
# 原 target_link_libraries(PlatformLib PUBLIC TinyProcessEngineLib) 已移除
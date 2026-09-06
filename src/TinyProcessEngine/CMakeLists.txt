file(GLOB SOURCES CONFIGURE_DEPENDS "*.cpp")
add_library(TinyProcessEngineLib STATIC ${SOURCES})

# 设置该模块库的包含目录
target_include_directories(TinyProcessEngineLib PUBLIC
    ${CMAKE_SOURCE_DIR}/include # 项目根目录的include文件夹
    ${CMAKE_SOURCE_DIR}/include/Platform
    ${CMAKE_SOURCE_DIR}/include/TinyProcessEngine
)

target_link_libraries(TinyProcessEngineLib PUBLIC
    PlatformLib
)
# Phase 01 远程连接第三方依赖
#
# 由顶层 CMakeLists.txt 在 add_subdirectory(src) 之前 include（T002），
# 使后续 target_link_libraries 能解析 hv_static 与 spdlog::spdlog。
# 两个依赖在同一文件内完成 FetchContent_MakeAvailable：
# include 本文件后目标即已创建。
#
# 许可证：libhv BSD-3-Clause、spdlog MIT（内置 fmt 为 MIT）。

# ---------------------------------------------------------------------------
# 补丁脚本分支：本文件同时充当 libhv CMakeLists.txt 的补丁脚本（见 PATCH_COMMAND）
#
# libhv v1.3.4 在 Windows 上无条件执行
#   install(FILES $<TARGET_PDB_FILE:hv> DESTINATION bin OPTIONAL)
# 而 hv 目标只在 BUILD_SHARED=ON 时创建；本阶段要求 BUILD_SHARED=OFF，
# 因此 CMake 在 generate 阶段报 “No target "hv"” 并失败。
# 上游后续版本已把该 install 包进 BUILD_SHARED 判断（即下面的最小修复），
# 此处施加同一修复；对非 Windows 平台为行为中性。
#
# 仅 WIN32 且非 MINGW 时声明该补丁：MINGW 与 POSIX 下 libhv 本就不执行该行。
# ---------------------------------------------------------------------------
if(CMAKE_SCRIPT_MODE_FILE)
  if(NOT DEFINED TPE_LIBHV_CMAKE OR NOT EXISTS "${TPE_LIBHV_CMAKE}")
    message(FATAL_ERROR "libhv 补丁脚本缺少目标文件: '${TPE_LIBHV_CMAKE}'")
  endif()
  file(READ "${TPE_LIBHV_CMAKE}" _tpe_libhv_cmake)
  if(_tpe_libhv_cmake MATCHES "if\\(WIN32 AND NOT MINGW AND BUILD_SHARED\\)")
    message(STATUS "libhv: PDB 安装规则已有 BUILD_SHARED 判断，跳过补丁")
    return()
  endif()
  # 匹配片段不含换行，CRLF/LF 检出均可命中；v1.3.4 中该片段唯一
  string(REPLACE
    "if(WIN32 AND NOT MINGW)"
    "if(WIN32 AND NOT MINGW AND BUILD_SHARED)"
    _tpe_libhv_patched "${_tpe_libhv_cmake}"
  )
  if(_tpe_libhv_patched STREQUAL _tpe_libhv_cmake)
    message(FATAL_ERROR "libhv: 未找到预期片段，PDB 安装补丁未应用: ${TPE_LIBHV_CMAKE}")
  endif()
  file(WRITE "${TPE_LIBHV_CMAKE}" "${_tpe_libhv_patched}")
  message(STATUS "libhv: 已为静态构建修复 PDB 安装规则")
  return()
endif()

include(FetchContent)

set(TPE_LIBHV_PATCH_ARGS "")
if(WIN32 AND NOT MINGW)
  set(TPE_LIBHV_PATCH_ARGS
    PATCH_COMMAND
      "${CMAKE_COMMAND}"
      "-DTPE_LIBHV_CMAKE=<SOURCE_DIR>/CMakeLists.txt"
      -P "${CMAKE_CURRENT_LIST_FILE}"
  )
endif()

# ---- libhv v1.3.4：evpp TCP，静态库最小化配置 ----
# 仅需事件循环 + TCP：不构建共享库/示例，不启用 HTTP/MQTT/Redis/TLS。
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_EVPP ON CACHE BOOL "" FORCE)
set(WITH_HTTP OFF CACHE BOOL "" FORCE)
set(WITH_HTTP_SERVER OFF CACHE BOOL "" FORCE)
set(WITH_HTTP_CLIENT OFF CACHE BOOL "" FORCE)
set(WITH_MQTT OFF CACHE BOOL "" FORCE)
set(WITH_REDIS OFF CACHE BOOL "" FORCE)
set(WITH_OPENSSL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  libhv
  GIT_REPOSITORY https://github.com/ithewei/libhv.git
  GIT_TAG        v1.3.4
  ${TPE_LIBHV_PATCH_ARGS}
)

# ---- spdlog v1.15.3：操作日志，静态库 ----
# 不构建测试、不安装；第三方头文件按 SYSTEM 引入以抑制其告警（R4-4）。
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_SYSTEM_INCLUDES ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.15.3
)

# MakeAvailable 名称与 Declare 名称一致：libhv -> hv_static，spdlog -> spdlog::spdlog
FetchContent_MakeAvailable(libhv spdlog)

# ---- libhv 告警抑制 ----
# Linux/GCC 的 hmain.c 会产生 -Wformat-truncation；只在 C 编译器确实支持
# 对应正向告警时，对 hv_static 的 C 编译局部抑制。使用正向探测，因为 GCC
# 会静默接受不存在的 -Wno-* 选项，负向探测可能假通过。
# PRIVATE + COMPILE_LANGUAGE:C 保证选项不扩散到项目目标或 C++ 编译。
if(UNIX)
  include(CheckCCompilerFlag)
  check_c_compiler_flag("-Wformat-truncation" TPE_C_COMPILER_HAS_FORMAT_TRUNCATION)
  if(TPE_C_COMPILER_HAS_FORMAT_TRUNCATION)
    target_compile_options(hv_static PRIVATE
      $<$<COMPILE_LANGUAGE:C>:-Wno-format-truncation>)
  endif()
endif()

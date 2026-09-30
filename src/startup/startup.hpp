#pragma once

#include <string_view>

namespace tpe::app {

/// 版本号(规范 §11.2 --version;来源与策略见 research.md R4)。
inline constexpr std::string_view kVersion = "0.1.0";

/// 应用入口:解析终端参数并分发到 CLI 装配点;返回值即进程退出码。
int run(int argc, char** argv);

} // namespace tpe::app

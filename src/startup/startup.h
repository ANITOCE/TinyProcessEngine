#pragma once

#include <string_view>

namespace tpe::app {

/// 版本号(规范 §11.2 --version;来源与策略见 research.md R4)。
inline constexpr std::string_view kVersion = "0.1.0";

/// 应用入口:分发到 CLI / GUI 装配点。
/// 本阶段(T001)仅保留签名与空实现,业务行为在 US1(T015/T016)落地。
int run(int argc, char** argv);

} // namespace tpe::app

#pragma once

namespace tpe::app {

/// CLI 装配点:终端一次性命令与 REPL 主循环。
/// 本阶段(T001)仅保留签名与空实现,业务行为在 US1/US2 落地。
int runCli(int argc, char** argv);

} // namespace tpe::app

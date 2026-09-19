#pragma once

#include <string>
#include <vector>

namespace tpe::app {

/// CLI 装配点:执行终端一次性命令(--all-processes / --search-process / --open-process)。
/// args 不含程序名;返回终端退出码(tpe::cli::kExitOk / kExitRuntimeError / kExitUsageError)。
int runCli(const std::vector<std::string>& args);

} // namespace tpe::app

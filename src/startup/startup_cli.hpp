#pragma once

#include <string>
#include <vector>

namespace tpe {
class ProcessEngine;
}

namespace tpe::app {

/// CLI 装配点:执行终端一次性命令(--all-processes / --search-process / --open-process)。
/// args 不含程序名;返回终端退出码(tpe::cli::kExitOk / kExitRuntimeError / kExitUsageError)。
int runCli(const std::vector<std::string>& args);

/// 注入接缝(缺陷⑧;R9 测试可测性):以调用方提供的引擎执行同一套终端命令。
/// runCli(args) = 默认引擎 + runCliWithEngine(args, engine);对外行为不变。
int runCliWithEngine(const std::vector<std::string>& args, ProcessEngine& engine);

} // namespace tpe::app

#include "startup_cli.h"

#include "CliParser.h"
#include "Platform.h"
#include "ProcessEngine.h"

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tpe::app {

namespace {

/// 最小 REPL 骨架(US1):状态提示符 + exit / EOF / 未知命令。
/// 完整命令集、ReplState 与 REPL 帮助在 US2 按契约 C-R1–C-R7 落地。
int runReplSkeleton(const std::string& processName)
{
    const std::string prompt = processName + "-equal-i32> ";
    std::cout << prompt << std::flush;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "exit") {
            return tpe::cli::kExitOk;
        }
        if (!line.empty()) {
            // 临时行为:US2 按契约 C-R6 改为“未知命令 → 显示 REPL 帮助”。
            std::cout << "Unknown command." << std::endl;
        }
        std::cout << prompt << std::flush;
    }
    return tpe::cli::kExitOk; // stdin EOF(实现默认):视为正常退出
}

/// 提示符展示用进程名:优先取进程列表中的真实名称。
/// (平台层 open() 以占位名构造进程对象,故不以它作为唯一来源。)
std::string promptProcessName(ProcessEngine& engine, Pid_t pid,
                              const std::shared_ptr<PlatformProcess>& process)
{
    const std::optional<std::string> listed = engine.searchProcess(pid);
    if (listed.has_value() && !listed->empty()) {
        return *listed;
    }
    return process ? process->getProcessName() : std::string();
}

} // namespace

int runCli(const std::vector<std::string>& args)
{
    const tpe::cli::TerminalCommand command = tpe::cli::parseTerminalCommand(args);

    ProcessEngine engine;

    switch (command.kind) {
    case tpe::cli::TerminalCommandKind::AllProcesses:
        engine.getProcessList(); // 契约 C-T1:沿用 “PID: N ProcessName: X” 输出
        return tpe::cli::kExitOk;

    case tpe::cli::TerminalCommandKind::SearchProcess: {
        const std::optional<std::string> name = engine.searchProcess(command.pid);
        if (!name.has_value()) {
            std::cerr << "Process with PID " << command.pid << " not found." << std::endl;
            return tpe::cli::kExitRuntimeError;
        }
        if (name->empty()) {
            // 不得以空行充当成功(契约 C-T2)
            std::cerr << "Failed to read the process name for PID " << command.pid << "."
                      << std::endl;
            return tpe::cli::kExitRuntimeError;
        }
        std::cout << *name << std::endl; // 契约 C-T2:单行进程名
        return tpe::cli::kExitOk;
    }

    case tpe::cli::TerminalCommandKind::OpenProcess: {
        const std::shared_ptr<PlatformProcess> process = engine.openProcess(command.pid);
        if (!process) {
            // ProcessEngine::openProcess 已向 stderr 输出失败原因(契约 C-T3);不进入 REPL
            return tpe::cli::kExitRuntimeError;
        }
        return runReplSkeleton(promptProcessName(engine, command.pid, process));
    }

    default:
        // run() 仅转发三类执行命令到此处;其余分类(含 UsageError)在此不可达。
        return tpe::cli::kExitUsageError;
    }
}

} // namespace tpe::app

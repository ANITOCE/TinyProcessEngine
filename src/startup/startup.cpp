#include "startup.h"

#include "CliParser.hpp"
#include "startup_cli.h"

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

namespace tpe::app {

namespace {

/// 把 argv 转成不含程序名的参数序列。
std::vector<std::string> toArgs(int argc, char** argv)
{
    std::vector<std::string> args;
    if (argc > 1) {
        args.reserve(static_cast<std::size_t>(argc - 1));
    }
    for (int index = 1; index < argc; ++index) {
        args.emplace_back(argv[index] != nullptr ? argv[index] : "");
    }
    return args;
}

} // namespace

int run(int argc, char** argv)
{
    const std::vector<std::string> args = toArgs(argc, argv);
    const tpe::cli::TerminalCommand command = tpe::cli::parseTerminalCommand(args);

    switch (command.kind) {
    case tpe::cli::TerminalCommandKind::Help:
        std::cout << tpe::cli::terminalHelpText();
        return tpe::cli::parseExitCode(command.kind);

    case tpe::cli::TerminalCommandKind::Version:
        std::cout << "TinyProcessEngine " << kVersion << std::endl;
        return tpe::cli::parseExitCode(command.kind);

    case tpe::cli::TerminalCommandKind::UsageError:
        std::cerr << command.error << std::endl;   // 一行原因(stderr,契约 C-T6)
        std::cout << tpe::cli::terminalHelpText(); // 与 --help 相同的帮助(stdout)
        return tpe::cli::parseExitCode(command.kind);

    case tpe::cli::TerminalCommandKind::AllProcesses:
    case tpe::cli::TerminalCommandKind::SearchProcess:
    case tpe::cli::TerminalCommandKind::OpenProcess:
        return runCli(args);
    }

    return tpe::cli::kExitUsageError; // 防御:枚举扩展时保持确定的失败语义
}

} // namespace tpe::app

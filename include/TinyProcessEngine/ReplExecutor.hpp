#pragma once

#include <string>

namespace tpe {
class ProcessEngine;
}

namespace tpe::cli {

struct ReplCommand;
struct ReplState;

/// 一条扫描层命令的执行结果。
///
/// `output` 是应显示给用户的文本（可为空；多行以 LF 分隔）；`error` 表示该命令
/// 属于用法错误或运行期失败，远程层据此设置 `CommandError`。成功命令的文本保持
/// 与本地 REPL 相同，调用方只负责决定输出到 stdout 还是 RESPONSE。
struct ReplExecutionResult {
    std::string output;
    bool error = false;
};

/// 本地与远程共用的扫描命令执行器（纯逻辑，不直接写标准流）。
///
/// 该执行器只处理 `new-scan`、`next-scan`、`list`、`write`、`undo`；`help`、
/// `exit` 与空输入由调用方的层状态机处理。状态保存在调用方提供的 `ReplState`，
/// 扫描结果保存在 `ProcessEngine` 的 `ScanSession`，因此同一执行器可在 REPL
/// 循环中跨命令复用。
class ReplExecutor {
public:
    explicit ReplExecutor(tpe::ProcessEngine& engine);

    /// 执行一条已解析的扫描层命令。
    ReplExecutionResult execute(const ReplCommand& command, ReplState& state);

    tpe::ProcessEngine& engine() { return m_engine; }

private:
    tpe::ProcessEngine& m_engine;
};

} // namespace tpe::cli

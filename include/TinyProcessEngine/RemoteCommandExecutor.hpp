#pragma once

#include <string>
#include <string_view>

#include "RemoteProtocol.hpp"

namespace tpe {

class ProcessEngine;

} // namespace tpe

namespace tpe::remote {

// ============================================================
// TPE> 命令分类与执行（contracts/cli.md §3/§6；data-model.md §4）
// ============================================================

/// TPE> 命令类别；仅覆盖 Phase 01 交付的 4 条命令。
enum class RemoteCommandKind {
    AllProcesses,  // --all-processes：显示 Server 进程列表
    SearchProcess, // --search-process <PID>：显示 Server 上该 PID 的进程名
    Help,          // help / --help：显示 4 条已交付命令及一句话说明
    Exit,          // exit：结束 Client 会话，Server 继续监听
    Unknown,       // 其他输入：按未知命令处理（contracts/cli.md §3）
};

/// 一条远程命令的执行结果；Server 据此构造 RESPONSE（contracts/protocol.md §5）。
struct RemoteCommandResult {
    ResponseStatus status = ResponseStatus::Continue; // Continue / CommandError
    std::string output; // 可为空；多行以 LF（'\n'）分隔，不含提示符
};

// ============================================================
// 分类与执行接口（骨架：行为由 T017 实现）
// ============================================================

/// 对已通过发送前校验的单行命令分类；无法识别时返回 Unknown。
RemoteCommandKind classifyRemoteCommand(std::string_view line);

/// 执行一条单行命令并整理为可发送文本。
/// 失败时返回 status = CommandError，output 为契约 §6 规定的失败文本。
RemoteCommandResult executeRemoteCommand(std::string_view line, tpe::ProcessEngine& engine);

} // namespace tpe::remote

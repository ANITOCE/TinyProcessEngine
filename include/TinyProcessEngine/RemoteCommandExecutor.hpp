#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "RemoteLog.hpp"
#include "RemoteProtocol.hpp"
#include "ReplExecutor.hpp"
#include "ReplState.hpp"

namespace tpe {

class ProcessEngine;

} // namespace tpe

namespace tpe::remote {

/// 远程执行器当前层（data-model.md §1）。
enum class RemoteLayer {
    Connection,
    Scan,
};

// ============================================================
// TPE> 命令分类与执行（contracts/cli.md §3/§6；data-model.md §4）
// ============================================================

/// TPE> 命令类别；Phase 01 的 4 条命令加 Phase 02 的 `--open-process`。
enum class RemoteCommandKind {
    AllProcesses,  // --all-processes：显示 Server 进程列表
    SearchProcess, // --search-process <PID>：显示 Server 上该 PID 的进程名
    OpenProcess,   // --open-process <PID>：打开目标并进入扫描层
    Help,          // help / --help：显示连接层命令
    Exit,          // exit：结束 Client 会话，Server 继续监听
    Unknown,       // 其他输入：按未知命令处理（contracts/cli.md §3）
};

/// 一条远程命令的执行结果；Server 据此构造 RESPONSE（contracts/protocol.md §5）。
struct RemoteCommandResult {
    ResponseStatus status = ResponseStatus::Continue; // Continue / CommandError
    std::string output; // 可为空；多行以 LF（'\n'）分隔，不含提示符
    std::string prompt = "TPE>"; // 线上提示符，不含尾随空格

    // Phase 02 日志元数据（data-model.md §3/§5）
    bool loggable = false;
    std::string logCommand;
    std::optional<std::uint32_t> logPid;
    LogResult logResult = LogResult::Ok;
};

// ============================================================
// 分类与执行接口
// ============================================================

/// 对已通过发送前校验的单行命令分类；无法识别时返回 Unknown。
RemoteCommandKind classifyRemoteCommand(std::string_view line);

/// Phase 01 兼容入口：无状态执行一条 TPE> 命令。
RemoteCommandResult executeRemoteCommand(std::string_view line, tpe::ProcessEngine& engine);

/// 有状态远程命令执行器：每个会话一个实例，持有当前层、扫描状态与目标。
///
/// 所有方法只在会话独占工作线程调用；Server 事件循环不得直接调用引擎。
class RemoteCommandExecutor {
public:
    explicit RemoteCommandExecutor(tpe::ProcessEngine& engine);
    ~RemoteCommandExecutor();

    RemoteCommandExecutor(const RemoteCommandExecutor&) = delete;
    RemoteCommandExecutor& operator=(const RemoteCommandExecutor&) = delete;

    /// 执行一条已通过发送前校验的命令，返回文本、提示符与日志元数据。
    RemoteCommandResult execute(std::string_view line);

    /// 释放目标与扫描状态；回到 Connection/TPE>。可重复调用。
    void release();

    RemoteLayer layer() const { return m_layer; }
    std::string prompt() const;
    std::optional<std::uint32_t> targetPid() const { return m_pid; }

private:
    RemoteCommandResult executeConnectionCommand(std::string_view line);
    RemoteCommandResult executeScanCommand(std::string_view line);
    RemoteCommandResult executeOpenProcess(std::string_view line);
    void resetScanState();

    tpe::ProcessEngine& m_engine;
    tpe::cli::ReplExecutor m_executor;
    RemoteLayer m_layer = RemoteLayer::Connection;
    tpe::cli::ReplState m_state;
    std::optional<std::uint32_t> m_pid;
    std::string m_processName;
};

} // namespace tpe::remote

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tpe::remote {

// ============================================================
// 日志记录
// ============================================================

/// 单条 Server 会话日志的 result 字段（仅这 5 个取值）。
enum class LogResult {
    Ok,           // ok：接纳、正常结束、查询成功
    Busy,         // busy：第二 Client 或清理中拒绝
    NotFound,     // not-found：查询未命中
    Error,        // error：查询失败（含超限/不可编码）、接纳前握手失败、非法帧
    Disconnected, // disconnected：未收到 BYE 的意外断线
};

/// result 字段的文本形式（`ok` / `busy` / `not-found` / `error` / `disconnected`）。
std::string_view toLogResultText(LogResult result);

/// 会话级事件（事件名同样出现在 command 字段）。
enum class SessionEvent {
    Accept,     // 接纳 Client → command=accept
    Reject,     // 拒绝（busy/error）→ command=reject
    Disconnect, // 会话结束 → command=disconnect
};

/// 事件在 command 字段中的文本（`accept` / `reject` / `disconnect`）。
std::string_view sessionEventCommand(SessionEvent event);

/// 一条会话日志记录。
///
/// 行格式（，不含换行）：
/// `<utcTime> peer=<ip:port> command=<name> pid=<n|-> result=<summary>`
///
/// 不变量：
/// - `utcTime` 为 UTC 的 `YYYY-MM-DDTHH:MM:SSZ`；
/// - `peer` 为 `ip:port`，未知时渲染为 `-`；
/// - `pid` 仅在 `--search-process` 携带有效 PID 时存在，其余渲染为 `-`；
/// - 不包含内存数据、扫描值、完整命令行参数或进程名。
struct LogRecord {
    std::string utcTime;              // `YYYY-MM-DDTHH:MM:SSZ`（UTC）
    std::string peer;                 // `ip:port`；未知时为空
    std::string command;              // 命令名或事件名（见 SessionEvent）
    std::optional<std::uint32_t> pid; // 目标 PID；无则为 std::nullopt
    LogResult result = LogResult::Ok; // 结果摘要
};

// ============================================================
// 格式化接口（骨架：行为由后续任务实现）
// ============================================================

/// UTC ISO 8601 时间字符串 `YYYY-MM-DDTHH:MM:SSZ`。
std::string formatUtcTimestamp(std::chrono::system_clock::time_point tp);

/// 按的行格式渲染一条记录（不含换行）。
std::string formatLogLine(const LogRecord& record);

} // namespace tpe::remote

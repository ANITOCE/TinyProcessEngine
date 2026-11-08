#include "RemoteLog.hpp"

#include <spdlog/details/os.h>
#include <spdlog/fmt/fmt.h>

#include <ctime>
#include <string_view>

namespace tpe::remote {

namespace {

/// 字段缺失时的稳定占位（契约 §2/§4：未知 peer 与无 PID 均渲染为 `-`）。
constexpr std::string_view kMissingField{"-"};

/// 有 PID 时渲染十进制数值，否则渲染 `-`。
std::string formatPid(const std::optional<std::uint32_t>& pid)
{
    return pid.has_value() ? fmt::format("{}", *pid) : std::string{kMissingField};
}

} // namespace

std::string_view toLogResultText(LogResult result)
{
    // 契约 §2：仅这 5 个取值；枚举越界时返回稳定占位而非 UB。
    switch (result) {
    case LogResult::Ok:
        return "ok";
    case LogResult::Busy:
        return "busy";
    case LogResult::NotFound:
        return "not-found";
    case LogResult::Error:
        return "error";
    case LogResult::Disconnected:
        return "disconnected";
    }
    return kMissingField;
}

std::string_view sessionEventCommand(SessionEvent event)
{
    // 契约 §3：事件名写入 command 字段；枚举越界时返回稳定占位而非 UB。
    switch (event) {
    case SessionEvent::Accept:
        return "accept";
    case SessionEvent::Reject:
        return "reject";
    case SessionEvent::Disconnect:
        return "disconnect";
    }
    return kMissingField;
}

std::string formatUtcTimestamp(std::chrono::system_clock::time_point tp)
{
    // UTC 分解：使用 spdlog 的跨平台 gmtime 封装（内部为 gmtime_s / gmtime_r），
    // 全程不触碰本地时区 API。
    const std::time_t utcSeconds = static_cast<std::time_t>(
        std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count());
    const std::tm utc = spdlog::details::os::gmtime(utcSeconds);

    // 固定 20 字符：YYYY-MM-DDTHH:MM:SSZ（research.md R6）。
    return fmt::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z",
                       utc.tm_year + 1900,
                       utc.tm_mon + 1,
                       utc.tm_mday,
                       utc.tm_hour,
                       utc.tm_min,
                       utc.tm_sec);
}

std::string formatLogLine(const LogRecord& record)
{
    // 契约 §1：<utcTime> peer=<ip:port|-> command=<name> pid=<n|-> result=<summary>
    // utcTime 原样输出（不解析、不重新格式化）；peer 为空 → `-`；无 PID → `-`；
    // 行内不追加换行与其他字段（§4）。
    const std::string_view peer =
        record.peer.empty() ? kMissingField : std::string_view{record.peer};

    return fmt::format("{} peer={} command={} pid={} result={}",
                       record.utcTime,
                       peer,
                       record.command,
                       formatPid(record.pid),
                       toLogResultText(record.result));
}

} // namespace tpe::remote

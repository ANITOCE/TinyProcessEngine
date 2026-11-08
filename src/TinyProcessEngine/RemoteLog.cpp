#include "RemoteLog.hpp"

namespace tpe::remote {

// 骨架实现：仅提供可编译的占位，时间与字段格式化行为由后续任务实现。

std::string_view toLogResultText(LogResult result)
{
    (void)result;
    return {};
}

std::string_view sessionEventCommand(SessionEvent event)
{
    (void)event;
    return {};
}

std::string formatUtcTimestamp(std::chrono::system_clock::time_point tp)
{
    (void)tp;
    return {};
}

std::string formatLogLine(const LogRecord& record)
{
    (void)record;
    return {};
}

} // namespace tpe::remote

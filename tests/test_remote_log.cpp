// T010：远程日志格式化契约测试（TDD RED，T015 实现后转绿）。
// 依据：specs/001-remote-connection-phase-01/contracts/logging.md §1–§4、
//       specs/001-remote-connection-phase-01/data-model.md §5、
//       include/TinyProcessEngine/RemoteLog.hpp。
// 约束：只用 GoogleTest；时间点由固定整数秒构造，不读取系统时钟，
//       不访问网络、进程或文件系统。

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "RemoteLog.hpp"

using tpe::remote::LogRecord;
using tpe::remote::LogResult;
using tpe::remote::SessionEvent;

namespace {

/// 用 Unix 纪元起的固定秒数构造 time_point（不读取真实时钟）。
std::chrono::system_clock::time_point atEpochSeconds(std::int64_t seconds)
{
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
}

/// 构造一条字段齐全的记录；时间字段以字符串原文保存（渲染层不重新格式化）。
LogRecord makeContractRecord()
{
    LogRecord record;
    record.utcTime = "2026-11-08T12:34:56Z";
    record.peer = "127.0.0.1:54321";
    record.command = "--all-processes";
    record.pid = std::uint32_t{4321};
    record.result = LogResult::Ok;
    return record;
}

} // namespace

// ---------------------------------------------------------------------------
// UTC 时间戳（contracts/logging.md §1；research.md R6）
// ---------------------------------------------------------------------------

TEST(RemoteLog, FormatUtcTimestampRendersUnixEpoch)
{
    EXPECT_EQ(tpe::remote::formatUtcTimestamp(atEpochSeconds(0)),
              "1970-01-01T00:00:00Z");
}

TEST(RemoteLog, FormatUtcTimestampRendersOrdinaryDate)
{
    // 2026-11-08T12:34:56Z 对应 1794141296 Unix 秒。
    EXPECT_EQ(tpe::remote::formatUtcTimestamp(atEpochSeconds(1794141296)),
              "2026-11-08T12:34:56Z");
}

TEST(RemoteLog, FormatUtcTimestampCrossesDayBoundary)
{
    // 相差 1 秒的跨日边界：23:59:59 → 00:00:00。
    EXPECT_EQ(tpe::remote::formatUtcTimestamp(atEpochSeconds(1794182399)),
              "2026-11-08T23:59:59Z");
    EXPECT_EQ(tpe::remote::formatUtcTimestamp(atEpochSeconds(1794182400)),
              "2026-11-09T00:00:00Z");
}

TEST(RemoteLog, FormatUtcTimestampCrossesYearBoundary)
{
    // 相差 1 秒的跨年边界：2026-12-31T23:59:59Z → 2027-01-01T00:00:00Z。
    EXPECT_EQ(tpe::remote::formatUtcTimestamp(atEpochSeconds(1798761599)),
              "2026-12-31T23:59:59Z");
    EXPECT_EQ(tpe::remote::formatUtcTimestamp(atEpochSeconds(1798761600)),
              "2027-01-01T00:00:00Z");
}

TEST(RemoteLog, FormatUtcTimestampHandlesLeapDay)
{
    // 2024-02-29T00:00:00Z 对应 1709164800 Unix 秒。
    EXPECT_EQ(tpe::remote::formatUtcTimestamp(atEpochSeconds(1709164800)),
              "2024-02-29T00:00:00Z");
}

TEST(RemoteLog, FormatUtcTimestampEndsWithZAndHasNoLocalOffset)
{
    const std::string text = tpe::remote::formatUtcTimestamp(atEpochSeconds(1794141296));

    ASSERT_EQ(text.size(), std::size_t{20}) << "UTC 时间字符串固定 20 个字符：" << text;
    EXPECT_EQ(text[10], 'T') << text;
    EXPECT_EQ(text.back(), 'Z') << text; // 以 Z 结尾表示 UTC，不含本地时区偏移
    EXPECT_EQ(text.find('+'), std::string::npos) << text;
    EXPECT_EQ(text.find('-', 11), std::string::npos) << text; // 时间部分不含负偏移
}

// ---------------------------------------------------------------------------
// result 文本（contracts/logging.md §2：仅 5 个取值）
// ---------------------------------------------------------------------------

TEST(RemoteLog, ToLogResultTextMapsOk)
{
    EXPECT_EQ(std::string{tpe::remote::toLogResultText(LogResult::Ok)}, "ok");
}

TEST(RemoteLog, ToLogResultTextMapsBusy)
{
    EXPECT_EQ(std::string{tpe::remote::toLogResultText(LogResult::Busy)}, "busy");
}

TEST(RemoteLog, ToLogResultTextMapsNotFound)
{
    EXPECT_EQ(std::string{tpe::remote::toLogResultText(LogResult::NotFound)}, "not-found");
}

TEST(RemoteLog, ToLogResultTextMapsError)
{
    EXPECT_EQ(std::string{tpe::remote::toLogResultText(LogResult::Error)}, "error");
}

TEST(RemoteLog, ToLogResultTextMapsDisconnected)
{
    EXPECT_EQ(std::string{tpe::remote::toLogResultText(LogResult::Disconnected)}, "disconnected");
}

// ---------------------------------------------------------------------------
// 事件命令文本（contracts/logging.md §3：事件名写入 command 字段）
// ---------------------------------------------------------------------------

TEST(RemoteLog, SessionEventCommandMapsAccept)
{
    EXPECT_EQ(std::string{tpe::remote::sessionEventCommand(SessionEvent::Accept)}, "accept");
}

TEST(RemoteLog, SessionEventCommandMapsReject)
{
    EXPECT_EQ(std::string{tpe::remote::sessionEventCommand(SessionEvent::Reject)}, "reject");
}

TEST(RemoteLog, SessionEventCommandMapsDisconnect)
{
    EXPECT_EQ(std::string{tpe::remote::sessionEventCommand(SessionEvent::Disconnect)}, "disconnect");
}

// ---------------------------------------------------------------------------
// 日志行渲染（contracts/logging.md §1 行格式、§4 不变量）
// ---------------------------------------------------------------------------

TEST(RemoteLog, FormatLogLineRendersFullRecord)
{
    EXPECT_EQ(tpe::remote::formatLogLine(makeContractRecord()),
              "2026-11-08T12:34:56Z peer=127.0.0.1:54321 command=--all-processes pid=4321 result=ok");
}

TEST(RemoteLog, FormatLogLineRendersTimeFieldVerbatim)
{
    LogRecord record;
    record.utcTime = "sentinel-time"; // 非标准值：渲染层不得重新格式化或校验时间字段
    record.peer = "10.0.0.2:5000";
    record.command = "reject";
    record.pid.reset();
    record.result = LogResult::Busy;

    EXPECT_EQ(tpe::remote::formatLogLine(record),
              "sentinel-time peer=10.0.0.2:5000 command=reject pid=- result=busy");
}

TEST(RemoteLog, FormatLogLineRendersDashForMissingPeerAndPid)
{
    LogRecord record;
    record.utcTime = "2026-11-08T09:21:33Z";
    record.peer.clear();       // 未知 peer → "-"
    record.command = "accept";
    record.pid = std::nullopt; // 无 PID → "-"
    record.result = LogResult::Ok;

    EXPECT_EQ(tpe::remote::formatLogLine(record),
              "2026-11-08T09:21:33Z peer=- command=accept pid=- result=ok");
}

TEST(RemoteLog, FormatLogLineRendersDisconnectEventWithoutNewline)
{
    LogRecord record;
    record.utcTime = "2026-11-08T09:21:52Z";
    record.peer = "192.168.1.21:51235";
    record.command = "disconnect";
    record.pid = std::nullopt;
    record.result = LogResult::Disconnected;

    const std::string line = tpe::remote::formatLogLine(record);

    EXPECT_EQ(line,
              "2026-11-08T09:21:52Z peer=192.168.1.21:51235 command=disconnect pid=- result=disconnected");
    EXPECT_EQ(line.find('\n'), std::string::npos);
    EXPECT_EQ(line.find('\r'), std::string::npos);
}

TEST(RemoteLog, FormatLogLineIncludesOnlyRecordFields)
{
    LogRecord record;
    record.utcTime = "2026-11-08T09:21:44Z";
    record.peer = "192.168.1.20:51234";
    record.command = "--search-process";
    record.pid = std::uint32_t{4321};
    record.result = LogResult::Ok;

    const std::string line = tpe::remote::formatLogLine(record);

    // 契约 §4：不拼接内存数据、扫描值、完整命令行参数或进程名。
    // 渲染器只接收 LogRecord，精确行相等即证明输出仅含五个字段。
    EXPECT_EQ(line,
              "2026-11-08T09:21:44Z peer=192.168.1.20:51234 command=--search-process pid=4321 result=ok");
    EXPECT_EQ(line.find('\n'), std::string::npos);
    EXPECT_EQ(line.find('\r'), std::string::npos);
}

// T010：远程日志格式化契约测试（TDD RED，T015 实现后转绿）。
// T039：Server 操作日志事件分类测试（TDD RED，T041 实现后转绿）。
// 相关接口见 include/TinyProcessEngine/RemoteLog.hpp。
// 约束：只用 GoogleTest；时间点由固定整数秒构造，不读取系统时钟。
//       T010 段为纯格式化断言（不访问网络/进程/文件系统）；
//       T039 段使用进程内 RemoteServer + 127.0.0.1 回环原始 socket，
//       仅捕获 Server 标准输出，不启动子进程、不访问外部网络。
//       T043 段的查询错误日志用例注入 PlatformOS（固定快照/枚举失败），
//       同样只捕获 Server 标准输出，不读取宿主真实进程表。

// 平台网络头必须最先包含：Windows 上 winsock2.h 必须先于任何 windows.h
// （项目 Platform 头会间接包含 windows.h，若先包含会与 winsock.h 冲突重定义）。
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "ProcessEngine.hpp"
#include "RemoteClient.hpp"
#include "RemoteLog.hpp"
#include "RemoteProtocol.hpp"
#include "RemoteServer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

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
// UTC 时间戳
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
// result 文本（仅 5 个取值）
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
// 事件命令文本（事件名写入 command 字段）
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
// 日志行渲染
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

    // 不拼接内存数据、扫描值、完整命令行参数或进程名。
    // 渲染器只接收 LogRecord，精确行相等即证明输出仅含五个字段。
    EXPECT_EQ(line,
              "2026-11-08T09:21:44Z peer=192.168.1.20:51234 command=--search-process pid=4321 result=ok");
    EXPECT_EQ(line.find('\n'), std::string::npos);
    EXPECT_EQ(line.find('\r'), std::string::npos);
}

// ===========================================================================
// T039：Server 操作日志事件分类
//
// 本段使用真实回环 TCP（127.0.0.1）与进程内 RemoteServer：
//   - Server 日志写入 std::cout，由 ScopedLogStreams 以线程安全文本槽捕获；
//   - 原始 socket 客户端构造握手/命令/BYE/断线场景，不经过 RemoteClient，
//     避免 Client 输出混入 Server 日志断言；
//   - 全部等待为 5 s 上限的有界轮询；
//   - accept/reject 日志已在 US1 接入（基线用例），查询日志与部分 disconnect
//     日志属于 T041，相关断言当前预期 RED。
// ===========================================================================

namespace {

using namespace std::chrono_literals;

using tpe::remote::ErrorCode;
using tpe::remote::Frame;
using tpe::remote::MessageType;
using tpe::remote::ResponseStatus;

/// 单次等待上限为 5 秒。
constexpr std::chrono::milliseconds kWaitLimit{5000};

/// 事件分类用例各自使用独立端口（18110–18118），与 test_remote_session 的
/// 18080–18106 互不重叠；前一用例结束后同端口可能残留 TIME_WAIT。
constexpr std::uint16_t kLogAcceptPort = 18110;
constexpr std::uint16_t kLogRejectBusyPort = 18111;
constexpr std::uint16_t kLogRejectErrorPort = 18112;
constexpr std::uint16_t kLogDisconnectOkPort = 18113;
constexpr std::uint16_t kLogDisconnectDisconnectedPort = 18114;
constexpr std::uint16_t kLogDisconnectErrorPort = 18115;
constexpr std::uint16_t kLogSearchFailurePort = 18116;
constexpr std::uint16_t kLogHelpUnknownPort = 18117;
constexpr std::uint16_t kLogPreSendPort = 18118;
// T043（评审 I1）：查询错误日志用例端口（18119–18125）。
constexpr std::uint16_t kLogAllProcessesEnumerationErrorPort = 18119;
constexpr std::uint16_t kLogAllProcessesOversizePort = 18120;
constexpr std::uint16_t kLogAllProcessesUnencodablePort = 18121;
constexpr std::uint16_t kLogSearchParameterErrorPort = 18122;
constexpr std::uint16_t kLogSearchUnreadablePort = 18123;
constexpr std::uint16_t kLogSearchOversizePort = 18124;
constexpr std::uint16_t kLogSearchUnencodablePort = 18125;

/// 有界轮询：predicate 就绪返回 true；超时返回最后一次判定结果。
bool waitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (predicate()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return predicate();
        }
        std::this_thread::sleep_for(5ms);
    }
}

// ---------------------------------------------------------------------------
// 线程安全 stdout/stderr 捕获（Server 事件线程与测试线程并发写入）
// ---------------------------------------------------------------------------

/// 线程安全文本槽：捕获区由 Server 事件线程与测试线程并发写入，读取时加锁取快照。
class TextSink {
public:
    void append(const char* data, std::size_t size)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_text.append(data, size);
    }

    std::string text() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_text;
    }

    void clear()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_text.clear();
    }

private:
    mutable std::mutex m_mutex;
    std::string m_text;
};

/// 进程级单例（故意不回收）：若某用例超时后线程被 detach，缓冲区仍保持有效。
TextSink& logStdoutSink()
{
    static TextSink* sink = new TextSink();
    return *sink;
}

TextSink& logStderrSink()
{
    static TextSink* sink = new TextSink();
    return *sink;
}

/// 把 C++ 流写入落到 TextSink。
class SinkStreambuf : public std::streambuf {
public:
    explicit SinkStreambuf(TextSink& sink)
        : m_sink(sink)
    {
    }

protected:
    int_type overflow(int_type character) override
    {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        const char value = traits_type::to_char_type(character);
        m_sink.append(&value, 1);
        return character;
    }

    std::streamsize xsputn(const char* data, std::streamsize size) override
    {
        if (size > 0) {
            m_sink.append(data, static_cast<std::size_t>(size));
        }
        return size;
    }

private:
    TextSink& m_sink;
};

SinkStreambuf& logStdoutBuffer()
{
    static SinkStreambuf* buffer = new SinkStreambuf(logStdoutSink());
    return *buffer;
}

SinkStreambuf& logStderrBuffer()
{
    static SinkStreambuf* buffer = new SinkStreambuf(logStderrSink());
    return *buffer;
}

/// RAII 捕获 std::cout/std::cerr：构造时清空捕获区，析构时恢复原缓冲。
class ScopedLogStreams {
public:
    ScopedLogStreams()
    {
        logStdoutSink().clear();
        logStderrSink().clear();
        std::cout.flush();
        std::cerr.flush();
        m_oldOut = std::cout.rdbuf(&logStdoutBuffer());
        m_oldErr = std::cerr.rdbuf(&logStderrBuffer());
        std::cout.clear();
        std::cerr.clear();
    }

    ~ScopedLogStreams()
    {
        std::cout.flush();
        std::cerr.flush();
        std::cout.rdbuf(m_oldOut);
        std::cerr.rdbuf(m_oldErr);
        std::cout.clear();
        std::cerr.clear();
    }

    ScopedLogStreams(const ScopedLogStreams&) = delete;
    ScopedLogStreams& operator=(const ScopedLogStreams&) = delete;

private:
    std::streambuf* m_oldOut = nullptr;
    std::streambuf* m_oldErr = nullptr;
};

std::string capturedLogOut()
{
    return logStdoutSink().text();
}

/// 把 std::cin 接到固定文本（进程级泄漏的 istringstream，detach 兜底时仍有效）。
class ScopedInputText {
public:
    explicit ScopedInputText(const std::string& text)
    {
        static std::istringstream* stream = new std::istringstream();
        stream->clear();
        stream->str(text);
        m_oldIn = std::cin.rdbuf(stream->rdbuf());
        std::cin.clear();
    }

    ~ScopedInputText()
    {
        std::cin.rdbuf(m_oldIn);
        std::cin.clear();
    }

    ScopedInputText(const ScopedInputText&) = delete;
    ScopedInputText& operator=(const ScopedInputText&) = delete;

private:
    std::streambuf* m_oldIn = nullptr;
};

// ---------------------------------------------------------------------------
// 端口可用性探测（仅在 Server::start() 失败后调用）
// ---------------------------------------------------------------------------

enum class PortState { Free, Busy, Unknown };

#ifdef _WIN32
class WinsockGuard {
public:
    WinsockGuard()
    {
        WSADATA data{};
        m_ok = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }

    ~WinsockGuard()
    {
        if (m_ok) {
            ::WSACleanup();
        }
    }

    bool ok() const { return m_ok; }

private:
    bool m_ok = false;
};

bool winsockReady()
{
    static const WinsockGuard* guard = new WinsockGuard();
    return guard->ok();
}
#endif

/// 在指定 IPv4 地址上 bind 一个临时 TCP socket（不设置 SO_REUSEADDR）：
/// bind 成功 = 可用；EADDRINUSE（Windows 另含 WSAEACCES）= 不可用。
PortState probePortOn(std::uint32_t addressHostOrder, std::uint16_t port)
{
#ifdef _WIN32
    if (!winsockReady()) {
        return PortState::Unknown;
    }
    const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) {
        return PortState::Unknown;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    address.sin_addr.s_addr = htonl(addressHostOrder);
    const int result =
        ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const int error = (result == 0) ? 0 : ::WSAGetLastError();
    ::closesocket(handle);
    if (result == 0) {
        return PortState::Free;
    }
    return (error == WSAEADDRINUSE || error == WSAEACCES) ? PortState::Busy : PortState::Unknown;
#else
    const int handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle < 0) {
        return PortState::Unknown;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(addressHostOrder);
    const int result =
        ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const int error = errno;
    ::close(handle);
    if (result == 0) {
        return PortState::Free;
    }
    return (error == EADDRINUSE) ? PortState::Busy : PortState::Unknown;
#endif
}

bool isPortUnavailable(std::uint16_t port)
{
    return probePortOn(INADDR_ANY, port) == PortState::Busy ||
           probePortOn(INADDR_LOOPBACK, port) == PortState::Busy;
}

// ---------------------------------------------------------------------------
// 回环原始 socket 工具
// ---------------------------------------------------------------------------

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

bool ensureSocketsReady()
{
#ifdef _WIN32
    return winsockReady();
#else
    return true;
#endif
}

void closeSocketHandle(SocketHandle handle)
{
    if (handle == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

int lastSocketError()
{
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

bool isRecvTimeoutError(int error)
{
#ifdef _WIN32
    return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
#endif
}

bool isConnectionResetError(int error)
{
#ifdef _WIN32
    return error == WSAECONNRESET;
#else
    return error == ECONNRESET;
#endif
}

bool setSocketRecvTimeout(SocketHandle handle, std::chrono::milliseconds timeout)
{
    if (timeout < 1ms) {
        timeout = 1ms;
    }
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(timeout.count());
    return ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
#else
    timeval value{};
    value.tv_sec = static_cast<long>(timeout.count() / 1000);
    value.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    return ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0;
#endif
}

/// send 标志：Linux 上禁止 SIGPIPE（对端已关闭时返回错误而不是终止测试进程）。
int sendFlags()
{
#ifdef _WIN32
    return 0;
#else
    return MSG_NOSIGNAL;
#endif
}

bool sendAllBytes(SocketHandle handle, const void* data, std::size_t size)
{
    const char* bytes = static_cast<const char*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const int sent = static_cast<int>(
            ::send(handle, bytes + offset, static_cast<int>(size - offset), sendFlags()));
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

std::vector<std::uint8_t> toBytes(const std::string& text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

bool sendFrameBytes(SocketHandle handle, MessageType type,
                    const std::vector<std::uint8_t>& payload)
{
    const std::vector<std::uint8_t> bytes = tpe::remote::encodeFrame(type, payload);
    return sendAllBytes(handle, bytes.data(), bytes.size());
}

/// 有界读取一个完整帧：超过总期限、对端关闭或解码失败时返回 false 并写入 failure。
bool receiveFrameBefore(SocketHandle handle, std::chrono::milliseconds timeout, Frame& frame,
                        std::string& failure)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<std::uint8_t> buffer;
    std::uint8_t chunk[512];
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= 0ms) {
            failure = "timed out waiting for a complete frame (" +
                      std::to_string(buffer.size()) + " byte(s) received)";
            return false;
        }
        setSocketRecvTimeout(handle, (remaining < 200ms) ? remaining : 200ms);
        const int received =
            static_cast<int>(::recv(handle, reinterpret_cast<char*>(chunk), sizeof(chunk), 0));
        if (received > 0) {
            buffer.insert(buffer.end(), chunk, chunk + received);
            const tpe::remote::DecodeResult decoded = tpe::remote::tryDecodeFrame(buffer);
            if (decoded.status == tpe::remote::DecodeStatus::Complete) {
                frame = decoded.frame;
                return true;
            }
            if (decoded.status == tpe::remote::DecodeStatus::Failed) {
                failure = "frame decode failed: " + decoded.error.reason;
                return false;
            }
            continue;
        }
        if (received == 0) {
            failure = "peer closed before a complete frame arrived (" +
                      std::to_string(buffer.size()) + " byte(s) received)";
            return false;
        }
        const int error = lastSocketError();
        if (isConnectionResetError(error)) {
            failure = "peer reset the connection before a complete frame arrived";
            return false;
        }
        if (!isRecvTimeoutError(error)) {
            failure = "recv failed with socket error " + std::to_string(error);
            return false;
        }
    }
}

/// RAII 原始 Client socket（仅 127.0.0.1 回环）；析构时关闭。
class RawSocket {
public:
    RawSocket() = default;
    ~RawSocket() { close(); }

    RawSocket(const RawSocket&) = delete;
    RawSocket& operator=(const RawSocket&) = delete;

    bool connectTo(std::uint16_t port, std::string& failure)
    {
        close();
        if (!ensureSocketsReady()) {
            failure = "socket subsystem unavailable";
            return false;
        }
        m_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_handle == kInvalidSocket) {
            failure = "socket() failed";
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(m_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
            0) {
            failure = "connect() to 127.0.0.1:" + std::to_string(port) + " failed";
            close();
            return false;
        }
        return true;
    }

    bool sendFrame(MessageType type, const std::vector<std::uint8_t>& payload)
    {
        return m_handle != kInvalidSocket && sendFrameBytes(m_handle, type, payload);
    }

    bool receiveFrame(std::chrono::milliseconds timeout, Frame& frame, std::string& failure)
    {
        return m_handle != kInvalidSocket &&
               receiveFrameBefore(m_handle, timeout, frame, failure);
    }

    void close()
    {
        closeSocketHandle(m_handle);
        m_handle = kInvalidSocket;
    }

private:
    SocketHandle m_handle = kInvalidSocket;
};

// ---------------------------------------------------------------------------
// 进程内 Server 与日志文本解析
// ---------------------------------------------------------------------------

/// Server + 引擎的组合；析构时保证 stop()。
class LocalLogServer {
public:
    explicit LocalLogServer(std::uint16_t port)
        : m_server(port, m_engine)
    {
    }

    /// T043：注入平台实现（固定进程快照 / 可控枚举失败），
    /// 查询错误日志用例因此不读取宿主真实进程表。
    LocalLogServer(std::uint16_t port, std::unique_ptr<tpe::platform::PlatformOS> os)
        : m_engine(std::move(os))
        , m_server(port, m_engine)
    {
    }

    ~LocalLogServer() { m_server.stop(); }

    LocalLogServer(const LocalLogServer&) = delete;
    LocalLogServer& operator=(const LocalLogServer&) = delete;

    bool start() { return m_server.start(); }
    void stop() { m_server.stop(); }
    tpe::remote::RemoteServer& instance() { return m_server; }

private:
    tpe::ProcessEngine m_engine;
    tpe::remote::RemoteServer m_server;
};

enum class ServerStart { Ready, PortUnavailable, Failed };

ServerStart startLogServer(LocalLogServer& server, std::uint16_t port)
{
    if (server.start()) {
        return ServerStart::Ready;
    }
    return isPortUnavailable(port) ? ServerStart::PortUnavailable : ServerStart::Failed;
}

// ---------------------------------------------------------------------------
// T043：固定进程快照平台（查询错误日志用例）
//
// 与 test_remote_session 的 SnapshotProcess/ConfigurablePlatformOS 同一模式：
// 进程表在构造时固定，不读取宿主真实进程列表，避免“宿主恰好存在 PID 999999”
// 之类的外部依赖；额外支持可控枚举失败、空名称与非法 UTF-8 名称。
// ---------------------------------------------------------------------------

/// 快照进程：只承载 PID 与名称；查询路径不扫描内存。
class LogSnapshotProcess : public tpe::platform::PlatformProcess {
public:
    LogSnapshotProcess(tpe::platform::Pid_t pid, std::string name)
        : PlatformProcess(pid, std::move(name))
    {
    }

    std::vector<tpe::MemoryPage> getCheatablePages() const override { return {}; }

    tpe::Result<tpe::Memory, tpe::PlatformError> read(tpe::MemoryPage) const override
    {
        return tpe::Result<tpe::Memory, tpe::PlatformError>::error(notSupported());
    }

    tpe::Result<void, tpe::PlatformError> write(tpe::Address, const tpe::Memory&) override
    {
        return tpe::Result<void, tpe::PlatformError>::error(notSupported());
    }

private:
    tpe::PlatformError notSupported() const
    {
        return tpe::PlatformError{"LogSnapshotProcess",
                                  static_cast<unsigned long>(getPid()), 0, "not supported"};
    }
};

/// 可配置平台实现：固定进程列表 + 可选枚举失败（枚举失败与
/// `getAllProcessesPid()` 的错误返回保持一致）。
class LogSnapshotPlatformOS : public tpe::platform::PlatformOS {
public:
    void addProcess(tpe::platform::Pid_t pid, std::string name)
    {
        ProcessList.push_back(std::make_shared<LogSnapshotProcess>(pid, std::move(name)));
    }

    void setEnumerationFailure(std::string message)
    {
        m_enumerationError =
            tpe::PlatformError{"LogSnapshotPlatformOS", 0, 0, std::move(message)};
    }

    std::shared_ptr<tpe::platform::PlatformProcess> open(tpe::platform::Pid_t pid) override
    {
        for (const std::shared_ptr<tpe::platform::PlatformProcess>& process : ProcessList) {
            if (process->getPid() == pid) {
                return process;
            }
        }
        return nullptr;
    }

    tpe::Result<std::vector<tpe::platform::Pid_t>, tpe::PlatformError> getAllProcessesPid()
        override
    {
        if (m_enumerationError.has_value()) {
            return tpe::Result<std::vector<tpe::platform::Pid_t>, tpe::PlatformError>::error(
                *m_enumerationError);
        }
        std::vector<tpe::platform::Pid_t> pids;
        pids.reserve(ProcessList.size());
        for (const std::shared_ptr<tpe::platform::PlatformProcess>& process : ProcessList) {
            pids.push_back(process->getPid());
        }
        return tpe::Result<std::vector<tpe::platform::Pid_t>, tpe::PlatformError>::success(
            std::move(pids));
    }

    void getAllProcesses(std::vector<tpe::platform::Pid_t>) override {}
};

/// 非法 UTF-8 字节（0xFF 永不出现于合法 UTF-8 序列）的进程名。
std::string invalidUtf8LogFixtureName()
{
    std::string name = "bad";
    name.push_back(static_cast<char>(0xFF));
    name += "-name.exe";
    return name;
}

/// 超限名称：长度达到单条 RESPONSE payload 上限（合法 ASCII，
/// `PID: ... ProcessName: <name>` 或名称本身因此必然超过 1 MiB）。
std::string oversizeLogFixtureName()
{
    return std::string(static_cast<std::size_t>(tpe::remote::kMaxPayloadBytes), 'x');
}

/// 从捕获文本中提取 Server 日志行（含 peer= 与 command= 的完整行）。
std::vector<std::string> extractServerLogLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        const std::string line =
            text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (line.find("peer=") != std::string::npos &&
            line.find("command=") != std::string::npos) {
            lines.push_back(line);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return lines;
}

bool logLineMatches(const std::string& line, const std::string& command,
                    const std::string& result)
{
    return line.find("command=" + command) != std::string::npos &&
           line.find("result=" + result) != std::string::npos;
}

std::size_t countServerLogLines(const std::string& text, const std::string& command,
                                const std::string& result)
{
    std::size_t count = 0;
    for (const std::string& line : extractServerLogLines(text)) {
        if (logLineMatches(line, command, result)) {
            ++count;
        }
    }
    return count;
}

std::optional<std::string> findServerLogLine(const std::string& text,
                                             const std::string& command,
                                             const std::string& result)
{
    for (const std::string& line : extractServerLogLines(text)) {
        if (logLineMatches(line, command, result)) {
            return line;
        }
    }
    return std::nullopt;
}

/// 查询日志行数（`--all-processes` 与 `--search-process` 两种命令）。
std::size_t countQueryLogLines(const std::string& text)
{
    std::size_t count = 0;
    for (const std::string& line : extractServerLogLines(text)) {
        if (line.find("command=--all-processes") != std::string::npos ||
            line.find("command=--search-process") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// T043：日志行严格断言
//
// 以“恰好 5 个单空格分隔字段”作为结构断言：任何附加原因文本、进程名或
// 完整命令行都会增加字段数或改变字段内容，从而被拒绝。
// ---------------------------------------------------------------------------

/// 按单个空格拆分日志行；连续/尾随空格会产生空字段，从而被字段数断言拒绝。
std::vector<std::string> splitLogFields(const std::string& line)
{
    std::vector<std::string> fields;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = line.find(' ', begin);
        fields.push_back(line.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return fields;
}

/// 严格校验单条日志行：utcTime/peer/command/pid/result 五个字段，
/// 且 command/pid/result 逐字相等（无原因文本/进程名）。
::testing::AssertionResult logLineFieldsMatch(const std::string& line,
                                              const std::string& command,
                                              const std::string& pidField,
                                              const std::string& result)
{
    const std::vector<std::string> fields = splitLogFields(line);
    if (fields.size() != std::size_t{5}) {
        return ::testing::AssertionFailure()
               << "日志行必须恰好 5 个字段（无原因文本/进程名），实际 " << fields.size()
               << " 个：[" << line << "]";
    }
    if (fields[0].size() != std::size_t{20} || fields[0][10] != 'T' ||
        fields[0].back() != 'Z') {
        return ::testing::AssertionFailure()
               << "utcTime 必须是 YYYY-MM-DDTHH:MM:SSZ：[" << line << "]";
    }
    if (fields[1].rfind("peer=", 0) != 0 || fields[1].size() <= 5) {
        return ::testing::AssertionFailure() << "peer 字段无效：[" << line << "]";
    }
    if (fields[2] != "command=" + command) {
        return ::testing::AssertionFailure()
               << "command 期望 " << command << "：[" << line << "]";
    }
    if (fields[3] != "pid=" + pidField) {
        return ::testing::AssertionFailure()
               << "pid 期望 " << pidField << "：[" << line << "]";
    }
    if (fields[4] != "result=" + result) {
        return ::testing::AssertionFailure()
               << "result 期望 " << result << "：[" << line << "]";
    }
    return ::testing::AssertionSuccess();
}

/// 断言：捕获文本中查询日志恰好 expectedCount 条，且每一条字段与期望一致。
::testing::AssertionResult queryLogsMatch(const std::string& text,
                                          std::size_t expectedCount,
                                          const std::string& command,
                                          const std::string& pidField,
                                          const std::string& result)
{
    std::vector<std::string> queryLines;
    for (const std::string& line : extractServerLogLines(text)) {
        if (line.find("command=--all-processes") != std::string::npos ||
            line.find("command=--search-process") != std::string::npos) {
            queryLines.push_back(line);
        }
    }
    if (queryLines.size() != expectedCount) {
        return ::testing::AssertionFailure()
               << "期望 " << expectedCount << " 条查询日志，实际 " << queryLines.size()
               << " 条；server stdout=[" << text << "]";
    }
    for (const std::string& line : queryLines) {
        const ::testing::AssertionResult match =
            logLineFieldsMatch(line, command, pidField, result);
        if (!match) {
            return match;
        }
    }
    return ::testing::AssertionSuccess();
}

/// 完成 HELLO/WELCOME 握手；返回 false 时 failure 说明原因。
bool openRawSession(RawSocket& socket, std::uint16_t port, std::string& failure)
{
    if (!socket.connectTo(port, failure)) {
        return false;
    }
    if (!socket.sendFrame(MessageType::Hello, toBytes("TPE"))) {
        failure = "failed to send HELLO";
        return false;
    }
    Frame welcome;
    if (!socket.receiveFrame(kWaitLimit, welcome, failure)) {
        return false;
    }
    if (welcome.header.type != MessageType::Welcome) {
        failure = "expected WELCOME, got type " +
                  std::to_string(static_cast<int>(welcome.header.type));
        return false;
    }
    if (!welcome.payload.empty()) {
        failure = "WELCOME payload must be empty";
        return false;
    }
    return true;
}

/// 发送一条 COMMAND 并消费对应 RESPONSE。
bool exchangeCommand(RawSocket& socket, const std::string& line, Frame& response,
                     std::string& failure)
{
    if (!socket.sendFrame(MessageType::Command, toBytes(line))) {
        failure = "failed to send COMMAND: " + line;
        return false;
    }
    if (!socket.receiveFrame(kWaitLimit, response, failure)) {
        return false;
    }
    if (response.header.type != MessageType::Response) {
        failure = "expected RESPONSE for '" + line + "', got type " +
                  std::to_string(static_cast<int>(response.header.type));
        return false;
    }
    return true;
}

/// 发送 BYE 并消费 Server 的 BYE 确认。
bool closeWithBye(RawSocket& socket, std::string& failure)
{
    if (!socket.sendFrame(MessageType::Bye, {})) {
        failure = "failed to send BYE";
        return false;
    }
    Frame ack;
    if (!socket.receiveFrame(kWaitLimit, ack, failure)) {
        return false;
    }
    if (ack.header.type != MessageType::Bye) {
        failure = "expected BYE acknowledgement, got type " +
                  std::to_string(static_cast<int>(ack.header.type));
        return false;
    }
    return true;
}

/// T043：在已启动的 Server 上依次执行命令并正常结束会话；等待 disconnect/ok
/// 日志出现后返回（该日志最后写入，出现即证明查询日志均已捕获）。
/// 返回 false 时 failure 说明原因（调用方 ASSERT_TRUE）。
bool runLoggedQueries(std::uint16_t port, const std::vector<std::string>& commands,
                      std::vector<Frame>& responses, std::string& failure)
{
    RawSocket socket;
    if (!openRawSession(socket, port, failure)) {
        return false;
    }
    for (const std::string& command : commands) {
        Frame response;
        if (!exchangeCommand(socket, command, response, failure)) {
            return false;
        }
        responses.push_back(std::move(response));
    }
    if (!closeWithBye(socket, failure)) {
        return false;
    }
    if (!waitUntil(
            [] {
                return findServerLogLine(capturedLogOut(), "disconnect", "ok").has_value();
            },
            kWaitLimit)) {
        failure = "disconnect/ok log was not observed within the wait limit; server stdout=[" +
                  capturedLogOut() + "]";
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 事件分类：accept/ok、reject/busy、reject/error
// ---------------------------------------------------------------------------

/// 接纳 Client → 恰好 1 条 `accept/ok`，pid 为 `-`。
TEST(RemoteLogEvents, AcceptOkIsLoggedExactlyOnce)
{
    ScopedLogStreams streams;
    LocalLogServer server(kLogAcceptPort);
    const ServerStart started = startLogServer(server, kLogAcceptPort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogAcceptPort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::string failure;
    RawSocket socket;
    ASSERT_TRUE(openRawSession(socket, kLogAcceptPort, failure)) << failure;

    ASSERT_TRUE(waitUntil(
        [] { return findServerLogLine(capturedLogOut(), "accept", "ok").has_value(); },
        kWaitLimit))
        << "HELLO/WELCOME 后必须记录 accept/ok；server stdout=[" << capturedLogOut() << "]";

    EXPECT_EQ(countServerLogLines(capturedLogOut(), "accept", "ok"), std::size_t{1});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "reject", "error"), std::size_t{0});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "disconnect", "ok"), std::size_t{0});

    const std::optional<std::string> line =
        findServerLogLine(capturedLogOut(), "accept", "ok");
    ASSERT_TRUE(line.has_value());
    EXPECT_NE(line->find("peer="), std::string::npos) << *line;
    EXPECT_NE(line->find("pid=-"), std::string::npos) << *line;

    // 正常结束后 accept 仍只有 1 条（不因会话结束重复记录）。
    ASSERT_TRUE(closeWithBye(socket, failure)) << failure;
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "accept", "ok"), std::size_t{1});
}

/// 已占用时第二连接被拒 → 恰好 1 条 `reject/busy`。
TEST(RemoteLogEvents, RejectBusyIsLoggedExactlyOnce)
{
    ScopedLogStreams streams;
    LocalLogServer server(kLogRejectBusyPort);
    const ServerStart started = startLogServer(server, kLogRejectBusyPort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogRejectBusyPort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::string failure;
    RawSocket first;
    ASSERT_TRUE(openRawSession(first, kLogRejectBusyPort, failure)) << failure;

    RawSocket second;
    ASSERT_TRUE(second.connectTo(kLogRejectBusyPort, failure)) << failure;
    Frame busy;
    ASSERT_TRUE(second.receiveFrame(kWaitLimit, busy, failure)) << failure;
    EXPECT_EQ(static_cast<int>(busy.header.type), static_cast<int>(MessageType::Busy));
    EXPECT_EQ(std::string(busy.payload.begin(), busy.payload.end()), "Server is busy.");

    ASSERT_TRUE(waitUntil(
        [] {
            return findServerLogLine(capturedLogOut(), "reject", "busy").has_value();
        },
        kWaitLimit))
        << "第二连接被拒后必须记录 reject/busy；server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "reject", "busy"), std::size_t{1});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "reject", "error"), std::size_t{0});

    const std::optional<std::string> line =
        findServerLogLine(capturedLogOut(), "reject", "busy");
    ASSERT_TRUE(line.has_value());
    EXPECT_NE(line->find("pid=-"), std::string::npos) << *line;
}

/// 接纳前握手失败（此处为握手期收到非 HELLO 帧）→ 1 条 `reject/error`。
TEST(RemoteLogEvents, RejectErrorIsLoggedExactlyOnce)
{
    ScopedLogStreams streams;
    LocalLogServer server(kLogRejectErrorPort);
    const ServerStart started = startLogServer(server, kLogRejectErrorPort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogRejectErrorPort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::string failure;
    RawSocket socket;
    ASSERT_TRUE(socket.connectTo(kLogRejectErrorPort, failure)) << failure;
    // 握手期发送 WELCOME（S→C 类型）属于非法消息类型 → ERROR(2) 并关闭。
    ASSERT_TRUE(socket.sendFrame(MessageType::Welcome, {})) << "failed to send bad frame";
    Frame error;
    ASSERT_TRUE(socket.receiveFrame(kWaitLimit, error, failure)) << failure;
    ASSERT_EQ(static_cast<int>(error.header.type), static_cast<int>(MessageType::Error));
    ASSERT_FALSE(error.payload.empty());
    EXPECT_EQ(error.payload.front(), static_cast<std::uint8_t>(ErrorCode::BadMessageType));

    ASSERT_TRUE(waitUntil(
        [] {
            return findServerLogLine(capturedLogOut(), "reject", "error").has_value();
        },
        kWaitLimit))
        << "握手失败必须记录 reject/error；server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "reject", "error"), std::size_t{1});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "accept", "ok"), std::size_t{0});

    const std::optional<std::string> line =
        findServerLogLine(capturedLogOut(), "reject", "error");
    ASSERT_TRUE(line.has_value());
    EXPECT_NE(line->find("pid=-"), std::string::npos) << *line;
}

// ---------------------------------------------------------------------------
// 事件分类：disconnect/ok、disconnect/disconnected、disconnect/error
// ---------------------------------------------------------------------------

/// 正常结束（BYE）→ 恰好 1 条 `disconnect/ok`。
TEST(RemoteLogEvents, DisconnectOkOnByeIsLoggedExactlyOnce)
{
    ScopedLogStreams streams;
    LocalLogServer server(kLogDisconnectOkPort);
    const ServerStart started = startLogServer(server, kLogDisconnectOkPort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogDisconnectOkPort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::string failure;
    RawSocket socket;
    ASSERT_TRUE(openRawSession(socket, kLogDisconnectOkPort, failure)) << failure;
    ASSERT_TRUE(closeWithBye(socket, failure)) << failure;

    ASSERT_TRUE(waitUntil(
        [] {
            return findServerLogLine(capturedLogOut(), "disconnect", "ok").has_value();
        },
        kWaitLimit))
        << "BYE 正常结束必须记录 disconnect/ok；server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "disconnect", "ok"), std::size_t{1});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "disconnect", "disconnected"),
              std::size_t{0});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "disconnect", "error"), std::size_t{0});
}

/// 未收到 BYE 的意外断线 → 恰好 1 条 `disconnect/disconnected`。
TEST(RemoteLogEvents, DisconnectDisconnectedOnPeerCloseIsLoggedExactlyOnce)
{
    ScopedLogStreams streams;
    LocalLogServer server(kLogDisconnectDisconnectedPort);
    const ServerStart started =
        startLogServer(server, kLogDisconnectDisconnectedPort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogDisconnectDisconnectedPort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::string failure;
    {
        RawSocket socket;
        ASSERT_TRUE(openRawSession(socket, kLogDisconnectDisconnectedPort, failure))
            << failure;
        socket.close(); // 活动会话未发 BYE 即断开
    }

    ASSERT_TRUE(waitUntil(
        [] {
            return findServerLogLine(capturedLogOut(), "disconnect", "disconnected")
                .has_value();
        },
        kWaitLimit))
        << "未收到 BYE 的断线必须记录 disconnect/disconnected；server stdout=["
        << capturedLogOut() << "]";
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "disconnect", "disconnected"),
              std::size_t{1});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "disconnect", "ok"), std::size_t{0});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "disconnect", "error"), std::size_t{0});
}

/// 已接纳会话遇到非法载荷帧（非空 BYE）→ 只记 1 条 `disconnect/error`，
/// 不记 `disconnect/disconnected`，也不额外记 reject。
TEST(RemoteLogEvents, DisconnectErrorOnInvalidPayloadIsLoggedOnceWithoutDisconnected)
{
    ScopedLogStreams streams;
    LocalLogServer server(kLogDisconnectErrorPort);
    const ServerStart started = startLogServer(server, kLogDisconnectErrorPort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogDisconnectErrorPort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::string failure;
    RawSocket socket;
    ASSERT_TRUE(openRawSession(socket, kLogDisconnectErrorPort, failure)) << failure;

    ASSERT_TRUE(socket.sendFrame(MessageType::Bye, toBytes("x")))
        << "failed to send non-empty BYE";
    Frame error;
    ASSERT_TRUE(socket.receiveFrame(kWaitLimit, error, failure)) << failure;
    ASSERT_EQ(static_cast<int>(error.header.type), static_cast<int>(MessageType::Error));
    ASSERT_FALSE(error.payload.empty());
    EXPECT_EQ(error.payload.front(), static_cast<std::uint8_t>(ErrorCode::InvalidPayload));

    ASSERT_TRUE(waitUntil(
        [] {
            return findServerLogLine(capturedLogOut(), "disconnect", "error").has_value();
        },
        kWaitLimit))
        << "非法载荷关闭必须记录 disconnect/error；server stdout=[" << capturedLogOut()
        << "]";
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "disconnect", "error"), std::size_t{1});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "disconnect", "disconnected"),
              std::size_t{0});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "reject", "error"), std::size_t{0});
    EXPECT_EQ(extractServerLogLines(capturedLogOut()).size(), std::size_t{2})
        << "只允许 accept/ok 与 disconnect/error 两条日志；server stdout=["
        << capturedLogOut() << "]";
}

// ---------------------------------------------------------------------------
// 查询日志：失败保留 PID、help/未知命令不记录、无原因文本
// ---------------------------------------------------------------------------

/// `--search-process <不存在 PID>` → 精确 1 条 `pid=999999 result=not-found`
/// （T043 收紧原宽松断言）；日志行仅含 5 个字段，不回显响应原因文本
/// 进程表由注入快照固定，不依赖宿主真实 PID。
TEST(RemoteLogEvents, SearchProcessFailureKeepsPidAndOmitsReasonText)
{
    ScopedLogStreams streams;
    auto os = std::make_unique<LogSnapshotPlatformOS>();
    os->addProcess(4321, "alpha.exe"); // 快照中只有 4321：999999 必然不存在
    LocalLogServer server(kLogSearchFailurePort, std::move(os));
    const ServerStart started = startLogServer(server, kLogSearchFailurePort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogSearchFailurePort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::string failure;
    RawSocket socket;
    ASSERT_TRUE(openRawSession(socket, kLogSearchFailurePort, failure)) << failure;

    constexpr const char* kPidText = "999999";
    Frame response;
    ASSERT_TRUE(exchangeCommand(socket, std::string("--search-process ") + kPidText,
                                response, failure))
        << failure;

    const auto decoded = tpe::remote::decodeResponse(response.payload);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(static_cast<int>(decoded.value().status),
              static_cast<int>(ResponseStatus::CommandError))
        << "不存在的 PID 必须以 status=2 返回";
    EXPECT_NE(decoded.value().output.find("Process with PID 999999 not found."),
              std::string::npos)
        << "response output=[" << decoded.value().output << "]";

    ASSERT_TRUE(closeWithBye(socket, failure)) << failure;

    ASSERT_TRUE(waitUntil(
        [] {
            return findServerLogLine(capturedLogOut(), "--search-process", "not-found")
                .has_value();
        },
        kWaitLimit))
        << "--search-process 未命中必须记录 not-found；server stdout=[" << capturedLogOut()
        << "]";

    EXPECT_TRUE(queryLogsMatch(capturedLogOut(), std::size_t{1}, "--search-process",
                               kPidText, "not-found"))
        << "server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "--search-process", "error"),
              std::size_t{0})
        << "未命中不得记为 error；server stdout=[" << capturedLogOut() << "]";

    const std::optional<std::string> line =
        findServerLogLine(capturedLogOut(), "--search-process", "not-found");
    ASSERT_TRUE(line.has_value());
    EXPECT_EQ(line->find("not found"), std::string::npos) << *line;
    EXPECT_EQ(line->find("Process with PID"), std::string::npos) << *line;
}

/// `help`、`--help` 与未知命令不产生查询日志。
TEST(RemoteLogEvents, HelpAndUnknownCommandsProduceNoQueryLog)
{
    ScopedLogStreams streams;
    LocalLogServer server(kLogHelpUnknownPort);
    const ServerStart started = startLogServer(server, kLogHelpUnknownPort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogHelpUnknownPort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::string failure;
    RawSocket socket;
    ASSERT_TRUE(openRawSession(socket, kLogHelpUnknownPort, failure)) << failure;

    for (const char* command : {"help", "--help", "nonsense-command", "--bogus"}) {
        Frame response;
        ASSERT_TRUE(exchangeCommand(socket, command, response, failure)) << failure;
        const auto decoded = tpe::remote::decodeResponse(response.payload);
        ASSERT_TRUE(decoded.has_value());
        const bool isHelp = std::string(command) == "help" || std::string(command) == "--help";
        EXPECT_EQ(static_cast<int>(decoded.value().status),
                  static_cast<int>(isHelp ? ResponseStatus::Continue
                                          : ResponseStatus::CommandError))
            << command << " 的响应状态不符合契约（help=0，未知命令=2）";
    }

    ASSERT_TRUE(closeWithBye(socket, failure)) << failure;
    ASSERT_TRUE(waitUntil(
        [] {
            return findServerLogLine(capturedLogOut(), "disconnect", "ok").has_value();
        },
        kWaitLimit))
        << "server stdout=[" << capturedLogOut() << "]";

    EXPECT_EQ(countQueryLogLines(capturedLogOut()), std::size_t{0})
        << "help/未知命令不得产生查询日志；server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(extractServerLogLines(capturedLogOut()).size(), std::size_t{2})
        << "本会话只允许 accept/ok 与 disconnect/ok 两条日志；server stdout=["
        << capturedLogOut() << "]";
}

/// Client 发送前拒绝的命令（内嵌 CR）不得发送到 Server，也不产生查询日志。
TEST(RemoteLogEvents, PreSendRejectedCommandProducesNoQueryLog)
{
    ScopedLogStreams streams;
    ScopedInputText input("bad\rcommand\nexit\n");
    LocalLogServer server(kLogPreSendPort);
    const ServerStart started = startLogServer(server, kLogPreSendPort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogPreSendPort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    struct ClientOutcome {
        std::atomic<bool> done{false};
        std::atomic<int> exitCode{-1};
    };
    const auto outcome = std::make_shared<ClientOutcome>();
    std::thread worker([outcome] {
        tpe::remote::RemoteClient client("127.0.0.1", kLogPreSendPort);
        outcome->exitCode.store(client.run(), std::memory_order_relaxed);
        outcome->done.store(true, std::memory_order_release);
    });
    if (!waitUntil([&outcome] { return outcome->done.load(std::memory_order_acquire); },
                   kWaitLimit)) {
        worker.detach(); // 有界失败：绝不对可能永不返回的线程做无界 join
        server.stop();
        FAIL() << "RemoteClient::run() did not return within 5 s";
    }
    worker.join();

    EXPECT_EQ(outcome->exitCode.load(std::memory_order_relaxed), 0)
        << "本地拒绝后仍可 exit 正常结束；captured output=[" << capturedLogOut() << "]";
    EXPECT_NE(capturedLogOut().find("Command must not contain line breaks."),
              std::string::npos)
        << "captured output=[" << capturedLogOut() << "]";

    ASSERT_TRUE(waitUntil(
        [] {
            return findServerLogLine(capturedLogOut(), "disconnect", "ok").has_value();
        },
        kWaitLimit))
        << "server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(countQueryLogLines(capturedLogOut()), std::size_t{0})
        << "发送前拒绝的命令不得产生查询日志；server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "accept", "ok"), std::size_t{1});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "disconnect", "ok"), std::size_t{1});
}

// ===========================================================================
// T043（评审 I1）：查询错误日志覆盖
//
// 要求 --all-processes/--search-process 的所有失败路径
// 各产生恰好 1 条查询日志，且日志行不含原因文本/进程名/完整命令行。
// 本段用注入的固定进程快照覆盖：枚举失败、结果超限、非法 UTF-8 名称、PID 缺失
// 或非法、名称不可读、名称不可编码；不读取宿主真实进程表（避免 PID 999999
// 恰好存在的偶然依赖）。所有等待沿用 kWaitLimit（≤5 s）。
// ===========================================================================

namespace {

/// 超限回退的固定响应文本。
constexpr const char* kOversizeResponseLogText =
    "Response exceeds the maximum message size.";

/// `--all-processes` 列表含非法 UTF-8 名称时的固定响应文本。
constexpr const char* kUnencodableListLogText =
    "Process list contains a name that cannot be encoded as UTF-8.";

/// `--search-process` 名称含非法 UTF-8 时的固定响应文本。
constexpr const char* kUnencodableNameLogText =
    "Process name cannot be encoded as UTF-8.";

} // namespace

/// `--all-processes` 枚举失败 → 恰好 1 条 `command=--all-processes pid=- result=error`，
/// 日志不含平台原因文本。
TEST(RemoteLogEvents, AllProcessesEnumerationFailureIsLoggedAsError)
{
    ScopedLogStreams streams;
    auto os = std::make_unique<LogSnapshotPlatformOS>();
    os->setEnumerationFailure("enumeration unavailable in test");
    LocalLogServer server(kLogAllProcessesEnumerationErrorPort, std::move(os));
    const ServerStart started =
        startLogServer(server, kLogAllProcessesEnumerationErrorPort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogAllProcessesEnumerationErrorPort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::vector<Frame> responses;
    std::string failure;
    ASSERT_TRUE(runLoggedQueries(kLogAllProcessesEnumerationErrorPort, {"--all-processes"},
                                 responses, failure))
        << failure;
    ASSERT_EQ(responses.size(), std::size_t{1});

    const auto decoded = tpe::remote::decodeResponse(responses.front().payload);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(static_cast<int>(decoded.value().status),
              static_cast<int>(ResponseStatus::CommandError));
    EXPECT_NE(decoded.value().output.find("Failed to enumerate processes:"),
              std::string::npos)
        << "response output=[" << decoded.value().output << "]";

    EXPECT_TRUE(queryLogsMatch(capturedLogOut(), std::size_t{1}, "--all-processes", "-",
                               "error"))
        << "server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "accept", "ok"), std::size_t{1});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "disconnect", "ok"), std::size_t{1});
    EXPECT_EQ(extractServerLogLines(capturedLogOut()).size(), std::size_t{3})
        << "只允许 accept/ok、1 条查询 error 与 disconnect/ok；server stdout=["
        << capturedLogOut() << "]";
}

/// `--all-processes` 结果超限（>1 MiB）→ 同 1 条 `pid=- result=error`，
/// 响应为固定超限文本；日志不回显进程名。
TEST(RemoteLogEvents, AllProcessesOversizeIsLoggedAsError)
{
    ScopedLogStreams streams;
    auto os = std::make_unique<LogSnapshotPlatformOS>();
    os->addProcess(5000, oversizeLogFixtureName()); // 单条名称即超过 1 MiB
    LocalLogServer server(kLogAllProcessesOversizePort, std::move(os));
    const ServerStart started = startLogServer(server, kLogAllProcessesOversizePort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogAllProcessesOversizePort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::vector<Frame> responses;
    std::string failure;
    ASSERT_TRUE(runLoggedQueries(kLogAllProcessesOversizePort, {"--all-processes"},
                                 responses, failure))
        << failure;
    ASSERT_EQ(responses.size(), std::size_t{1});

    const auto decoded = tpe::remote::decodeResponse(responses.front().payload);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(static_cast<int>(decoded.value().status),
              static_cast<int>(ResponseStatus::CommandError));
    EXPECT_EQ(decoded.value().output, kOversizeResponseLogText)
        << "超限必须回退为固定 status=2 文本";

    EXPECT_TRUE(queryLogsMatch(capturedLogOut(), std::size_t{1}, "--all-processes", "-",
                               "error"))
        << "server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(extractServerLogLines(capturedLogOut()).size(), std::size_t{3})
        << "只允许 accept/ok、1 条查询 error 与 disconnect/ok；server stdout=["
        << capturedLogOut() << "]";
}

/// `--all-processes` 列表含非法 UTF-8 名称 → 同 1 条 `pid=- result=error`，
/// 日志不含该名称。
TEST(RemoteLogEvents, AllProcessesInvalidUtf8NameIsLoggedAsError)
{
    ScopedLogStreams streams;
    auto os = std::make_unique<LogSnapshotPlatformOS>();
    os->addProcess(5001, invalidUtf8LogFixtureName());
    LocalLogServer server(kLogAllProcessesUnencodablePort, std::move(os));
    const ServerStart started =
        startLogServer(server, kLogAllProcessesUnencodablePort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogAllProcessesUnencodablePort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::vector<Frame> responses;
    std::string failure;
    ASSERT_TRUE(runLoggedQueries(kLogAllProcessesUnencodablePort, {"--all-processes"},
                                 responses, failure))
        << failure;
    ASSERT_EQ(responses.size(), std::size_t{1});

    const auto decoded = tpe::remote::decodeResponse(responses.front().payload);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(static_cast<int>(decoded.value().status),
              static_cast<int>(ResponseStatus::CommandError));
    EXPECT_EQ(decoded.value().output, kUnencodableListLogText)
        << "response output=[" << decoded.value().output << "]";

    EXPECT_TRUE(queryLogsMatch(capturedLogOut(), std::size_t{1}, "--all-processes", "-",
                               "error"))
        << "server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(capturedLogOut().find("bad"), std::string::npos)
        << "日志不得回显进程名；server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(extractServerLogLines(capturedLogOut()).size(), std::size_t{3})
        << "只允许 accept/ok、1 条查询 error 与 disconnect/ok；server stdout=["
        << capturedLogOut() << "]";
}

/// `--search-process` 参数缺失或非法 → 每次查询各 1 条
/// `command=--search-process pid=- result=error`。
TEST(RemoteLogEvents, SearchProcessMissingAndInvalidPidAreLoggedWithDashPid)
{
    ScopedLogStreams streams;
    auto os = std::make_unique<LogSnapshotPlatformOS>();
    os->addProcess(4321, "alpha.exe"); // 参数错误不得触碰进程表
    LocalLogServer server(kLogSearchParameterErrorPort, std::move(os));
    const ServerStart started = startLogServer(server, kLogSearchParameterErrorPort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogSearchParameterErrorPort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::vector<Frame> responses;
    std::string failure;
    ASSERT_TRUE(runLoggedQueries(kLogSearchParameterErrorPort,
                                 {"--search-process", "--search-process abc"}, responses,
                                 failure))
        << failure;
    ASSERT_EQ(responses.size(), std::size_t{2});
    for (const Frame& response : responses) {
        const auto decoded = tpe::remote::decodeResponse(response.payload);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(static_cast<int>(decoded.value().status),
                  static_cast<int>(ResponseStatus::CommandError));
        EXPECT_NE(decoded.value().output.find("Usage: --search-process <PID>."),
                  std::string::npos)
            << "response output=[" << decoded.value().output << "]";
    }

    EXPECT_TRUE(queryLogsMatch(capturedLogOut(), std::size_t{2}, "--search-process", "-",
                               "error"))
        << "每次查询各 1 条日志且 PID 缺失/非法用 `-`；server stdout=["
        << capturedLogOut() << "]";
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "accept", "ok"), std::size_t{1});
    EXPECT_EQ(countServerLogLines(capturedLogOut(), "disconnect", "ok"), std::size_t{1});
    EXPECT_EQ(extractServerLogLines(capturedLogOut()).size(), std::size_t{4})
        << "只允许 accept/ok、2 条查询 error 与 disconnect/ok；server stdout=["
        << capturedLogOut() << "]";
}

/// `--search-process` 名称不可读（空名称）→ 恰好 1 条
/// `pid=4328 result=error`。
TEST(RemoteLogEvents, SearchProcessUnreadableNameIsLoggedAsError)
{
    constexpr const char* kPidText = "4328";
    ScopedLogStreams streams;
    auto os = std::make_unique<LogSnapshotPlatformOS>();
    os->addProcess(4328, std::string{}); // 空名称 = 名称不可读
    LocalLogServer server(kLogSearchUnreadablePort, std::move(os));
    const ServerStart started = startLogServer(server, kLogSearchUnreadablePort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogSearchUnreadablePort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::vector<Frame> responses;
    std::string failure;
    ASSERT_TRUE(runLoggedQueries(kLogSearchUnreadablePort,
                                 {std::string("--search-process ") + kPidText}, responses,
                                 failure))
        << failure;
    ASSERT_EQ(responses.size(), std::size_t{1});

    const auto decoded = tpe::remote::decodeResponse(responses.front().payload);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(static_cast<int>(decoded.value().status),
              static_cast<int>(ResponseStatus::CommandError));
    EXPECT_NE(decoded.value().output.find("Failed to read the process name for PID 4328."),
              std::string::npos)
        << "response output=[" << decoded.value().output << "]";

    EXPECT_TRUE(queryLogsMatch(capturedLogOut(), std::size_t{1}, "--search-process",
                               kPidText, "error"))
        << "server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(extractServerLogLines(capturedLogOut()).size(), std::size_t{3})
        << "只允许 accept/ok、1 条查询 error 与 disconnect/ok；server stdout=["
        << capturedLogOut() << "]";
}

/// `--search-process` 结果超限（>1 MiB）→ 恰好 1 条
/// `pid=4329 result=error`，响应为固定超限文本。
TEST(RemoteLogEvents, SearchProcessOversizeIsLoggedAsError)
{
    constexpr const char* kPidText = "4329";
    ScopedLogStreams streams;
    auto os = std::make_unique<LogSnapshotPlatformOS>();
    os->addProcess(4329, oversizeLogFixtureName());
    LocalLogServer server(kLogSearchOversizePort, std::move(os));
    const ServerStart started = startLogServer(server, kLogSearchOversizePort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogSearchOversizePort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::vector<Frame> responses;
    std::string failure;
    ASSERT_TRUE(runLoggedQueries(kLogSearchOversizePort,
                                 {std::string("--search-process ") + kPidText}, responses,
                                 failure))
        << failure;
    ASSERT_EQ(responses.size(), std::size_t{1});

    const auto decoded = tpe::remote::decodeResponse(responses.front().payload);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(static_cast<int>(decoded.value().status),
              static_cast<int>(ResponseStatus::CommandError));
    EXPECT_EQ(decoded.value().output, kOversizeResponseLogText)
        << "超限必须回退为固定 status=2 文本";

    EXPECT_TRUE(queryLogsMatch(capturedLogOut(), std::size_t{1}, "--search-process",
                               kPidText, "error"))
        << "server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(extractServerLogLines(capturedLogOut()).size(), std::size_t{3})
        << "只允许 accept/ok、1 条查询 error 与 disconnect/ok；server stdout=["
        << capturedLogOut() << "]";
}

/// `--search-process` 名称不可编码（非法 UTF-8）→ 恰好 1 条
/// `pid=4330 result=error`，日志不含名称。
TEST(RemoteLogEvents, SearchProcessUnencodableNameIsLoggedAsError)
{
    constexpr const char* kPidText = "4330";
    ScopedLogStreams streams;
    auto os = std::make_unique<LogSnapshotPlatformOS>();
    os->addProcess(4330, invalidUtf8LogFixtureName());
    LocalLogServer server(kLogSearchUnencodablePort, std::move(os));
    const ServerStart started = startLogServer(server, kLogSearchUnencodablePort);
    if (started == ServerStart::PortUnavailable) {
        GTEST_SKIP() << "TCP port " << kLogSearchUnencodablePort
                     << " is unavailable (bound by another process or reserved by the system).";
    }
    ASSERT_TRUE(started == ServerStart::Ready)
        << "RemoteServer::start() must succeed on a free port";

    std::vector<Frame> responses;
    std::string failure;
    ASSERT_TRUE(runLoggedQueries(kLogSearchUnencodablePort,
                                 {std::string("--search-process ") + kPidText}, responses,
                                 failure))
        << failure;
    ASSERT_EQ(responses.size(), std::size_t{1});

    const auto decoded = tpe::remote::decodeResponse(responses.front().payload);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(static_cast<int>(decoded.value().status),
              static_cast<int>(ResponseStatus::CommandError));
    EXPECT_EQ(decoded.value().output, kUnencodableNameLogText)
        << "response output=[" << decoded.value().output << "]";

    EXPECT_TRUE(queryLogsMatch(capturedLogOut(), std::size_t{1}, "--search-process",
                               kPidText, "error"))
        << "server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(capturedLogOut().find("bad"), std::string::npos)
        << "日志不得回显进程名；server stdout=[" << capturedLogOut() << "]";
    EXPECT_EQ(extractServerLogLines(capturedLogOut()).size(), std::size_t{3})
        << "只允许 accept/ok、1 条查询 error 与 disconnect/ok；server stdout=["
        << capturedLogOut() << "]";
}

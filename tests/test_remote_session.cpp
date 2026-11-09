/**
 * test_remote_session.cpp — 远程会话回环握手测试（T020/T021 RED；T022/T023 实现后转绿）
 *
 * 依据：
 *   - specs/001-remote-connection-phase-01/contracts/protocol.md §2/§3/§4
 *   - specs/001-remote-connection-phase-01/contracts/cli.md §2/§7
 *   - specs/001-remote-connection-phase-01/quickstart.md 场景 1/2/9/10/17
 *   - .superpowers/sdd/remote-connection-phase-01/t020-brief.md
 *   - .superpowers/sdd/remote-connection-phase-01/t021-brief.md
 *
 * 架构（brief “测试架构要求”）：
 *   - Server 与 Client 在同一进程的不同线程中通过真实回环 TCP（127.0.0.1）通信；
 *   - std::cin/std::cout/std::cerr 的替换由 ScopedRemoteStreams 以 RAII 恢复；
 *   - 输出按写入线程分流：Client 线程写入自己的捕获区，其余线程（Server 事件线程、
 *     主线程）写入 Server 捕获区，因此可分别断言两端的固定文本与提示符顺序；
 *   - Client 输入使用 BlockingStdinBuffer：underflow() 在条件变量上等待“释放”信号，
 *     使首会话停留在 TPE>；释放后返回 exit\n，随后返回 EOF；
 *   - 所有等待均为有界轮询（单次上限 5 秒）；超时先 stop()/释放输入再 FAIL()，
 *     绝不对可能永不返回的线程做无界 join（析构 detach 兜底）；
 *   - 端口策略：先调用 start()；仅当其返回 false 时才探测端口是否真的不可用
 *     （同时 bind 0.0.0.0 与 127.0.0.1；Windows 另把 WSAEACCES 视为不可用），
 *     确实不可用才在用例体内 GTEST_SKIP()，否则以真实错误 FAIL；每个自定义端口
 *     用例使用独立端口（18080–18086），避免前一用例的 TIME_WAIT 干扰后续用例；
 *   - T021 增加 ScriptedServer（bind 127.0.0.1:0 的脚本化原始 socket Server）与
 *     RawSocket（原始 socket Client），用于构造坏 Server / 坏 Client 握手场景；
 *     时限通过 RemoteClient::setTimeoutsForTesting 与
 *     RemoteServer::setHandshakeTimeoutForTesting 注入缩短值。
 *   - T026 增加固定进程快照注入（SnapshotPlatformOS；LocalServer 增加引擎重载
 *     构造函数）与 US2 成功查询用例：远程 `--all-processes`/`--search-process`
 *     的输出（统一 CRLF/LF 后）与同一引擎的本地结果逐字一致。
 *   - T027 增加 US2 失败查询与发送前校验用例：查询失败（PID 不存在、名称不可读、
 *     枚举失败、未知命令、结果超限、不可编码、不可编码优先于超限）均保留 `TPE>`
 *     并可用 `help` 继续接收命令；发送前校验（65,537 字节、非法 UTF-8、裸 `CR`）
 *     由脚本化 Server 断言被拒命令未以 COMMAND 发出，且不产生未知命令响应。
 *
 * 约束：仅 GoogleTest + 标准库 + 平台 socket API；不启动子进程；除 127.0.0.1
 * 回环外不访问网络。
 *
 * 当前阶段（US2）：T022/T023 的 Server/Client 握手已实现，T020/T021 的握手用例
 * 为 GREEN；T026/T027 的成功查询、失败查询与发送前校验用例在 T028–T030 实现前
 * 预期 RED（Client 尚未发送 COMMAND，查询结果与本地拒绝文本不会出现），但必须
 * 在 5 秒上限内以断言失败结束，不得挂起。失败查询的 Client 退出码留到 US3
 * （T038）实际退出时验证。
 */

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
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "Platform.hpp"
#include "ProcessEngine.hpp"
#include "RemoteClient.hpp"
#include "RemoteProtocol.hpp"
#include "RemoteServer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

/// 单次等待上限（brief：所有等待最多 5 秒）。
constexpr std::chrono::milliseconds kWaitLimit{5000};

/// 契约 §1：Server 省略端口时使用 8008。
constexpr std::uint16_t kDefaultPort = 8008;

/// Client 提示符（含其后的单个空格）：结果块分段断言依赖这一完整文本。
constexpr const char* kPromptText = "TPE> ";

/// 自定义端口用例各自使用独立端口：前一用例结束后同一端口上可能残留 TIME_WAIT，
/// 复用端口会让后一用例的 bind/start 受到干扰，因此每例一个端口。
constexpr std::uint16_t kLifecyclePort = 18080;
constexpr std::uint16_t kCustomPort = 18081;
constexpr std::uint16_t kBusyRejectionPort = 18082;
constexpr std::uint16_t kWelcomePort = 18083;

/// T021 真实 Server 握手失败用例的独立端口（18084–18086，与 T020 用例互不重叠）。
constexpr std::uint16_t kVersionRejectPort = 18084;
constexpr std::uint16_t kHandshakeTimeoutPort = 18085;
constexpr std::uint16_t kPeerClosePort = 18086;

/// T026 成功查询用例的独立端口（18087–18089，与既有用例互不重叠）。
constexpr std::uint16_t kQueryAllProcessesPort = 18087;
constexpr std::uint16_t kQuerySearchProcessPort = 18088;
constexpr std::uint16_t kQueryUtf8NamePort = 18089;

/// T027 失败查询用例的独立端口（18090–18097，与既有用例互不重叠）。
constexpr std::uint16_t kQueryFailurePidNotFoundPort = 18090;
constexpr std::uint16_t kQueryFailureUnreadableNamePort = 18091;
constexpr std::uint16_t kQueryFailureEnumerationPort = 18092;
constexpr std::uint16_t kQueryFailureUnknownCommandPort = 18093;
constexpr std::uint16_t kQueryFailureOversizePort = 18094;
constexpr std::uint16_t kQueryFailureUnencodablePort = 18095;
constexpr std::uint16_t kQueryFailureEnumerationUnencodablePort = 18096;
constexpr std::uint16_t kQueryFailureDoubleFailurePort = 18097;

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
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::size_t countOccurrences(const std::string& text, const std::string& needle)
{
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

// ---------------------------------------------------------------------------
// 线程安全文本捕获与按线程分流
// ---------------------------------------------------------------------------

/// 线程安全文本槽：捕获区由不同线程并发写入，读取时加锁取快照。
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

    bool contains(const std::string& needle) const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_text.find(needle) != std::string::npos;
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

/// 按“写入线程”分流 std::cout/std::cerr：登记过的 Client 线程写入自己的槽，
/// 其余线程（Server 事件线程、主线程）写入 Server 槽。
/// 这样同一进程内的两端固定文本可以分别断言（brief 第 2 条）。
class RoutingStreambuf : public std::streambuf {
public:
    explicit RoutingStreambuf(TextSink& serverSink)
        : m_serverSink(serverSink)
    {
    }

    void registerClient(std::thread::id id, TextSink& sink)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_clientSinks[id] = &sink;
    }

    void unregisterClient(std::thread::id id)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_clientSinks.erase(id);
    }

    /// 清理上一用例可能遗留的登记（仅用于防御性复位，正常路径由线程自行注销）。
    void unregisterAllClients()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_clientSinks.clear();
    }

protected:
    int_type overflow(int_type character) override
    {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        const char value = traits_type::to_char_type(character);
        write(&value, 1);
        return character;
    }

    std::streamsize xsputn(const char* data, std::streamsize size) override
    {
        if (size > 0) {
            write(data, static_cast<std::size_t>(size));
        }
        return size;
    }

private:
    void write(const char* data, std::size_t size)
    {
        TextSink* sink = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_clientSinks.find(std::this_thread::get_id());
            sink = (found != m_clientSinks.end()) ? found->second : &m_serverSink;
        }
        sink->append(data, size);
    }

    TextSink& m_serverSink;
    mutable std::mutex m_mutex;
    std::map<std::thread::id, TextSink*> m_clientSinks;
};

/// 进程级单例（故意不回收）：即使某用例超时后线程被 detach，缓冲区仍保持有效，
/// 避免测试收尾阶段的 use-after-free。ScopedRemoteStreams 负责在用例开始时清空。
TextSink& serverStdoutSink()
{
    static TextSink* sink = new TextSink();
    return *sink;
}

TextSink& serverStderrSink()
{
    static TextSink* sink = new TextSink();
    return *sink;
}

RoutingStreambuf& stdoutRouter()
{
    static RoutingStreambuf* router = new RoutingStreambuf(serverStdoutSink());
    return *router;
}

RoutingStreambuf& stderrRouter()
{
    static RoutingStreambuf* router = new RoutingStreambuf(serverStderrSink());
    return *router;
}

std::string capturedServerOut()
{
    return serverStdoutSink().text();
}

// ---------------------------------------------------------------------------
// 可释放的阻塞 stdin：让首个 Client 停留在 TPE> 等待输入
// ---------------------------------------------------------------------------

/// brief 第 5 条：underflow() 在收到释放信号前阻塞（条件变量）。
/// releaseWith() 追加文本（如 "exit\n"）并置 EOF 标记；文本消费完后返回 EOF。
/// wait_for 超时也返回 EOF，保证读取线程在最坏情况下仍能自行退出（不会永久挂起）。
class BlockingStdinBuffer : public std::streambuf {
public:
    /// 用例开始前复位（此时没有读取线程）。
    void reset()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_text.clear();
        m_position = 0;
        m_eof = false;
    }

    /// 释放阻塞：追加文本并通知；文本读完后返回 EOF。
    void releaseWith(std::string text)
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_text += text;
            m_eof = true;
        }
        m_condition.notify_all();
    }

    /// 立即以 EOF 解锁（超时清理路径）。
    void interrupt()
    {
        releaseWith(std::string{});
    }

protected:
    int_type underflow() override
    {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait_for(lock, kWaitLimit, [this] {
                return m_position < m_text.size() || m_eof;
            });
            if (m_position >= m_text.size()) {
                m_eof = true; // 释放后文本耗尽，或等待超时：统一按 EOF 结束
                return traits_type::eof();
            }
            m_character = m_text[m_position++];
        }

        setg(&m_character, &m_character, &m_character + 1);
        return traits_type::to_int_type(m_character);
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::string m_text;
    std::size_t m_position = 0;
    bool m_eof = false;
    char m_character = '\0';
};

BlockingStdinBuffer& stdinBuffer()
{
    static BlockingStdinBuffer* buffer = new BlockingStdinBuffer();
    return *buffer;
}

/// RAII 流重定向：构造时清空捕获区并换入测试缓冲，析构时刷新并恢复原缓冲。
class ScopedRemoteStreams {
public:
    ScopedRemoteStreams()
    {
        stdinBuffer().reset();
        serverStdoutSink().clear();
        serverStderrSink().clear();
        stdoutRouter().unregisterAllClients();
        stderrRouter().unregisterAllClients();

        std::cout.flush();
        std::cerr.flush();
        m_oldIn = std::cin.rdbuf(&stdinBuffer());
        m_oldOut = std::cout.rdbuf(&stdoutRouter());
        m_oldErr = std::cerr.rdbuf(&stderrRouter());
        std::cin.clear();
        std::cout.clear();
        std::cerr.clear();
    }

    ~ScopedRemoteStreams()
    {
        std::cout.flush();
        std::cerr.flush();
        std::cin.rdbuf(m_oldIn);
        std::cout.rdbuf(m_oldOut);
        std::cerr.rdbuf(m_oldErr);
        std::cin.clear();
        std::cout.clear();
        std::cerr.clear();
    }

    ScopedRemoteStreams(const ScopedRemoteStreams&) = delete;
    ScopedRemoteStreams& operator=(const ScopedRemoteStreams&) = delete;

private:
    std::streambuf* m_oldIn = nullptr;
    std::streambuf* m_oldOut = nullptr;
    std::streambuf* m_oldErr = nullptr;
};

// ---------------------------------------------------------------------------
// 端口不可用性诊断（仅在 start() 失败后调用）
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
    // 进程级、不回收：避免与测试退出阶段仍在收尾的线程竞争 WSACleanup。
    static const WinsockGuard* guard = new WinsockGuard();
    return guard->ok();
}
#endif

/// 在指定 IPv4 地址（主机字节序）上 bind 一个临时 TCP socket（不设置 SO_REUSEADDR）：
/// bind 成功 = 该地址可用；EADDRINUSE（Windows 另含 WSAEACCES）= 不可用；
/// 其它错误（或 Winsock 初始化失败）= 无法判定。
PortState probeTcpPortOn(std::uint32_t addressHostOrder, std::uint16_t port)
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
    const int result = ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const int error = (result == 0) ? 0 : ::WSAGetLastError();
    ::closesocket(handle);
    if (result == 0) {
        return PortState::Free;
    }
    // WSAEACCES：端口被系统保留（如 Hyper-V 动态端口排除区间），对绑定方同样不可用。
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
    const int result = ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const int error = errno;
    ::close(handle);
    if (result == 0) {
        return PortState::Free;
    }
    return (error == EADDRINUSE) ? PortState::Busy : PortState::Unknown;
#endif
}

/// 同时探测通配地址 0.0.0.0 与回环地址 127.0.0.1：
/// 任一地址不可用（占用/保留）即视为端口实际不可用；两者均可用才算空闲；
/// 其余情形（探测本身失败）为无法判定。
PortState probeTcpPort(std::uint16_t port)
{
    const PortState wildcard = probeTcpPortOn(INADDR_ANY, port);
    const PortState loopback = probeTcpPortOn(INADDR_LOOPBACK, port);
    if (wildcard == PortState::Busy || loopback == PortState::Busy) {
        return PortState::Busy;
    }
    if (wildcard == PortState::Free && loopback == PortState::Free) {
        return PortState::Free;
    }
    return PortState::Unknown;
}

/// brief 第 4 条：仅在 start() 失败后调用。返回 true = 端口确实不可用，调用方必须
/// 在用例体内 GTEST_SKIP()（GTEST_SKIP 只返回到当前函数，不能放在返回非 void 的
/// 辅助函数里）；返回 false = 端口空闲或无法判定，调用方应以真实错误 FAIL。
bool isPortUnavailable(std::uint16_t port)
{
    return probeTcpPort(port) == PortState::Busy;
}

// ---------------------------------------------------------------------------
// 原始 socket 工具与脚本化 Server（T021；Windows Winsock / Linux BSD socket）
// ---------------------------------------------------------------------------

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

/// 平台 socket 子系统就绪（Windows 惰性 WSAStartup；POSIX 恒 true）。
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

/// send 标志：Linux 上禁止 SIGPIPE（对端已关闭时返回错误而不是终止测试进程）。
int sendFlags()
{
#ifdef _WIN32
    return 0;
#else
    return MSG_NOSIGNAL;
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

/// 手工编码任意版本的帧（encodeFrame 固定 version=1，版本不兼容用例需构造 version=2）。
std::vector<std::uint8_t> encodeRawFrame(std::uint8_t version, tpe::remote::MessageType type,
                                         const std::vector<std::uint8_t>& payload)
{
    const std::uint32_t length = static_cast<std::uint32_t>(2 + payload.size());
    std::vector<std::uint8_t> bytes;
    bytes.reserve(tpe::remote::kFrameLengthBytes + length);
    bytes.push_back(static_cast<std::uint8_t>((length >> 24) & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((length >> 16) & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>(length & 0xFFu));
    bytes.push_back(version);
    bytes.push_back(static_cast<std::uint8_t>(type));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

bool sendFrameBytes(SocketHandle handle, tpe::remote::MessageType type,
                    const std::vector<std::uint8_t>& payload)
{
    const std::vector<std::uint8_t> bytes = tpe::remote::encodeFrame(type, payload);
    return sendAllBytes(handle, bytes.data(), bytes.size());
}

/// 有界读取一个完整帧：逐块 recv（单次超时切片 ≤200 ms）并交给 tryDecodeFrame；
/// 超过总期限、对端关闭或解码失败时返回 false 并写入 failure。
bool receiveFrameBefore(SocketHandle handle, std::chrono::milliseconds timeout,
                        tpe::remote::Frame& frame, std::string& failure)
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
        setSocketRecvTimeout(
            handle, (remaining < 200ms) ? remaining : std::chrono::milliseconds{200});
        const int received =
            static_cast<int>(::recv(handle, reinterpret_cast<char*>(chunk), sizeof(chunk), 0));
        if (received > 0) {
            buffer.insert(buffer.end(), chunk, chunk + received);
            const tpe::remote::DecodeResult decoded =
                tpe::remote::tryDecodeFrame(buffer.data(), buffer.size());
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
        // 单次接收超时：继续在总期限内重试
    }
}

/// 对端事件：收到数据 / 对端关闭 / 期限到达。
enum class PeerEvent { Data, Closed, Timeout };

PeerEvent waitForPeerEvent(SocketHandle handle, std::chrono::milliseconds timeout,
                           std::string* receivedData = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::uint8_t chunk[256];
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= 0ms) {
            return PeerEvent::Timeout;
        }
        setSocketRecvTimeout(
            handle, (remaining < 200ms) ? remaining : std::chrono::milliseconds{200});
        const int received =
            static_cast<int>(::recv(handle, reinterpret_cast<char*>(chunk), sizeof(chunk), 0));
        if (received > 0) {
            if (receivedData != nullptr) {
                receivedData->append(reinterpret_cast<const char*>(chunk),
                                     static_cast<std::size_t>(received));
            }
            return PeerEvent::Data;
        }
        if (received == 0) {
            return PeerEvent::Closed;
        }
        const int error = lastSocketError();
        if (isConnectionResetError(error)) {
            return PeerEvent::Closed;
        }
        if (!isRecvTimeoutError(error)) {
            return PeerEvent::Closed; // 其它接收错误按对端不可用处理（有界返回）
        }
    }
}

/// 消费并校验 Client 连接后必然发送的 HELLO 帧。
/// ScriptedServer 的脚本必须先调用它，之后的 Data/Closed 判定才针对握手答复之后的行为；
/// 否则 T023 正确实现时首个对端事件会是 HELLO，脚本会误判为失败。
bool consumeClientHello(SocketHandle handle, std::string& failure)
{
    tpe::remote::Frame hello;
    if (!receiveFrameBefore(handle, kWaitLimit, hello, failure)) {
        return false;
    }
    if (hello.header.version != tpe::remote::kProtocolVersion) {
        failure = "Client HELLO must use protocol version 1";
        return false;
    }
    if (hello.header.type != tpe::remote::MessageType::Hello) {
        failure = "Client must open the session with HELLO";
        return false;
    }
    return true;
}

/// RAII 原始 Client socket（仅 127.0.0.1 回环），析构时关闭。
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

    bool sendBytes(const std::vector<std::uint8_t>& bytes)
    {
        return m_handle != kInvalidSocket &&
               sendAllBytes(m_handle, bytes.data(), bytes.size());
    }

    bool sendFrame(tpe::remote::MessageType type, const std::vector<std::uint8_t>& payload)
    {
        return sendFrameBytes(m_handle, type, payload);
    }

    bool receiveFrame(std::chrono::milliseconds timeout, tpe::remote::Frame& frame,
                      std::string& failure)
    {
        return m_handle != kInvalidSocket &&
               receiveFrameBefore(m_handle, timeout, frame, failure);
    }

    PeerEvent waitForPeer(std::chrono::milliseconds timeout, std::string* receivedData = nullptr)
    {
        return waitForPeerEvent(m_handle, timeout, receivedData);
    }

    void close()
    {
        closeSocketHandle(m_handle);
        m_handle = kInvalidSocket;
    }

    bool valid() const { return m_handle != kInvalidSocket; }

private:
    SocketHandle m_handle = kInvalidSocket;
};

/// 探测 Server 是否已回到 Listening：反复以原始 Client 完成 HELLO/WELCOME。
/// Server 忙碌（BUSY）、瞬时连接失败或没有应答时在总预算内重试；其它协议应答立即失败。
/// 用于验证“握手失败/对端关闭后释放占用，下一连接可被接纳”。
bool expectHandshakeAccepted(std::uint16_t port, std::chrono::milliseconds budget,
                             std::string& failure)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::string lastFailure = "no probe attempted";
    while (std::chrono::steady_clock::now() < deadline) {
        RawSocket probe;
        if (!probe.connectTo(port, lastFailure)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            continue;
        }
        probe.sendFrame(tpe::remote::MessageType::Hello, toBytes("TPE"));
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto slice =
            (remaining < 1000ms) ? remaining : std::chrono::milliseconds{1000};
        tpe::remote::Frame reply;
        if (probe.receiveFrame(slice, reply, lastFailure)) {
            if (reply.header.type == tpe::remote::MessageType::Welcome) {
                if (!reply.payload.empty()) {
                    failure = "WELCOME payload must be empty";
                    return false;
                }
                return true;
            }
            if (reply.header.type != tpe::remote::MessageType::Busy) {
                failure = "unexpected reply type " +
                          std::to_string(static_cast<int>(reply.header.type)) +
                          " while probing for acceptance";
                return false;
            }
            lastFailure = "server is still busy";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    failure = "server did not accept a new connection within the budget; last: " + lastFailure;
    return false;
}

/// "Connection failed." 之后必须还有非空白的原因文本（cli.md §2：原因写标准错误）。
bool hasReasonAfter(const std::string& text, const std::string& prefix)
{
    const std::size_t position = text.find(prefix);
    if (position == std::string::npos) {
        return false;
    }
    for (std::size_t index = position + prefix.size(); index < text.size(); ++index) {
        const char character = text[index];
        if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
            return true;
        }
    }
    return false;
}

/// 脚本化“坏 Server”：独立线程监听 127.0.0.1 动态端口（bind 0 + getsockname），
/// 接纳一次连接后把 socket 交给脚本；脚本返回 false 时记录原因。所有等待有界：
/// accept 以 20 ms select 轮询并检查停止标志，析构 stop + join，不遗留监听端口。
class ScriptedServer {
public:
    /// 脚本：操作已连接 socket；成功返回 true，失败写入 failure。
    using Script = std::function<bool(SocketHandle, std::string&)>;

    explicit ScriptedServer(Script script)
        : m_script(std::move(script))
    {
    }

    ~ScriptedServer()
    {
        stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        closeSocketHandle(m_listenHandle);
        m_listenHandle = kInvalidSocket;
    }

    ScriptedServer(const ScriptedServer&) = delete;
    ScriptedServer& operator=(const ScriptedServer&) = delete;

    bool start()
    {
        if (!ensureSocketsReady()) {
            m_failure = "socket subsystem unavailable";
            return false;
        }
        m_listenHandle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listenHandle == kInvalidSocket) {
            m_failure = "scripted server socket() failed";
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(0); // 动态端口
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(m_listenHandle, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)) != 0) {
            m_failure = "scripted server bind() failed";
            closeSocketHandle(m_listenHandle);
            m_listenHandle = kInvalidSocket;
            return false;
        }
#ifdef _WIN32
        int addressLength = sizeof(address);
#else
        socklen_t addressLength = sizeof(address);
#endif
        if (::getsockname(m_listenHandle, reinterpret_cast<sockaddr*>(&address),
                          &addressLength) != 0) {
            m_failure = "scripted server getsockname() failed";
            closeSocketHandle(m_listenHandle);
            m_listenHandle = kInvalidSocket;
            return false;
        }
        m_port = ntohs(address.sin_port);
        if (::listen(m_listenHandle, 4) != 0) {
            m_failure = "scripted server listen() failed";
            closeSocketHandle(m_listenHandle);
            m_listenHandle = kInvalidSocket;
            return false;
        }
        m_thread = std::thread([this] { run(); });
        return true;
    }

    std::uint16_t port() const { return m_port; }

    bool waitScriptDone(std::chrono::milliseconds timeout)
    {
        return waitUntil(
            [this] { return m_scriptDone.load(std::memory_order_acquire); }, timeout);
    }

    bool scriptExecuted() const { return m_scriptExecuted.load(std::memory_order_acquire); }
    bool scriptSucceeded() const { return m_scriptSucceeded.load(std::memory_order_acquire); }

    std::string scriptFailure() const
    {
        const std::lock_guard<std::mutex> lock(m_failureMutex);
        return m_failure;
    }

private:
    void stop() { m_stop.store(true, std::memory_order_release); }

    void run()
    {
        SocketHandle connection = kInvalidSocket;
        while (!m_stop.load(std::memory_order_acquire)) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(m_listenHandle, &readSet);
            timeval wait{};
            wait.tv_sec = 0;
            wait.tv_usec = 20 * 1000;
#ifdef _WIN32
            const int ready = ::select(0, &readSet, nullptr, nullptr, &wait);
#else
            const int ready =
                ::select(m_listenHandle + 1, &readSet, nullptr, nullptr, &wait);
#endif
            if (ready > 0 && FD_ISSET(m_listenHandle, &readSet)) {
                connection = ::accept(m_listenHandle, nullptr, nullptr);
                break;
            }
        }

        if (connection == kInvalidSocket) {
            {
                const std::lock_guard<std::mutex> lock(m_failureMutex);
                m_failure = "no client connection arrived";
            }
            m_scriptDone.store(true, std::memory_order_release);
            return;
        }

        std::string failure;
        const bool succeeded = m_script(connection, failure);
        closeSocketHandle(connection);
        {
            const std::lock_guard<std::mutex> lock(m_failureMutex);
            m_failure = succeeded ? std::string{} : failure;
        }
        m_scriptSucceeded.store(succeeded, std::memory_order_release);
        m_scriptExecuted.store(true, std::memory_order_release);
        m_scriptDone.store(true, std::memory_order_release);
    }

    SocketHandle m_listenHandle = kInvalidSocket;
    std::uint16_t m_port = 0;
    Script m_script;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_scriptDone{false};
    std::atomic<bool> m_scriptExecuted{false};
    std::atomic<bool> m_scriptSucceeded{false};
    mutable std::mutex m_failureMutex;
    std::string m_failure;
};

// ---------------------------------------------------------------------------
// T026 固定进程快照（通过 ProcessEngine 注入构造控制）
//
// 夹具前提（brief 第 5 条）：进程快照在构造时固定，不读取宿主机真实进程列表；
// “无关进程行的整行增减不影响判定”由快照的不变性保证。断言保持受控行的原始
// 行序，不重排、不过滤。
// ---------------------------------------------------------------------------

/// 快照条目：PID 与展示名（UTF-8）。顺序即 `--all-processes` 的行序。
struct SnapshotProcessEntry {
    tpe::platform::Pid_t pid;
    const char* name;
};

/// 固定快照的 3 个受控进程（含有效非 ASCII 名称；FR-017）。
/// 注入顺序刻意非 PID 单调（4323 → 4321 → 4322）：整块逐字比较时，
/// 任何按 PID 排序的实现都会得到不同行序而被用例拒绝。
constexpr tpe::platform::Pid_t kSnapshotAsciiPid = 4321;
constexpr tpe::platform::Pid_t kSnapshotUtf8Pid = 4322;
constexpr tpe::platform::Pid_t kSnapshotServicePid = 4323;

constexpr const char* kSnapshotAsciiName = "alpha.exe";
constexpr const char* kSnapshotUtf8Name = "中文进程.exe";
constexpr const char* kSnapshotServiceName = "gamma service.exe";

/// 夹具自检用的固定期望文本：与 `processListText()` 的既有行格式逐字一致，
/// 行序 = 注入顺序（4323 → 4321 → 4322），不是 PID 升序。
constexpr const char* kSnapshotListText =
    "PID: 4323 ProcessName: gamma service.exe\n"
    "PID: 4321 ProcessName: alpha.exe\n"
    "PID: 4322 ProcessName: 中文进程.exe\n";

std::vector<SnapshotProcessEntry> snapshotEntries()
{
    return {
        {kSnapshotServicePid, kSnapshotServiceName},
        {kSnapshotAsciiPid, kSnapshotAsciiName},
        {kSnapshotUtf8Pid, kSnapshotUtf8Name},
    };
}

/// 快照进程：只承载 PID 与名称（查询路径所需的全部数据）。
/// 本测试不扫描内存，可扫描页与读写均为“不支持”。
class SnapshotProcess : public tpe::platform::PlatformProcess {
public:
    SnapshotProcess(tpe::platform::Pid_t pid, std::string name)
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
        return tpe::PlatformError{"SnapshotProcess", static_cast<unsigned long>(getPid()), 0,
                                  "not supported"};
    }
};

/// 固定快照平台实现：枚举成功且 Always 返回同一组受控进程。
class SnapshotPlatformOS : public tpe::platform::PlatformOS {
public:
    SnapshotPlatformOS()
    {
        for (const SnapshotProcessEntry& entry : snapshotEntries()) {
            ProcessList.push_back(std::make_shared<SnapshotProcess>(entry.pid, entry.name));
        }
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

/// T027：可配置的失败查询平台实现。
/// 与 SnapshotPlatformOS 共用 SnapshotProcess 数据面（PID + 名称），额外支持：
///   - 任意名称（空名称 = 名称不可读；可含非法 UTF-8 字节）；
///   - 任意数量进程（用于构造超过单条 RESPONSE 上限的列表）；
///   - 枚举失败（enumerationError 与 getAllProcessesPid() 的错误返回保持一致，
///     对应缺陷⑧契约：不得以空列表伪装成功）。
class ConfigurablePlatformOS : public tpe::platform::PlatformOS {
public:
    void addProcess(tpe::platform::Pid_t pid, std::string name)
    {
        ProcessList.push_back(std::make_shared<SnapshotProcess>(pid, std::move(name)));
    }

    void setEnumerationFailure(std::string message)
    {
        m_enumerationError =
            tpe::PlatformError{"ConfigurablePlatformOS", 0, 0, std::move(message)};
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

// ---------------------------------------------------------------------------
// Server / Client 封装
// ---------------------------------------------------------------------------

/// Server + 引擎的组合；析构时保证 stop()（契约：stop 可重复调用）。
class LocalServer {
public:
    explicit LocalServer(std::uint16_t port)
        : m_engine()
        , m_server(port, m_engine)
    {
    }

    /// T026：用调用方提供的平台实现构造引擎（固定进程快照）。
    /// 同一引擎既供 Server 执行远程查询，也供测试取得本地期望文本。
    LocalServer(std::uint16_t port, std::unique_ptr<tpe::platform::PlatformOS> os)
        : m_engine(std::move(os))
        , m_server(port, m_engine)
    {
    }

    ~LocalServer() { m_server.stop(); }

    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;

    bool start() { return m_server.start(); }
    void stop() { m_server.stop(); }
    tpe::remote::RemoteServer& instance() { return m_server; }
    tpe::ProcessEngine& engine() { return m_engine; }

private:
    tpe::ProcessEngine m_engine;
    tpe::remote::RemoteServer m_server;
};

/// 单个 Client 的捕获区与运行状态。
struct ClientCapture {
    TextSink out;
    TextSink err;
    std::atomic<bool> done{false};
    std::atomic<int> exitCode{-1};
};

/// 在独立线程中运行 RemoteClient::run()（内部读写全局 stdin/stdout）。
/// 线程启动后先登记自己的输出槽，确保 Client 文本与 Server 文本分离。
/// 析构兜底：已完成 → join；未完成 → 释放 stdin 后 detach（brief 第 3 条）。
class ClientRun {
public:
    /// timeouts：T021 注入的帧接收期限（默认即契约 30 秒）；仅影响该 Client 实例。
    ClientRun(std::string host, std::uint16_t port,
              tpe::remote::ClientTimeouts timeouts = tpe::remote::ClientTimeouts{})
        : m_host(std::move(host))
        , m_port(port)
        , m_timeouts(timeouts)
        , m_capture(std::make_shared<ClientCapture>())
    {
    }

    ~ClientRun()
    {
        if (!m_thread.joinable()) {
            return;
        }
        if (m_capture->done.load(std::memory_order_acquire)) {
            m_thread.join();
            return;
        }
        stdinBuffer().interrupt();
        m_thread.detach(); // 不对可能永不返回的线程做无界 join
    }

    ClientRun(const ClientRun&) = delete;
    ClientRun& operator=(const ClientRun&) = delete;

    void start()
    {
        auto capture = m_capture;
        const std::string host = m_host;
        const std::uint16_t port = m_port;
        const tpe::remote::ClientTimeouts timeouts = m_timeouts;
        m_thread = std::thread([capture, host, port, timeouts]() {
            const std::thread::id id = std::this_thread::get_id();
            stdoutRouter().registerClient(id, capture->out);
            stderrRouter().registerClient(id, capture->err);
            {
                tpe::remote::RemoteClient client(host, port);
                client.setTimeoutsForTesting(timeouts);
                capture->exitCode.store(client.run(), std::memory_order_relaxed);
            }
            stdoutRouter().unregisterClient(id);
            stderrRouter().unregisterClient(id);
            capture->done.store(true, std::memory_order_release);
        });
    }

    bool waitDone(std::chrono::milliseconds timeout) const
    {
        return waitUntil(
            [this] { return m_capture->done.load(std::memory_order_acquire); }, timeout);
    }

    /// 仅在 run() 已返回后 join；返回 false 时保持未 join 状态由析构兜底。
    bool join()
    {
        if (!m_thread.joinable()) {
            return true;
        }
        if (!m_capture->done.load(std::memory_order_acquire)) {
            return false;
        }
        m_thread.join();
        return true;
    }

    int exitCode() const { return m_capture->exitCode.load(std::memory_order_relaxed); }
    std::string out() const { return m_capture->out.text(); }
    std::string err() const { return m_capture->err.text(); }

private:
    std::string m_host;
    std::uint16_t m_port;
    tpe::remote::ClientTimeouts m_timeouts;
    std::shared_ptr<ClientCapture> m_capture;
    std::thread m_thread;
};

// ---------------------------------------------------------------------------
// 共用场景：Server 启动 → Client 握手进入 TPE> → exit 正常退出
// ---------------------------------------------------------------------------

void expectSuccessfulHandshake(std::uint16_t port)
{
    ScopedRemoteStreams streams;
    LocalServer server(port);

    // 先启动；仅当启动失败时才诊断端口，避免把 TIME_WAIT 误判为“被占用”而跳过。
    if (!server.start()) {
        if (isPortUnavailable(port)) {
            GTEST_SKIP() << "TCP port " << port
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a skeleton failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << port
               << " (skeleton currently returns false: expected RED)";
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";
    ASSERT_TRUE(waitUntil(
        [] { return serverStdoutSink().contains("Waiting for connection ..."); }, kWaitLimit))
        << "Server stdout must contain 'Waiting for connection ...'; got ["
        << capturedServerOut() << "]";

    ClientRun client("127.0.0.1", port);
    client.start();

    if (!waitUntil([&client] { return client.out().find("TPE>") != std::string::npos; },
                   kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        client.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "Client did not display 'TPE>' within 5 s; client stdout=[" << client.out()
               << "] client stderr=[" << client.err() << "] server stdout=["
               << capturedServerOut() << "]";
    }

    // 释放首会话：Client 读取 exit\n 后应正常结束（退出码 0）。
    stdinBuffer().releaseWith("exit\n");
    if (!client.waitDone(kWaitLimit)) {
        server.stop();
        stdinBuffer().interrupt();
        client.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "Client did not return from run() within 5 s after 'exit'; client stdout=["
               << client.out() << "] client stderr=[" << client.err() << "]";
    }
    ASSERT_TRUE(client.join());

    EXPECT_EQ(client.exitCode(), 0) << "normal exit must return exit code 0";
    EXPECT_NE(client.out().find("Connection successful."), std::string::npos)
        << "client stdout=[" << client.out() << "]";
    EXPECT_NE(client.out().find("TPE>"), std::string::npos);
    EXPECT_NE(capturedServerOut().find("Connection successful."), std::string::npos)
        << "server stdout=[" << capturedServerOut() << "]";

    const std::string merged = capturedServerOut() + client.out();
    EXPECT_GE(countOccurrences(merged, "Connection successful."), std::size_t{2})
        << "both endpoints must announce success; merged stdout=[" << merged << "]";

    server.stop();
    EXPECT_TRUE(waitUntil([&server] { return !server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become false after stop()";
}

// ---------------------------------------------------------------------------
// T026 成功查询场景共用辅助
// ---------------------------------------------------------------------------

/// 一次快照会话的结果：Client 捕获、退出码与同一引擎的本地期望文本。
struct SnapshotSessionResult {
    int exitCode = -1;
    std::string out;
    std::string err;
    std::string localListText;   // 同一引擎 processListText()（本地 --all-processes 文本）
    std::string localSearchName; // 传入 searchPid 时为同一引擎 searchProcess() 的结果
};

/// 统一行分隔符（cli.md §5：比较前把 CRLF 统一为 LF；孤立 CR 原样保留）。
std::string normalizedLineEndings(const std::string& text)
{
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
            continue;
        }
        normalized.push_back(text[index]);
    }
    return normalized;
}

/// 在固定快照 Server 上完成一次有界远程会话：
/// 先等待 Client 进入 TPE>，再送入 commands（LF 分隔），随后标准输入返回 EOF
/// （US2 不实现 exit；EOF 由 T023 已有路径结束会话），最后采集 Client 输出、
/// 退出码与同一引擎的本地期望文本。所有等待 ≤5 秒，不做无界 join。
void runSnapshotQuerySession(std::uint16_t port, const std::string& commands,
                             SnapshotSessionResult& result,
                             std::optional<tpe::platform::Pid_t> searchPid = std::nullopt)
{
    ScopedRemoteStreams streams;
    LocalServer server(port, std::make_unique<SnapshotPlatformOS>());

    // 先启动；仅当启动失败时才诊断端口（与既有用例同一策略）。
    if (!server.start()) {
        if (isPortUnavailable(port)) {
            GTEST_SKIP() << "TCP port " << port
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << port;
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    // 本地期望文本：与 Server 执行远程查询使用的是同一个引擎实例。
    const tpe::Result<std::string, tpe::PlatformError> localText =
        server.engine().processListText();
    ASSERT_TRUE(localText.has_value()) << "the fixed snapshot must enumerate successfully";
    result.localListText = localText.value();

    if (searchPid.has_value()) {
        const std::optional<std::string> name = server.engine().searchProcess(*searchPid);
        ASSERT_TRUE(name.has_value()) << "fixture sanity: PID " << *searchPid
                                      << " must exist in the fixed snapshot";
        ASSERT_FALSE(name->empty()) << "fixture sanity: the snapshot name must not be empty";
        result.localSearchName = *name;
    }

    ClientRun client("127.0.0.1", port);
    client.start();

    if (!waitUntil([&client] { return client.out().find("TPE>") != std::string::npos; },
                   kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        client.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "client did not display 'TPE>' within 5 s; client stdout=[" << client.out()
               << "] client stderr=[" << client.err() << "] server stdout=["
               << capturedServerOut() << "]";
    }

    stdinBuffer().releaseWith(commands);

    if (!client.waitDone(kWaitLimit)) {
        server.stop();
        stdinBuffer().interrupt();
        client.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "client did not finish within 5 s after the query commands; client stdout=["
               << client.out() << "] client stderr=[" << client.err() << "]";
    }
    ASSERT_TRUE(client.join());

    result.exitCode = client.exitCode();
    result.out = client.out();
    result.err = client.err();

    server.stop();
}

/// 断言 Client 标准输出中恰有一处与本地结果逐字一致的整块文本：
/// 定位结果块前最近的 `TPE> ` 提示符，要求「该提示符之后 → 下一个提示符之前」
/// 的完整分段与本地整份文本完全相同：无额外空行、无前缀、不重复提示符；
/// 同时断言整块只出现一次。不使用子串式判定（FR-017；FR-007“回到 TPE>”）。
void expectVerbatimQueryBlock(const SnapshotSessionResult& result,
                              const std::string& localBlock)
{
    ASSERT_FALSE(localBlock.empty())
        << "fixture sanity: the local query result must not be empty";
    const std::string clientOut = normalizedLineEndings(result.out);

    ASSERT_EQ(countOccurrences(clientOut, localBlock), std::size_t{1})
        << "client stdout must contain the whole multi-line result verbatim exactly once;\n"
        << "expected block=[" << localBlock << "]\nclient stdout=[" << clientOut
        << "]\nclient stderr=[" << result.err << "]";

    const std::string prompt(kPromptText);
    const std::size_t blockPosition = clientOut.find(localBlock);
    const std::size_t promptBefore = clientOut.rfind(prompt, blockPosition);
    ASSERT_NE(promptBefore, std::string::npos)
        << "the query result must be rendered after a 'TPE> ' prompt; stdout=[" << clientOut
        << "]";

    const std::size_t segmentStart = promptBefore + prompt.size();
    const std::size_t promptAfter = clientOut.find(prompt, segmentStart);
    ASSERT_NE(promptAfter, std::string::npos)
        << "the client must return to the 'TPE> ' prompt after the result; stdout=["
        << clientOut << "]";

    const std::string segment = clientOut.substr(segmentStart, promptAfter - segmentStart);
    EXPECT_EQ(segment, localBlock)
        << "the segment between the nearest prompt and the following prompt must equal the "
           "local result verbatim (no extra blank line, prefix, or repeated prompt);\n"
        << "segment=[" << segment << "]\nexpected=[" << localBlock << "]\nclient stdout=["
        << clientOut << "]";
}

// ---------------------------------------------------------------------------
// T027 失败查询与发送前校验共用辅助
//
// 固定文本按契约逐字复制（tests 不跨 TU 引用实现内部常量，实现漂移时由断言暴露）：
//   - contracts/cli.md §4：帮助文本；
//   - contracts/cli.md §3/§6：未知命令与查询失败文本；
//   - contracts/cli.md §3.1：发送前校验文本。
// ---------------------------------------------------------------------------

constexpr const char* kHelpText =
    "--all-processes        List processes on the server.\n"
    "--search-process <PID> Show the name of a process on the server.\n"
    "help                   Show this help.\n"
    "exit                   End this client session.";

constexpr const char* kUnknownCommandText =
    "Unknown command. Type 'help' for available commands.";
constexpr const char* kOversizeResultText =
    "Response exceeds the maximum message size.";
constexpr const char* kUnencodableNameText = "Process name cannot be encoded as UTF-8.";
constexpr const char* kUnencodableListText =
    "Process list contains a name that cannot be encoded as UTF-8.";
constexpr const char* kOversizeCommandText =
    "Command exceeds the maximum message size.";
constexpr const char* kUnencodableCommandText =
    "Command cannot be encoded as UTF-8.";
constexpr const char* kLineBreakCommandText =
    "Command must not contain line breaks.";

/// 非法 UTF-8 字节（0xFF 永不出现于合法 UTF-8 序列）的进程名。
std::string invalidUtf8ProcessName()
{
    std::string name = "bad";
    name.push_back(static_cast<char>(0xFF));
    name += "-name.exe";
    return name;
}

/// 超限夹具的进程名；kOversizeProcessCount 条合计超过 1 MiB。
constexpr int kOversizeProcessCount = 20000;

std::string oversizeProcessName(int index)
{
    return "bulk-process-" + std::to_string(index) + std::string(32, 'x');
}

/// 一次失败查询会话的结果。
struct QueryFailureSessionResult {
    int exitCode = -1;
    std::string out;
    std::string err;
};

/// 在固定快照 Server 上完成一次「失败命令 + help」会话：
/// 先等待 Client 进入 TPE>，再送入 commands（失败命令在前、help 在后），随后标准输入
/// EOF（US2 不实现 exit；EOF 走 T023 已有正常结束路径）。所有等待 ≤5 秒。
void runQueryFailureSession(std::uint16_t port,
                            std::unique_ptr<tpe::platform::PlatformOS> os,
                            const std::string& commands,
                            QueryFailureSessionResult& result)
{
    ScopedRemoteStreams streams;
    LocalServer server(port, std::move(os));

    // 先启动；仅当启动失败时才诊断端口（与既有用例同一策略）。
    if (!server.start()) {
        if (isPortUnavailable(port)) {
            GTEST_SKIP() << "TCP port " << port
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << port;
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    ClientRun client("127.0.0.1", port);
    client.start();

    if (!waitUntil([&client] { return client.out().find("TPE>") != std::string::npos; },
                   kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        client.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "client did not display 'TPE>' within 5 s; client stdout=[" << client.out()
               << "] client stderr=[" << client.err() << "] server stdout=["
               << capturedServerOut() << "]";
    }

    stdinBuffer().releaseWith(commands);

    if (!client.waitDone(kWaitLimit)) {
        server.stop();
        stdinBuffer().interrupt();
        client.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "client did not finish within 5 s after the failing query; client stdout=["
               << client.out() << "] client stderr=[" << client.err() << "]";
    }
    ASSERT_TRUE(client.join());

    result.exitCode = client.exitCode();
    result.out = client.out();
    result.err = client.err();

    server.stop();
}

/// 断言：失败文本恰出现 1 次 → 回到 TPE> → help 的 4 行帮助逐字出现 1 次 → 再次出现
/// 提示符（会话仍可用）。cli.md §6：查询失败保留 TPE>、Client 退出码 0。
void expectFailureThenHelp(const QueryFailureSessionResult& result,
                           const std::string& expectedFailure)
{
    EXPECT_EQ(result.exitCode, 0)
        << "a failed query must keep the session alive (exit code 0 after EOF); stderr=["
        << result.err << "]";

    const std::string clientOut = normalizedLineEndings(result.out);

    ASSERT_EQ(countOccurrences(clientOut, expectedFailure), std::size_t{1})
        << "expected the failure text exactly once; expected=[" << expectedFailure
        << "] client stdout=[" << clientOut << "] client stderr=[" << result.err << "]";

    const std::size_t failurePosition = clientOut.find(expectedFailure);
    const std::size_t promptAfterFailure =
        clientOut.find("TPE>", failurePosition + expectedFailure.size());
    ASSERT_NE(promptAfterFailure, std::string::npos)
        << "the client must return to 'TPE>' after a failed query; stdout=[" << clientOut
        << "]";

    const std::string helpBlock(kHelpText);
    ASSERT_EQ(countOccurrences(clientOut, helpBlock), std::size_t{1})
        << "the subsequent 'help' must render the four-line help text exactly once; stdout=["
        << clientOut << "]";
    const std::size_t helpPosition = clientOut.find(helpBlock);
    EXPECT_LT(promptAfterFailure, helpPosition)
        << "the help output must follow the failure and the returned prompt; stdout=["
        << clientOut << "]";
    EXPECT_NE(clientOut.find("TPE>", helpPosition + helpBlock.size()), std::string::npos)
        << "the prompt must follow the help output, proving the session is still usable; "
           "stdout=["
        << clientOut << "]";
}

/// 一次发送前校验会话的结果（含脚本化 Server 对「未发送」的观察）。
struct PreSendSessionResult {
    int exitCode = -1;
    std::string out;
    std::string err;
    bool scriptSucceeded = false;
    std::string scriptFailure;
};

/// 脚本化 Server：完成握手后要求 Client 发出的第一条（也是唯一一条）COMMAND 是 help。
/// 若本地拒绝的命令被错误发送，脚本会看到不同的首帧并以明确原因失败；这同时证明
/// 被拒命令未发送、也不会产生任何 Server 响应（不依赖 T041 的查询日志）。
/// 随后回 help 的 RESPONSE，并在 Client 因标准输入 EOF 发送 BYE 后回空 BYE。
void runPreSendRejectionSession(const std::string& rejectedLine,
                                PreSendSessionResult& result)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!consumeClientHello(socket, failure)) {
            return false;
        }
        if (!sendFrameBytes(socket, tpe::remote::MessageType::Welcome, {})) {
            failure = "failed to send WELCOME";
            return false;
        }

        tpe::remote::Frame frame;
        if (!receiveFrameBefore(socket, kWaitLimit, frame, failure)) {
            return false;
        }
        if (frame.header.type != tpe::remote::MessageType::Command) {
            failure = "locally rejected input must not be sent; expected COMMAND(help) as the "
                      "first frame after WELCOME, got type " +
                      std::to_string(static_cast<int>(frame.header.type));
            return false;
        }
        const std::string commandLine(frame.payload.begin(), frame.payload.end());
        if (commandLine != "help") {
            const std::string preview = commandLine.substr(0, 32);
            failure = "locally rejected input must not be sent; expected COMMAND(help) "
                      "first, got [" +
                      preview + "] (" + std::to_string(commandLine.size()) + " byte(s))";
            return false;
        }

        const tpe::Result<std::vector<std::uint8_t>, tpe::remote::ProtocolError> payload =
            tpe::remote::encodeResponse(tpe::remote::ResponseStatus::Continue, "TPE>",
                                        kHelpText);
        if (!payload.has_value()) {
            failure = "failed to encode the help RESPONSE";
            return false;
        }
        if (!sendFrameBytes(socket, tpe::remote::MessageType::Response, payload.value())) {
            failure = "failed to send the help RESPONSE";
            return false;
        }

        // 标准输入 EOF → BYE；回 BYE 确认（T023 正常结束路径）。
        if (!receiveFrameBefore(socket, kWaitLimit, frame, failure)) {
            return false;
        }
        if (frame.header.type != tpe::remote::MessageType::Bye) {
            failure = "after the help response the client must end with BYE; got type " +
                      std::to_string(static_cast<int>(frame.header.type));
            return false;
        }
        if (!sendFrameBytes(socket, tpe::remote::MessageType::Bye, {})) {
            failure = "failed to acknowledge BYE";
            return false;
        }
        waitForPeerEvent(socket, std::chrono::milliseconds{1000});
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();

    if (!waitUntil([&client] { return client.out().find("TPE>") != std::string::npos; },
                   kWaitLimit)) {
        stdinBuffer().interrupt();
        client.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "client did not display 'TPE>' within 5 s; client stdout=[" << client.out()
               << "] client stderr=[" << client.err() << "]";
    }

    stdinBuffer().releaseWith(rejectedLine + "help\n");

    if (!client.waitDone(kWaitLimit)) {
        stdinBuffer().interrupt();
        client.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "client did not finish within 5 s after the rejected command; client "
                  "stdout=["
               << client.out() << "] client stderr=[" << client.err() << "]";
    }
    ASSERT_TRUE(client.join());

    result.exitCode = client.exitCode();
    result.out = client.out();
    result.err = client.err();

    EXPECT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    result.scriptSucceeded = server.scriptSucceeded();
    result.scriptFailure = server.scriptFailure();
}

/// 断言：本地拒绝文本恰出现 1 次且写标准输出 → 不出现未知命令响应 → 回到 TPE> →
/// help 的 4 行帮助逐字出现 1 次 → 再次出现提示符。
/// 「未发送」由 runPreSendRejectionSession 的脚本化 Server 单独证明：
/// 握手后第一条 COMMAND 必须是 help。Client 退出码留到 US3 验证。
void expectLocalRejectionThenHelp(const PreSendSessionResult& result,
                                  const std::string& expectedError)
{
    EXPECT_TRUE(result.scriptSucceeded)
        << "the scripted server must observe COMMAND(help) as the first frame after "
           "WELCOME; "
        << result.scriptFailure;

    const std::string clientOut = normalizedLineEndings(result.out);

    ASSERT_EQ(countOccurrences(clientOut, expectedError), std::size_t{1})
        << "expected the local rejection text exactly once; expected=[" << expectedError
        << "] client stdout=[" << clientOut << "] client stderr=[" << result.err << "]";
    EXPECT_EQ(result.err.find(expectedError), std::string::npos)
        << "cli.md §3.1: the local rejection text must be written to standard output; "
           "stderr=["
        << result.err << "]";

    EXPECT_EQ(clientOut.find(kUnknownCommandText), std::string::npos)
        << "a locally rejected command must not be sent, so the server must not produce an "
           "unknown-command response; stdout=["
        << clientOut << "]";

    const std::size_t rejectionPosition = clientOut.find(expectedError);
    const std::size_t promptAfterRejection =
        clientOut.find("TPE>", rejectionPosition + expectedError.size());
    ASSERT_NE(promptAfterRejection, std::string::npos)
        << "the client must keep 'TPE>' after a local rejection; stdout=[" << clientOut
        << "]";

    const std::string helpBlock(kHelpText);
    ASSERT_EQ(countOccurrences(clientOut, helpBlock), std::size_t{1})
        << "the subsequent 'help' must render the four-line help text exactly once; stdout=["
        << clientOut << "]";
    const std::size_t helpPosition = clientOut.find(helpBlock);
    EXPECT_LT(promptAfterRejection, helpPosition)
        << "the help output must follow the rejection and the returned prompt; stdout=["
        << clientOut << "]";
    EXPECT_NE(clientOut.find("TPE>", helpPosition + helpBlock.size()), std::string::npos)
        << "the prompt must follow the help output; stdout=[" << clientOut << "]";
}

} // namespace

// ---------------------------------------------------------------------------
// 套件 RemoteSession — 回环握手（brief 用例 1–5）
// ---------------------------------------------------------------------------

/// brief 用例 1：自定义端口 start() 成功、isRunning() 为 true；
/// stop() 后为 false；可重复 stop()。
TEST(RemoteSession, ServerStartsAndStops)
{
    ScopedRemoteStreams streams;
    LocalServer server(kLifecyclePort);

    // 先启动；仅当启动失败时才诊断端口，避免把 TIME_WAIT 误判为“被占用”而跳过。
    if (!server.start()) {
        if (isPortUnavailable(kLifecyclePort)) {
            GTEST_SKIP() << "TCP port " << kLifecyclePort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a skeleton failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kLifecyclePort
               << " (skeleton currently returns false: expected RED)";
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";
    EXPECT_TRUE(server.instance().isRunning());

    server.stop();
    EXPECT_TRUE(waitUntil([&server] { return !server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become false after stop()";
    EXPECT_FALSE(server.instance().isRunning());

    // 契约：stop() 可重复调用且无副作用。
    server.stop();
    EXPECT_FALSE(server.instance().isRunning());
}

/// brief 用例 2：默认端口 8008 的回环握手（quickstart 场景 1）。
TEST(RemoteSession, DefaultPortHandshake)
{
    expectSuccessfulHandshake(kDefaultPort);
}

/// brief 用例 3：自定义端口 18081 的回环握手（quickstart 场景 2）。
TEST(RemoteSession, CustomPortHandshake)
{
    expectSuccessfulHandshake(kCustomPort);
}

/// brief 用例 4：首 Client 停留在 TPE> 时，第二 Client 收到 BUSY；
/// 首 Client 之后仍能正常退出（quickstart 场景 4；protocol §3）。
TEST(RemoteSession, SecondClientDuringHandshakeGetsBusy)
{
    ScopedRemoteStreams streams;
    LocalServer server(kBusyRejectionPort);

    // 先启动；仅当启动失败时才诊断端口，避免把 TIME_WAIT 误判为“被占用”而跳过。
    if (!server.start()) {
        if (isPortUnavailable(kBusyRejectionPort)) {
            GTEST_SKIP() << "TCP port " << kBusyRejectionPort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a skeleton failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kBusyRejectionPort
               << " (skeleton currently returns false: expected RED)";
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    ClientRun first("127.0.0.1", kBusyRejectionPort);
    first.start();
    if (!waitUntil([&first] { return first.out().find("TPE>") != std::string::npos; },
                   kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        first.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "first client did not reach 'TPE>' within 5 s; stdout=[" << first.out()
               << "] stderr=[" << first.err() << "]";
    }

    // 首会话处于 Active：第二连接必须立即收到 BUSY 并退出 1。
    ClientRun second("127.0.0.1", kBusyRejectionPort);
    second.start();
    if (!second.waitDone(kWaitLimit)) {
        server.stop();
        stdinBuffer().interrupt();
        second.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "second client did not return within 5 s; stdout=[" << second.out()
               << "] stderr=[" << second.err() << "]";
    }
    ASSERT_TRUE(second.join());
    EXPECT_EQ(second.exitCode(), 1) << "BUSY must end the client with exit code 1";
    EXPECT_NE(second.err().find("Connection failed."), std::string::npos)
        << "second client stderr=[" << second.err() << "]";
    EXPECT_NE(second.err().find("Server is busy."), std::string::npos)
        << "second client stderr=[" << second.err() << "]";

    // 首 Client 不受影响：释放后正常退出 0。
    stdinBuffer().releaseWith("exit\n");
    if (!first.waitDone(kWaitLimit)) {
        server.stop();
        stdinBuffer().interrupt();
        first.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "first client did not exit within 5 s after 'exit'; stdout=[" << first.out()
               << "] stderr=[" << first.err() << "]";
    }
    ASSERT_TRUE(first.join());
    EXPECT_EQ(first.exitCode(), 0) << "first client must remain healthy after a BUSY rejection";
    EXPECT_NE(first.out().find("TPE>"), std::string::npos);

    server.stop();
}

/// brief 用例 5：Client 先看到 "Connection successful."，之后才出现首个 "TPE>"。
/// 即 WELCOME 完成握手前不输出提示符（cli.md §2）。
TEST(RemoteSession, WelcomeCompletesHandshake)
{
    ScopedRemoteStreams streams;
    LocalServer server(kWelcomePort);

    // 先启动；仅当启动失败时才诊断端口，避免把 TIME_WAIT 误判为“被占用”而跳过。
    if (!server.start()) {
        if (isPortUnavailable(kWelcomePort)) {
            GTEST_SKIP() << "TCP port " << kWelcomePort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a skeleton failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kWelcomePort
               << " (skeleton currently returns false: expected RED)";
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    ClientRun client("127.0.0.1", kWelcomePort);
    client.start();

    if (!waitUntil([&client] { return client.out().find("TPE>") != std::string::npos; },
                   kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        client.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "client did not display 'TPE>' within 5 s; client stdout=[" << client.out()
               << "] client stderr=[" << client.err() << "]";
    }

    const std::string clientOut = client.out();
    const std::size_t successPosition = clientOut.find("Connection successful.");
    const std::size_t promptPosition = clientOut.find("TPE>");
    ASSERT_NE(successPosition, std::string::npos)
        << "client stdout=[" << clientOut << "]";
    ASSERT_NE(promptPosition, std::string::npos) << "client stdout=[" << clientOut << "]";
    EXPECT_LT(successPosition, promptPosition)
        << "Client must print 'Connection successful.' before the first 'TPE>'; stdout=["
        << clientOut << "]";

    stdinBuffer().releaseWith("exit\n");
    if (!client.waitDone(kWaitLimit)) {
        server.stop();
        stdinBuffer().interrupt();
        client.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "client did not exit within 5 s after 'exit'; stdout=[" << client.out() << "]";
    }
    ASSERT_TRUE(client.join());
    EXPECT_EQ(client.exitCode(), 0);

    server.stop();
}

// ---------------------------------------------------------------------------
// T021 握手失败用例（brief 用例 1–8；真实 Server 或脚本化原始 socket Server）
//
// 时序策略：用注入的毫秒级期限模拟契约的 30 秒/5 秒规则。Windows 计时粒度约
// 15.6 ms，sleep 可能晚到，因此边界类用例保留 > 计时粒度的余量：
//   - 首字节边界用例 B 的首字节在 260 ms 发出（注入期限 200 ms + 60 ms 余量）；
//   - 其余“未超过期限”的发送点比期限至少提前 60 ms。
// 所有等待 ≤5 秒；Client 结束后若脚本仍在等待，析构 stop + join 有界退出。
// ---------------------------------------------------------------------------

/// T021 用例 1（Client 侧）：Server 回 ERROR(UnsupportedVersion) → Client 显示
/// "Connection failed." 与具体原因、退出码 1（cli.md §8；quickstart 场景 10）。
TEST(RemoteSession, ClientReportsUnsupportedVersionFromServer)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        tpe::remote::Frame hello;
        if (!receiveFrameBefore(socket, kWaitLimit, hello, failure)) {
            return false;
        }
        if (hello.header.version != tpe::remote::kProtocolVersion) {
            failure = "Client HELLO must use protocol version 1";
            return false;
        }
        if (hello.header.type != tpe::remote::MessageType::Hello) {
            failure = "Client must open the session with HELLO";
            return false;
        }
        std::vector<std::uint8_t> payload{
            static_cast<std::uint8_t>(tpe::remote::ErrorCode::UnsupportedVersion)};
        const std::string reason = "Unsupported protocol version.";
        payload.insert(payload.end(), reason.begin(), reason.end());
        if (!sendFrameBytes(socket, tpe::remote::MessageType::Error, payload)) {
            failure = "failed to send ERROR(UnsupportedVersion)";
            return false;
        }
        if (waitForPeerEvent(socket, std::chrono::milliseconds{2000}) != PeerEvent::Closed) {
            failure = "Client must close the connection after a handshake ERROR";
            return false;
        }
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();
    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());

    ASSERT_EQ(client.exitCode(), 1) << "unhandled handshake ERROR must exit 1";
    ASSERT_NE(client.err().find("Connection failed."), std::string::npos)
        << "client stderr=[" << client.err() << "]";
    ASSERT_TRUE(hasReasonAfter(client.err(), "Connection failed."))
        << "Client must print the specific reason after 'Connection failed.'; stderr=["
        << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// T021 用例 1（Server 侧）：版本 2 的 HELLO → ERROR(1) 并关闭；随后回到 Listening，
/// 下一连接发送合法 HELLO 可得到 WELCOME（protocol §4；quickstart 场景 10）。
TEST(RemoteSession, ServerRejectsVersionTwoHelloAndAcceptsNextClient)
{
    ScopedRemoteStreams streams;
    LocalServer server(kVersionRejectPort);

    // 先启动；仅当启动失败时才诊断端口，避免把 TIME_WAIT 误判为“被占用”而跳过。
    if (!server.start()) {
        if (isPortUnavailable(kVersionRejectPort)) {
            GTEST_SKIP() << "TCP port " << kVersionRejectPort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a skeleton failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kVersionRejectPort
               << " (skeleton currently returns false: expected RED)";
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    std::string failure;
    RawSocket first;
    ASSERT_TRUE(first.connectTo(kVersionRejectPort, failure)) << failure;
    ASSERT_TRUE(
        first.sendBytes(encodeRawFrame(2, tpe::remote::MessageType::Hello, toBytes("TPE"))))
        << "failed to send a version-2 HELLO";

    tpe::remote::Frame reply;
    ASSERT_TRUE(first.receiveFrame(kWaitLimit, reply, failure)) << failure;
    ASSERT_EQ(static_cast<int>(reply.header.type),
              static_cast<int>(tpe::remote::MessageType::Error))
        << "Server must answer a version-2 HELLO with ERROR";
    ASSERT_FALSE(reply.payload.empty()) << "ERROR payload = code(1) + reason";
    EXPECT_EQ(reply.payload.at(0),
              static_cast<std::uint8_t>(tpe::remote::ErrorCode::UnsupportedVersion));
    EXPECT_EQ(first.waitForPeer(kWaitLimit), PeerEvent::Closed)
        << "Server must close the connection after rejecting the version";
    first.close();

    ASSERT_TRUE(expectHandshakeAccepted(kVersionRejectPort, kWaitLimit, failure))
        << "after a version rejection the server must return to Listening; " << failure;
    server.stop();
}

/// T021 用例 2：Client 连接后不发任何字节；Server 注入 100 ms 握手期限 →
/// 发送 ERROR(4)、关闭并释放占用；下一连接可被接纳（protocol §4；quickstart 场景 10）。
TEST(RemoteSession, ServerHandshakeTimeoutReleasesSlot)
{
    ScopedRemoteStreams streams;
    LocalServer server(kHandshakeTimeoutPort);
    server.instance().setHandshakeTimeoutForTesting(std::chrono::milliseconds{100});

    if (!server.start()) {
        if (isPortUnavailable(kHandshakeTimeoutPort)) {
            GTEST_SKIP() << "TCP port " << kHandshakeTimeoutPort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a skeleton failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kHandshakeTimeoutPort
               << " (skeleton currently returns false: expected RED)";
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    std::string failure;
    RawSocket silent;
    ASSERT_TRUE(silent.connectTo(kHandshakeTimeoutPort, failure)) << failure;
    const auto connectedAt = std::chrono::steady_clock::now();

    tpe::remote::Frame reply;
    ASSERT_TRUE(silent.receiveFrame(std::chrono::milliseconds{3000}, reply, failure))
        << "server must answer a silent handshake with ERROR inside the injected 100 ms "
           "deadline; "
        << failure;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - connectedAt);
    EXPECT_GE(elapsed, std::chrono::milliseconds{80})
        << "ERROR must not arrive before the injected handshake deadline "
           "(100 ms - 20 ms tolerance); arrived after "
        << elapsed.count() << " ms";
    EXPECT_LT(elapsed, std::chrono::milliseconds{500})
        << "ERROR must arrive near the injected 100 ms handshake deadline; arrived after "
        << elapsed.count() << " ms (an implementation ignoring the injected timeout would fail)";

    ASSERT_EQ(static_cast<int>(reply.header.type),
              static_cast<int>(tpe::remote::MessageType::Error));
    ASSERT_FALSE(reply.payload.empty());
    EXPECT_EQ(reply.payload.at(0),
              static_cast<std::uint8_t>(tpe::remote::ErrorCode::HandshakeTimeout));
    EXPECT_EQ(silent.waitForPeer(kWaitLimit), PeerEvent::Closed)
        << "Server must close the connection after the handshake timeout";
    silent.close();

    ASSERT_TRUE(expectHandshakeAccepted(kHandshakeTimeoutPort, kWaitLimit, failure))
        << "after a handshake timeout the server must return to Listening; " << failure;
    server.stop();
}

/// T021 用例 3：Client 在握手中主动关闭 → Server 释放占用，下一连接可被接纳
/// （protocol §3；quickstart 场景 17）。
TEST(RemoteSession, ServerReleasesSlotWhenPeerClosesDuringHandshake)
{
    ScopedRemoteStreams streams;
    LocalServer server(kPeerClosePort);

    if (!server.start()) {
        if (isPortUnavailable(kPeerClosePort)) {
            GTEST_SKIP() << "TCP port " << kPeerClosePort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a skeleton failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kPeerClosePort
               << " (skeleton currently returns false: expected RED)";
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    {
        std::string failure;
        RawSocket closing;
        ASSERT_TRUE(closing.connectTo(kPeerClosePort, failure)) << failure;
        closing.close(); // 握手完成前主动关闭
    }

    std::string failure;
    ASSERT_TRUE(expectHandshakeAccepted(kPeerClosePort, kWaitLimit, failure))
        << "after a peer close during handshake the server must return to Listening; "
        << failure;
    server.stop();
}

/// T021 用例 4：注入 firstByte=100 ms，Server 不发任何字节 →
/// Client 显示 "Connection failed." + "No response from server."、退出码 1
/// （cli.md §7；quickstart 场景 7/10）。
TEST(RemoteSession, ClientReportsSilentServer)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!consumeClientHello(socket, failure)) {
            return false;
        }
        // 不发任何字节；等 Client 在注入的 100 ms 静默期限后关闭连接。
        if (waitForPeerEvent(socket, std::chrono::milliseconds{2000}) != PeerEvent::Closed) {
            failure = "Client must close the connection after the first-byte deadline";
            return false;
        }
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    tpe::remote::ClientTimeouts timeouts;
    timeouts.firstByte = std::chrono::milliseconds{100};
    ClientRun client("127.0.0.1", server.port(), timeouts);
    client.start();

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    ASSERT_EQ(client.exitCode(), 1) << "silent server must end the client with exit code 1";
    ASSERT_NE(client.err().find("Connection failed."), std::string::npos)
        << "client stderr=[" << client.err() << "]";
    ASSERT_NE(client.err().find("No response from server."), std::string::npos)
        << "client stderr=[" << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// T021 用例 5A：注入 firstByte=200 ms；首字节在 120 ms 发出、完整 BUSY 帧在
/// 首字节后 100 ms 内到达 → Client 以忙碌结束（退出码 1，显示 "Server is busy."）。
TEST(RemoteSession, ClientAcceptsBusyWhenFirstByteArrivesBeforeDeadline)
{
    ScopedRemoteStreams streams;
    const std::vector<std::uint8_t> busyFrame =
        tpe::remote::encodeFrame(tpe::remote::MessageType::Busy, toBytes("Server is busy."));
    ScriptedServer server([busyFrame](SocketHandle socket, std::string& failure) {
        if (!consumeClientHello(socket, failure)) {
            return false;
        }
        // 首字节 120 ms：比注入期限早 80 ms（> Windows 15.6 ms 计时粒度）。
        std::this_thread::sleep_for(std::chrono::milliseconds{120});
        if (!sendAllBytes(socket, busyFrame.data(), 1)) {
            failure = "failed to send the first BUSY byte";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        if (!sendAllBytes(socket, busyFrame.data() + 1, busyFrame.size() - 1)) {
            failure = "failed to send the remaining BUSY bytes";
            return false;
        }
        if (waitForPeerEvent(socket, std::chrono::milliseconds{2000}) != PeerEvent::Closed) {
            failure = "Client must close the connection after receiving BUSY";
            return false;
        }
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    tpe::remote::ClientTimeouts timeouts;
    timeouts.firstByte = std::chrono::milliseconds{200};
    ClientRun client("127.0.0.1", server.port(), timeouts);
    client.start();

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    ASSERT_EQ(client.exitCode(), 1) << "BUSY must end the client with exit code 1";
    ASSERT_NE(client.err().find("Connection failed."), std::string::npos)
        << "client stderr=[" << client.err() << "]";
    ASSERT_NE(client.err().find("Server is busy."), std::string::npos)
        << "a first byte before the 200 ms deadline must be treated as BUSY; stderr=["
        << client.err() << "]";
    EXPECT_EQ(client.err().find("No response from server."), std::string::npos)
        << "a first byte inside the deadline must not be reported as silence";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// T021 用例 5B：注入 firstByte=200 ms；首字节 260 ms 才到达（晚于期限）→
/// 即使随后收到完整 BUSY，也按静默超时结束（"No response from server."）。
TEST(RemoteSession, ClientReportsSilentTimeoutWhenFirstByteIsLate)
{
    ScopedRemoteStreams streams;
    const std::vector<std::uint8_t> busyFrame =
        tpe::remote::encodeFrame(tpe::remote::MessageType::Busy, toBytes("Server is busy."));
    ScriptedServer server([busyFrame](SocketHandle socket, std::string&) {
        // 首字节 260 ms：晚于注入期限 200 ms（超出 ±20 ms 容差余量）。
        // Client 此时已按静默超时关闭，本次发送可能失败，属预期。
        std::this_thread::sleep_for(std::chrono::milliseconds{260});
        sendAllBytes(socket, busyFrame.data(), busyFrame.size());
        waitForPeerEvent(socket, std::chrono::milliseconds{1000});
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    tpe::remote::ClientTimeouts timeouts;
    timeouts.firstByte = std::chrono::milliseconds{200};
    ClientRun client("127.0.0.1", server.port(), timeouts);
    client.start();

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    ASSERT_EQ(client.exitCode(), 1);
    ASSERT_NE(client.err().find("No response from server."), std::string::npos)
        << "a first byte after the deadline must be reported as silence; stderr=["
        << client.err() << "]";
    EXPECT_EQ(client.err().find("Server is busy."), std::string::npos)
        << "a late first byte must not be treated as BUSY; stderr=[" << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// T021 用例 6：注入 frameComplete=200 ms；首字节后再无字节 →
/// Client 显示 "Server sent an incomplete message."、退出码 1（cli.md §7）。
TEST(RemoteSession, ClientReportsIncompleteFrame)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!consumeClientHello(socket, failure)) {
            return false;
        }
        const std::vector<std::uint8_t> busyFrame =
            tpe::remote::encodeFrame(tpe::remote::MessageType::Busy, toBytes("Server is busy."));
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        // 只发送完整帧头（length+version+type）：首字节已到达，载荷永不到达。
        const std::size_t partial = tpe::remote::kFrameLengthBytes + 2;
        if (!sendAllBytes(socket, busyFrame.data(), partial)) {
            failure = "failed to send the partial frame";
            return false;
        }
        if (waitForPeerEvent(socket, std::chrono::milliseconds{2000}) != PeerEvent::Closed) {
            failure = "Client must close the connection after the frame-complete deadline";
            return false;
        }
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    tpe::remote::ClientTimeouts timeouts;
    timeouts.firstByte = std::chrono::milliseconds{2000};
    timeouts.frameComplete = std::chrono::milliseconds{200};
    ClientRun client("127.0.0.1", server.port(), timeouts);
    client.start();

    ASSERT_TRUE(client.waitDone(std::chrono::milliseconds{1500}))
        << "client must return after the injected frameComplete deadline (200 ms), "
           "well before the 2000 ms firstByte deadline";
    ASSERT_TRUE(client.join());
    ASSERT_EQ(client.exitCode(), 1);
    ASSERT_NE(client.err().find("Server sent an incomplete message."), std::string::npos)
        << "client stderr=[" << client.err() << "]";
    EXPECT_EQ(client.err().find("Server is busy."), std::string::npos)
        << "an incomplete frame must not be treated as BUSY; stderr=[" << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// T021 用例 7：握手期收到完整的 RESPONSE 帧 →
/// Client 显示 "Server sent an unexpected message."、退出码 1（protocol §4）。
TEST(RemoteSession, ClientReportsUnexpectedHandshakeType)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!consumeClientHello(socket, failure)) {
            return false;
        }
        const auto payload =
            tpe::remote::encodeResponse(tpe::remote::ResponseStatus::Continue, "", "");
        if (!payload.has_value()) {
            failure = "failed to encode a RESPONSE payload";
            return false;
        }
        if (!sendFrameBytes(socket, tpe::remote::MessageType::Response, payload.value())) {
            failure = "failed to send the RESPONSE frame";
            return false;
        }
        if (waitForPeerEvent(socket, std::chrono::milliseconds{2000}) != PeerEvent::Closed) {
            failure = "Client must close the connection after an unexpected handshake frame";
            return false;
        }
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    ASSERT_EQ(client.exitCode(), 1);
    ASSERT_NE(client.err().find("Server sent an unexpected message."), std::string::npos)
        << "client stderr=[" << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// T021 用例 8：WELCOME 载荷非空 → Client 显示 "Server sent an invalid message."、
/// 退出码 1，且不回 ERROR（脚本断言未收到任何客户端字节；protocol §4）。
TEST(RemoteSession, ClientReportsInvalidWelcomePayload)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!consumeClientHello(socket, failure)) {
            return false;
        }
        if (!sendFrameBytes(socket, tpe::remote::MessageType::Welcome, toBytes("x"))) {
            failure = "failed to send the non-empty WELCOME";
            return false;
        }
        std::string extra;
        const PeerEvent event = waitForPeerEvent(socket, std::chrono::milliseconds{2000}, &extra);
        if (event == PeerEvent::Data) {
            failure = "Client must not send any frame after an invalid handshake reply; received " +
                      std::to_string(extra.size()) + " byte(s)";
            return false;
        }
        if (event != PeerEvent::Closed) {
            failure = "Client must close the connection after an invalid handshake reply";
            return false;
        }
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    ASSERT_EQ(client.exitCode(), 1);
    ASSERT_NE(client.err().find("Server sent an invalid message."), std::string::npos)
        << "client stderr=[" << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

// ---------------------------------------------------------------------------
// 套件 RemoteSessionQuerySuccess — US2 成功查询（T026）
//
// 夹具前提（brief 第 5 条）：上面固定的 3 个受控进程构成快照，不读取宿主机真实
// 进程列表，因此“无关进程行的整行增减”不在快照内发生、不影响判定；断言对受控
// 行保留原始行序，不重排、不过滤，也不使用单行对照。
// 远程 Server 与本地期望文本共用同一个 ProcessEngine（注入构造）。
//
// RED（T026 阶段）：T028/T029/T030 未实现——Client 不发送 COMMAND，查询无输出；
// 用例在 5 秒上限内以断言失败结束，不挂起。
// ---------------------------------------------------------------------------

/// `--all-processes`：Client 标准输出中的整份多行结果与同一引擎的
/// processListText() 逐字一致（统一 CRLF/LF 后整块比较文本、行序与字段）。
TEST(RemoteSessionQuerySuccess, AllProcessesMatchesLocalVerbatim)
{
    SnapshotSessionResult result;
    runSnapshotQuerySession(kQueryAllProcessesPort, "--all-processes\n", result);

    // 夹具自检：固定快照按注入顺序渲染成固定多行文本（≥3 个受控进程）。
    ASSERT_EQ(result.localListText, std::string(kSnapshotListText))
        << "fixture sanity: the injected snapshot must render in its fixed order";
    // 注入顺序非 PID 单调：若实现按 PID 排序，整块比较必须失败（本自检固定该前提）。
    const std::string pidSortedText =
        "PID: 4321 ProcessName: alpha.exe\n"
        "PID: 4322 ProcessName: 中文进程.exe\n"
        "PID: 4323 ProcessName: gamma service.exe\n";
    ASSERT_NE(result.localListText, pidSortedText)
        << "fixture sanity: snapshot order must not coincide with PID order";
    ASSERT_EQ(countOccurrences(result.localListText, "\n"), std::size_t{3})
        << "fixture sanity: the local result must be a three-line block";
    ASSERT_NE(result.localListText.find(std::string(kSnapshotUtf8Name) + "\n"),
              std::string::npos)
        << "fixture sanity: the fixed snapshot must carry a valid non-ASCII name";

    EXPECT_EQ(result.exitCode, 0)
        << "a successful query must not end the session with an error; client stderr=["
        << result.err << "]";

    // 整块逐字比较：不重排、不过滤、不用单行对照。
    expectVerbatimQueryBlock(result, result.localListText);
}

/// `--search-process <PID>`：成功结果与本地 `searchProcess()` 的既有输出格式一致
/// （单行进程名 + 行尾换行）。
TEST(RemoteSessionQuerySuccess, SearchProcessMatchesLocalFormat)
{
    SnapshotSessionResult result;
    runSnapshotQuerySession(kQuerySearchProcessPort,
                            "--search-process " + std::to_string(kSnapshotAsciiPid) + "\n",
                            result, kSnapshotAsciiPid);

    EXPECT_EQ(result.localSearchName, std::string(kSnapshotAsciiName))
        << "fixture sanity: searchProcess() must return the fixed snapshot name";
    EXPECT_EQ(result.exitCode, 0)
        << "a successful query must not end the session with an error; client stderr=["
        << result.err << "]";

    // 本地既有格式：`std::cout << *name << std::endl` → 名称单独一行。
    expectVerbatimQueryBlock(result, result.localSearchName + "\n");
}

/// 有效非 ASCII（UTF-8）进程名：远程显示与本地 searchProcess() 逐字一致，
/// 名称独立成行（FR-017：不合并行、不引入额外空行）。
TEST(RemoteSessionQuerySuccess, NonAsciiProcessNameIsDisplayedVerbatim)
{
    SnapshotSessionResult result;
    runSnapshotQuerySession(kQueryUtf8NamePort,
                            "--search-process " + std::to_string(kSnapshotUtf8Pid) + "\n",
                            result, kSnapshotUtf8Pid);

    EXPECT_EQ(result.localSearchName, std::string(kSnapshotUtf8Name))
        << "fixture sanity: the injected snapshot must carry a valid UTF-8 name";
    EXPECT_EQ(result.exitCode, 0)
        << "a successful query must not end the session with an error; client stderr=["
        << result.err << "]";

    expectVerbatimQueryBlock(result, result.localSearchName + "\n");
}

// ---------------------------------------------------------------------------
// 套件 RemoteSessionQueryFailure — US2 失败查询（T027）
//
// 每个用例先执行失败查询，再执行 help；断言失败文本出现且会话仍可用。
// 固定快照通过 ConfigurablePlatformOS 注入，Server 与 Client 走真实回环 TCP。
// RED（T027 阶段）：T028/T029/T030 未实现——Client 不发送 COMMAND，失败文本不会
// 出现；用例在 5 秒上限内以断言失败结束，不挂起。
// ---------------------------------------------------------------------------

/// `--search-process <不存在的 PID>` → `Process with PID <n> not found.`（cli.md §6）。
TEST(RemoteSessionQueryFailure, SearchProcessPidNotFoundKeepsPrompt)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    os->addProcess(4321, "alpha.exe");

    constexpr tpe::platform::Pid_t kMissingPid = 999999;
    QueryFailureSessionResult result;
    runQueryFailureSession(kQueryFailurePidNotFoundPort, std::move(os),
                           "--search-process " + std::to_string(kMissingPid) + "\nhelp\n",
                           result);

    expectFailureThenHelp(result,
                          "Process with PID " + std::to_string(kMissingPid) + " not found.");
}

/// 进程名不可读（空名称）→ `Failed to read the process name for PID <n>.`（cli.md §6）。
TEST(RemoteSessionQueryFailure, SearchProcessUnreadableNameKeepsPrompt)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    os->addProcess(4324, std::string()); // 空名称 = 不可读

    QueryFailureSessionResult result;
    runQueryFailureSession(kQueryFailureUnreadableNamePort, std::move(os),
                           "--search-process 4324\nhelp\n", result);

    expectFailureThenHelp(result, "Failed to read the process name for PID 4324.");
}

/// 枚举失败 → `Failed to enumerate processes: <原因>`（cli.md §6；缺陷⑧透传）。
TEST(RemoteSessionQueryFailure, AllProcessesEnumerationFailureKeepsPrompt)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    os->setEnumerationFailure("simulated enumeration failure");

    QueryFailureSessionResult result;
    runQueryFailureSession(kQueryFailureEnumerationPort, std::move(os),
                           "--all-processes\nhelp\n", result);

    expectFailureThenHelp(result,
                          "Failed to enumerate processes: simulated enumeration failure");
}

/// 未知命令（`list` 属契约 §3 未交付命令）→ 固定未知命令文本（protocol §5）。
TEST(RemoteSessionQueryFailure, UnknownCommandKeepsPrompt)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    os->addProcess(4321, "alpha.exe");

    QueryFailureSessionResult result;
    runQueryFailureSession(kQueryFailureUnknownCommandPort, std::move(os),
                           "list\nhelp\n", result);

    expectFailureThenHelp(result, kUnknownCommandText);
}

/// 查询结果超过单条 RESPONSE 上限 → `Response exceeds the maximum message size.`，
/// 不截断、不分片（cli.md §6；protocol §5）。
TEST(RemoteSessionQueryFailure, OversizeResultKeepsPrompt)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    std::size_t outputBytes = 0;
    for (int index = 0; index < kOversizeProcessCount; ++index) {
        const tpe::platform::Pid_t pid =
            static_cast<tpe::platform::Pid_t>(20000 + index);
        const std::string name = oversizeProcessName(index);
        os->addProcess(pid, name);
        // 与 processListText() 的行格式一致：`PID: <pid> ProcessName: <name>\n`。
        outputBytes += std::string("PID: ").size() + std::to_string(pid).size() +
                       std::string(" ProcessName: ").size() + name.size() + 1;
    }
    // RESPONSE payload = 1(status) + 2(promptLen) + 4("TPE>") + 4(outputLen) + output。
    ASSERT_GT(outputBytes + 11, static_cast<std::size_t>(tpe::remote::kMaxPayloadBytes))
        << "fixture sanity: the process list must exceed one RESPONSE payload";

    QueryFailureSessionResult result;
    runQueryFailureSession(kQueryFailureOversizePort, std::move(os),
                           "--all-processes\nhelp\n", result);

    expectFailureThenHelp(result, kOversizeResultText);
}

/// 进程名含非法 UTF-8 字节 → `Process name cannot be encoded as UTF-8.`（cli.md §6）。
TEST(RemoteSessionQueryFailure, UnencodableProcessNameKeepsPrompt)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    os->addProcess(4325, invalidUtf8ProcessName());

    QueryFailureSessionResult result;
    runQueryFailureSession(kQueryFailureUnencodablePort, std::move(os),
                           "--search-process 4325\nhelp\n", result);

    expectFailureThenHelp(result, kUnencodableNameText);
}

/// 枚举结果中任一名称含非法 UTF-8 字节 →
/// `Process list contains a name that cannot be encoded as UTF-8.`（cli.md §6）。
TEST(RemoteSessionQueryFailure, UnencodableProcessListKeepsPrompt)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    os->addProcess(4321, "alpha.exe");
    os->addProcess(4325, invalidUtf8ProcessName());

    QueryFailureSessionResult result;
    runQueryFailureSession(kQueryFailureEnumerationUnencodablePort, std::move(os),
                           "--all-processes\nhelp\n", result);

    expectFailureThenHelp(result, kUnencodableListText);
}

/// 同一结果同时不可编码且超限 → 先报告不可编码（cli.md §6 末段；
/// protocol §5“先校验文本编码并报告不可编码，编码成功后才检查长度”）。
TEST(RemoteSessionQueryFailure, UnencodableTakesPriorityOverOversize)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    std::string hugeInvalidName(
        static_cast<std::size_t>(tpe::remote::kMaxPayloadBytes) + 64, 'x');
    hugeInvalidName[1000] = static_cast<char>(0xFF);
    os->addProcess(4326, hugeInvalidName);

    QueryFailureSessionResult result;
    runQueryFailureSession(kQueryFailureDoubleFailurePort, std::move(os),
                           "--search-process 4326\nhelp\n", result);

    expectFailureThenHelp(result, kUnencodableNameText);

    const std::string clientOut = normalizedLineEndings(result.out);
    EXPECT_EQ(clientOut.find(kOversizeResultText), std::string::npos)
        << "cli.md §6: a result that is both unencodable and oversized must report the "
           "encoding failure first; stdout=["
        << clientOut << "]";
}

// ---------------------------------------------------------------------------
// 套件 RemoteSessionPreSendValidation — US2 发送前校验（T027）
//
// 三条本地拒绝都必须写标准输出、保留 TPE>、不发送命令；「未发送」由脚本化 Server
// 断言：握手后收到的第一条（也是唯一一条）COMMAND 必须是随后输入的 help；
// 不使用 T041 尚未实现的查询日志。Client 退出码留到 US3 验证。
// ---------------------------------------------------------------------------

/// 65,537 字节命令（> 64 KiB）→ `Command exceeds the maximum message size.`（§3.1）。
TEST(RemoteSessionPreSendValidation, OversizeCommandIsRejectedLocally)
{
    const std::string rejectedLine(tpe::remote::kMaxCommandBytes + 1, 'a');

    PreSendSessionResult result;
    runPreSendRejectionSession(rejectedLine + "\n", result);

    expectLocalRejectionThenHelp(result, kOversizeCommandText);
}

/// 含非法 UTF-8 字节的命令 → `Command cannot be encoded as UTF-8.`（§3.1 第 1 条）。
TEST(RemoteSessionPreSendValidation, InvalidUtf8CommandIsRejectedLocally)
{
    std::string rejectedLine = "--all-processes";
    rejectedLine.push_back(static_cast<char>(0xFF));

    PreSendSessionResult result;
    runPreSendRejectionSession(rejectedLine + "\n", result);

    expectLocalRejectionThenHelp(result, kUnencodableCommandText);
}

/// 去除行尾换行后仍含裸 CR → `Command must not contain line breaks.`（§3.1 第 2 条）。
TEST(RemoteSessionPreSendValidation, EmbeddedCarriageReturnIsRejectedLocally)
{
    PreSendSessionResult result;
    runPreSendRejectionSession("--all-processes\rlist\n", result);

    expectLocalRejectionThenHelp(result, kLineBreakCommandText);
}

/// 超长未知命令（`list` 前缀）只走本地长度拒绝，不发送、不产生未知命令响应（§3.1）。
TEST(RemoteSessionPreSendValidation, OversizeUnknownCommandOnlyHitsLocalLengthCheck)
{
    std::string rejectedLine = "list ";
    rejectedLine.append(tpe::remote::kMaxCommandBytes + 1 - rejectedLine.size(), 'u');
    ASSERT_EQ(rejectedLine.size(), tpe::remote::kMaxCommandBytes + 1);

    PreSendSessionResult result;
    runPreSendRejectionSession(rejectedLine + "\n", result);

    // 若该命令被发送，Server 会回 unknown 文本；脚本化 Server 要求首条 COMMAND 为
    // help，配合 expectLocalRejectionThenHelp 的“无未知命令响应”断言共同证明未发送。
    expectLocalRejectionThenHelp(result, kOversizeCommandText);
}

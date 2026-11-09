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
 *   - T032 增加 US3 单连接用例：Active/长查询期间第二连接收到 BUSY、首会话不受
 *     影响；清理完成后被处理的连接被接纳；首字节/完整帧的 A/B/C 时点边界。
 *     长查询由 BlockingPlatformOS 注入（目标进程名称读取在 release() 前阻塞）。
 *   - T033 增加 Draining 与 FR-012 命令处置用例：用 Draining 清理延迟接缝把
 *     “会话已结束、清理未完成”的窗口拉长到可观测，窗口内被处理的连接收到 BUSY、
 *     清理完成后被接纳；同一读取事件中缓冲的完整 COMMAND 不得在首条 RESPONSE 后
 *     被续行执行；未开始命令（缓冲完整帧/不完整帧）在断开后不执行；已开始命令
 *     自然结束且写回失败被忽略。
 *   - T034 增加退出/BYE/断线用例：exit/EOF 经 BYE 确认退出 0；确认前断线与确认超时
 *     退出 1；等待响应/输入时断线的报告时机；活动期 ERROR 原因；非空 BYE 双向处理；
 *     断线与 EOF 竞态；以及 RawSocket 活动会话发送非空 BYE 的 Server 侧应答。
 *   - T036 评审修复（I1/I2/I3/I5/M1/M4/M5/M10）：BYE 确认期限增加 1.5 s 上界；
 *     断线优先于 EOF 由 Server 半关闭后检测任何后续数据证明（收到即失败）；失败查询
 *     与本地拒绝会话断言退出 0；非空 BYE 路径断言恰好 1 条 disconnect/error 且无
 *     disconnect/disconnected；BUSY 反应与断线报告上界收紧；等待输入断线断言固定
 *     文本；活动期 ERROR 后 Client 不得再发送 COMMAND。
 *
 * 约束：仅 GoogleTest + 标准库 + 平台 socket API；不启动子进程；除 127.0.0.1
 * 回环外不访问网络。
 *
 * 当前阶段（US3）：T022/T023 握手、T028–T030 查询、T035 Draining、T037 断线
 * 唤醒均已实现；失败查询与本地拒绝会话经 EOF → BYE 确认正常结束，退出码由断言
 * 校验为 0（T036 评审修复 I3）。
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

/// T032 单连接用例的独立端口（18099–18101，与既有用例互不重叠）。
constexpr std::uint16_t kActiveBusyPort = 18099;
constexpr std::uint16_t kLongQueryPort = 18100;
constexpr std::uint16_t kCleanupAcceptPort = 18101;

/// T033 清理与命令处置用例的独立端口（18102–18105，与既有用例互不重叠）。
constexpr std::uint16_t kDrainingBusyPort = 18102;
constexpr std::uint16_t kBufferedCommandPort = 18103;
constexpr std::uint16_t kIncompleteCommandPort = 18104;
constexpr std::uint16_t kStartedCommandPort = 18105;

/// T034 RawSocket 活动会话发送非空 BYE 的独立端口（18106）。
constexpr std::uint16_t kRawNonEmptyByePort = 18106;

/// T027 失败查询用例的独立端口（18090–18098，与既有用例互不重叠）。
constexpr std::uint16_t kQueryFailurePidNotFoundPort = 18090;
constexpr std::uint16_t kQueryFailureUnreadableNamePort = 18091;
constexpr std::uint16_t kQueryFailureEnumerationPort = 18092;
constexpr std::uint16_t kQueryFailureUnknownCommandPort = 18093;
constexpr std::uint16_t kQueryFailureOversizePort = 18094;
constexpr std::uint16_t kQueryFailureUnencodablePort = 18095;
constexpr std::uint16_t kQueryFailureEnumerationUnencodablePort = 18096;
constexpr std::uint16_t kQueryFailureDoubleFailurePort = 18097;
constexpr std::uint16_t kQueryFailureListDoubleFailurePort = 18098;

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

    /// T032：追加输入但不置 EOF；文本消费完后继续等待下一条输入。
    /// 用于同一会话内先发送查询、待响应后再发送 help/exit 的场景。
    void pushInput(std::string text)
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_text += text;
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

/// brief 第 4 条：仅在 start() 失败后调用。返回 true = 端口确实不可用，调用方必须在
/// 用例体（void 测试函数）中 GTEST_SKIP()；返回 false = 端口空闲或无法判定，
/// 调用方应以真实错误 FAIL。GTEST_SKIP() 只返回到当前函数：放在任何辅助函数里都只会
/// 结束辅助函数本身，调用方仍会继续断言（本文件的会话辅助改用 SessionRunReport 规避）。
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

/// 半关闭发送方向：对端立即看到断线（FIN），本端仍可读取对端后续数据。
/// T036 评审修复 I2 用它区分“断线优先”与“EOF 正常结束后的 BYE”。
bool shutdownSendSide(SocketHandle handle)
{
#ifdef _WIN32
    return ::shutdown(handle, SD_SEND) == 0;
#else
    return ::shutdown(handle, SHUT_WR) == 0;
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
// T032 可控长查询平台
//
// 只扩展测试夹具（不改生产接口）：目标进程的 getProcessName() 在 release() 前阻塞，
// 从而让一条 `--all-processes`/`--search-process` 查询持续执行；名称读取次数即
// 命令进入执行函数的次数，用于断言“未开始命令丢弃 / 已开始命令自然结束”。
// ---------------------------------------------------------------------------

class BlockingPlatformOS;

/// 目标进程：名称读取先登记并阻塞，release() 后恢复为正常名称。
class BlockingProcess : public SnapshotProcess {
public:
    BlockingProcess(tpe::platform::Pid_t pid, std::string name, BlockingPlatformOS& owner)
        : SnapshotProcess(pid, std::move(name))
        , m_owner(owner)
    {
    }

    std::string getProcessName() const override;

private:
    BlockingPlatformOS& m_owner;
};

/// 可控长查询平台：一个目标进程；名称读取可阻塞并可计数。
class BlockingPlatformOS : public tpe::platform::PlatformOS {
public:
    BlockingPlatformOS()
    {
        ProcessList.push_back(std::make_shared<BlockingProcess>(4321, "blocking.exe", *this));
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
        for (const std::shared_ptr<tpe::platform::PlatformProcess>& process : ProcessList) {
            pids.push_back(process->getPid());
        }
        return tpe::Result<std::vector<tpe::platform::Pid_t>, tpe::PlatformError>::success(
            std::move(pids));
    }

    void getAllProcesses(std::vector<tpe::platform::Pid_t>) override {}

    /// 名称读取次数（= 命令进入执行函数的次数）。
    int nameReadCount() const { return m_nameReads.load(std::memory_order_acquire); }

    bool waitForNameReads(int count, std::chrono::milliseconds timeout) const
    {
        return waitUntil([this, count] { return nameReadCount() >= count; }, timeout);
    }

    /// 当前是否有命令正在执行（名称读取尚未返回）。
    bool isBlocked() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_blocked;
    }

    bool waitForBlocked(std::chrono::milliseconds timeout) const
    {
        return waitUntil([this] { return isBlocked(); }, timeout);
    }

    /// 放行所有（包括后续）名称读取；可重复调用。
    void release()
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
        }
        m_condition.notify_all();
    }

    /// 夹具内部：登记一次名称读取并按需阻塞（带上限，绝不永久挂起）。
    void enterNameRead() const
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_nameReads.fetch_add(1, std::memory_order_acq_rel);
        m_blocked = true;
        m_condition.notify_all();
        m_condition.wait_for(lock, std::chrono::seconds{30}, [this] { return m_released; });
        m_blocked = false;
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_condition;
    mutable std::atomic<int> m_nameReads{0};
    mutable bool m_blocked = false;
    bool m_released = false;
};

std::string BlockingProcess::getProcessName() const
{
    m_owner.enterNameRead();
    return SnapshotProcess::getProcessName();
}

/// RAII：用例提前返回时也必须放行长查询，避免 Server::stop() 等待阻塞调用。
class ScopedPlatformRelease {
public:
    explicit ScopedPlatformRelease(BlockingPlatformOS& platform)
        : m_platform(platform)
    {
    }

    ~ScopedPlatformRelease() { m_platform.release(); }

    ScopedPlatformRelease(const ScopedPlatformRelease&) = delete;
    ScopedPlatformRelease& operator=(const ScopedPlatformRelease&) = delete;

private:
    BlockingPlatformOS& m_platform;
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

/// 会话辅助函数的运行结果（不是测试断言结果）。GTEST_SKIP()/FAIL() 只有在用例体
/// （void 测试函数）中才产生测试级结果：放在 void 辅助函数里只会返回辅助函数本身，
/// 调用方仍会继续断言并把 SKIP 变成 FAIL。因此辅助函数只返回状态与原因，
/// 由用例体决定调用 GTEST_SKIP() 或 FAIL()。
enum class SessionRunStatus { Completed, PortUnavailable, StartFailed };

struct SessionRunReport {
    SessionRunStatus status = SessionRunStatus::StartFailed;
    std::string detail;
};

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
/// 返回 SessionRunReport：PortUnavailable 由用例体 GTEST_SKIP()，其余失败由用例体 FAIL()。
SessionRunReport runSnapshotQuerySession(
    std::uint16_t port, const std::string& commands, SnapshotSessionResult& result,
    std::optional<tpe::platform::Pid_t> searchPid = std::nullopt)
{
    ScopedRemoteStreams streams;
    LocalServer server(port, std::make_unique<SnapshotPlatformOS>());

    // 先启动；仅当启动失败时才诊断端口（与既有用例同一策略）。
    if (!server.start()) {
        if (isPortUnavailable(port)) {
            return {SessionRunStatus::PortUnavailable,
                    "TCP port " + std::to_string(port) +
                        " is unavailable (bound by another process or reserved by the "
                        "system); skipping instead of misreporting a genuine port "
                        "conflict as a failure."};
        }
        return {SessionRunStatus::StartFailed,
                "RemoteServer::start() must succeed on free port " +
                    std::to_string(port)};
    }
    if (!waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit)) {
        server.stop();
        return {SessionRunStatus::StartFailed,
                "isRunning() must become true within 5 s after start() returned true"};
    }

    // 本地期望文本：与 Server 执行远程查询使用的是同一个引擎实例。
    const tpe::Result<std::string, tpe::PlatformError> localText =
        server.engine().processListText();
    if (!localText.has_value()) {
        server.stop();
        return {SessionRunStatus::StartFailed,
                "the fixed snapshot must enumerate successfully"};
    }
    result.localListText = localText.value();

    if (searchPid.has_value()) {
        const std::optional<std::string> name = server.engine().searchProcess(*searchPid);
        if (!name.has_value()) {
            server.stop();
            return {SessionRunStatus::StartFailed,
                    "fixture sanity: PID " + std::to_string(*searchPid) +
                        " must exist in the fixed snapshot"};
        }
        if (name->empty()) {
            server.stop();
            return {SessionRunStatus::StartFailed,
                    "fixture sanity: the snapshot name must not be empty"};
        }
        result.localSearchName = *name;
    }

    ClientRun client("127.0.0.1", port);
    client.start();

    if (!waitUntil([&client] { return client.out().find("TPE>") != std::string::npos; },
                   kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        client.waitDone(std::chrono::milliseconds{1000});
        return {SessionRunStatus::StartFailed,
                "client did not display 'TPE>' within 5 s; client stdout=[" +
                    client.out() + "] client stderr=[" + client.err() +
                    "] server stdout=[" + capturedServerOut() + "]"};
    }

    stdinBuffer().releaseWith(commands);

    if (!client.waitDone(kWaitLimit)) {
        server.stop();
        stdinBuffer().interrupt();
        client.waitDone(std::chrono::milliseconds{1000});
        return {SessionRunStatus::StartFailed,
                "client did not finish within 5 s after the query commands; client stdout=[" +
                    client.out() + "] client stderr=[" + client.err() + "]"};
    }
    if (!client.join()) {
        return {SessionRunStatus::StartFailed,
                "client thread did not finish after waitDone() returned true"};
    }

    result.exitCode = client.exitCode();
    result.out = client.out();
    result.err = client.err();

    server.stop();
    return {SessionRunStatus::Completed, {}};
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

/// 长查询用例的最长等待（单次有界；契约 30 秒语义由默认 ClientTimeouts 保持）。
constexpr std::chrono::milliseconds kLongWaitLimit{10000};

/// 长查询在第二连接进入等待后继续执行的目标时长（brief：>5 秒）。
constexpr std::chrono::milliseconds kLongQueryDuration{5500};

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
/// 返回 SessionRunReport：PortUnavailable 由用例体 GTEST_SKIP()，其余失败由用例体 FAIL()。
SessionRunReport runQueryFailureSession(
    std::uint16_t port, std::unique_ptr<tpe::platform::PlatformOS> os,
    const std::string& commands, QueryFailureSessionResult& result)
{
    ScopedRemoteStreams streams;
    LocalServer server(port, std::move(os));

    // 先启动；仅当启动失败时才诊断端口（与既有用例同一策略）。
    if (!server.start()) {
        if (isPortUnavailable(port)) {
            return {SessionRunStatus::PortUnavailable,
                    "TCP port " + std::to_string(port) +
                        " is unavailable (bound by another process or reserved by the "
                        "system); skipping instead of misreporting a genuine port "
                        "conflict as a failure."};
        }
        return {SessionRunStatus::StartFailed,
                "RemoteServer::start() must succeed on free port " +
                    std::to_string(port)};
    }
    if (!waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit)) {
        server.stop();
        return {SessionRunStatus::StartFailed,
                "isRunning() must become true within 5 s after start() returned true"};
    }

    ClientRun client("127.0.0.1", port);
    client.start();

    if (!waitUntil([&client] { return client.out().find("TPE>") != std::string::npos; },
                   kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        client.waitDone(std::chrono::milliseconds{1000});
        return {SessionRunStatus::StartFailed,
                "client did not display 'TPE>' within 5 s; client stdout=[" +
                    client.out() + "] client stderr=[" + client.err() +
                    "] server stdout=[" + capturedServerOut() + "]"};
    }

    stdinBuffer().releaseWith(commands);

    if (!client.waitDone(kWaitLimit)) {
        server.stop();
        stdinBuffer().interrupt();
        client.waitDone(std::chrono::milliseconds{1000});
        return {SessionRunStatus::StartFailed,
                "client did not finish within 5 s after the failing query; client stdout=[" +
                    client.out() + "] client stderr=[" + client.err() + "]"};
    }
    if (!client.join()) {
        return {SessionRunStatus::StartFailed,
                "client thread did not finish after waitDone() returned true"};
    }

    result.exitCode = client.exitCode();
    result.out = client.out();
    result.err = client.err();

    server.stop();
    return {SessionRunStatus::Completed, {}};
}

/// 断言：失败文本恰出现 1 次 → 回到 TPE> → help 的 4 行帮助逐字出现 1 次 → 再次出现
/// 提示符（会话仍可用）。cli.md §6：查询失败保留 TPE>；US3 已实现 EOF → BYE
/// 确认收尾，会话退出码必须为 0（T036 评审修复 I3）。
void expectFailureThenHelp(const QueryFailureSessionResult& result,
                           const std::string& expectedFailure)
{
    // 失败查询不改变会话终态：标准输入 EOF → BYE → 确认后以 0 结束。
    EXPECT_EQ(result.exitCode, 0)
        << "a failed query must not change the exit code; stderr=[" << result.err << "]";

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

/// 等待 Client 的会话结束帧：接受合法 `BYE`，也接受对端直接关闭（EOF/连接重置）。
/// US2 不实现 exit/EOF 的 BYE 收尾（US3 负责），EOF 后 Client 发 BYE 或直接关闭
/// 都属于可接受的结束方式；收到其它帧类型（如被误发的 COMMAND）仍判为失败。
bool receiveByeOrPeerClose(SocketHandle handle, std::chrono::milliseconds timeout,
                           std::string& failure)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<std::uint8_t> buffer;
    std::uint8_t chunk[512];
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= 0ms) {
            failure = "timed out waiting for BYE or peer close (" +
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
                if (decoded.frame.header.type != tpe::remote::MessageType::Bye) {
                    failure = "after the help response the client must end with BYE or "
                              "close the connection; got type " +
                              std::to_string(static_cast<int>(decoded.frame.header.type));
                    return false;
                }
                return true;
            }
            if (decoded.status == tpe::remote::DecodeStatus::Failed) {
                failure = "frame decode failed: " + decoded.error.reason;
                return false;
            }
            continue;
        }
        if (received == 0) {
            return true; // 对端关闭：US2 允许的直接结束方式
        }
        const int error = lastSocketError();
        if (isConnectionResetError(error)) {
            return true; // 对端重置同样视为结束
        }
        if (!isRecvTimeoutError(error)) {
            failure = "recv failed with socket error " + std::to_string(error);
            return false;
        }
    }
}

/// 脚本化 Server：完成握手后要求 Client 发出的第一条（也是唯一一条）COMMAND 是 help。
/// 若本地拒绝的命令被错误发送，脚本会看到不同的首帧并以明确原因失败；这同时证明
/// 被拒命令未发送、也不会产生任何 Server 响应（不依赖 T041 的查询日志）。
/// 随后回 help 的 RESPONSE，并以 BYE 或对端关闭接受会话结束（EOF 不得判失败）。
/// US3 已实现 EOF → BYE 收尾：会话退出码必须为 0（T036 评审修复 I3）。
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

        // 标准输入 EOF 后 Client 可能发送 BYE 并等待确认，也可能直接关闭连接；
        // US2 不实现 exit/EOF 收尾（US3 负责），两种结束方式都不得判为脚本失败。
        if (!receiveByeOrPeerClose(socket, kWaitLimit, failure)) {
            return false;
        }
        // 尽力回 BYE 确认；Client 已关闭时发送失败不构成脚本失败。
        sendFrameBytes(socket, tpe::remote::MessageType::Bye, {});
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
    // 本地拒绝不改变会话终态：EOF → BYE 确认后以 0 结束（T036 评审修复 I3）。
    EXPECT_EQ(result.exitCode, 0)
        << "a locally rejected command must not change the exit code; stderr=["
        << result.err << "]";

    EXPECT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    result.scriptSucceeded = server.scriptSucceeded();
    result.scriptFailure = server.scriptFailure();
}

/// 断言：本地拒绝文本恰出现 1 次且写标准输出 → 不出现未知命令响应 → 回到 TPE> →
/// help 的 4 行帮助逐字出现 1 次 → 再次出现提示符。
/// 「未发送」由 runPreSendRejectionSession 的脚本化 Server 单独证明：
/// 握手后第一条 COMMAND 必须是 help。会话退出码为 0 的断言在
/// runPreSendRejectionSession 内完成（T036 评审修复 I3）。
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
    const SessionRunReport report =
        runSnapshotQuerySession(kQueryAllProcessesPort, "--all-processes\n", result);
    if (report.status == SessionRunStatus::PortUnavailable) {
        GTEST_SKIP() << report.detail;
    }
    ASSERT_TRUE(report.status == SessionRunStatus::Completed) << report.detail;

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
    const SessionRunReport report = runSnapshotQuerySession(
        kQuerySearchProcessPort,
        "--search-process " + std::to_string(kSnapshotAsciiPid) + "\n", result,
        kSnapshotAsciiPid);
    if (report.status == SessionRunStatus::PortUnavailable) {
        GTEST_SKIP() << report.detail;
    }
    ASSERT_TRUE(report.status == SessionRunStatus::Completed) << report.detail;

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
    const SessionRunReport report = runSnapshotQuerySession(
        kQueryUtf8NamePort, "--search-process " + std::to_string(kSnapshotUtf8Pid) + "\n",
        result, kSnapshotUtf8Pid);
    if (report.status == SessionRunStatus::PortUnavailable) {
        GTEST_SKIP() << report.detail;
    }
    ASSERT_TRUE(report.status == SessionRunStatus::Completed) << report.detail;

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
    const SessionRunReport report = runQueryFailureSession(
        kQueryFailurePidNotFoundPort, std::move(os),
        "--search-process " + std::to_string(kMissingPid) + "\nhelp\n", result);
    if (report.status == SessionRunStatus::PortUnavailable) {
        GTEST_SKIP() << report.detail;
    }
    ASSERT_TRUE(report.status == SessionRunStatus::Completed) << report.detail;

    expectFailureThenHelp(result,
                          "Process with PID " + std::to_string(kMissingPid) + " not found.");
}

/// 进程名不可读（空名称）→ `Failed to read the process name for PID <n>.`（cli.md §6）。
TEST(RemoteSessionQueryFailure, SearchProcessUnreadableNameKeepsPrompt)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    os->addProcess(4324, std::string()); // 空名称 = 不可读

    QueryFailureSessionResult result;
    const SessionRunReport report = runQueryFailureSession(
        kQueryFailureUnreadableNamePort, std::move(os), "--search-process 4324\nhelp\n",
        result);
    if (report.status == SessionRunStatus::PortUnavailable) {
        GTEST_SKIP() << report.detail;
    }
    ASSERT_TRUE(report.status == SessionRunStatus::Completed) << report.detail;

    expectFailureThenHelp(result, "Failed to read the process name for PID 4324.");
}

/// 枚举失败 → `Failed to enumerate processes: <原因>`（cli.md §6；缺陷⑧透传）。
TEST(RemoteSessionQueryFailure, AllProcessesEnumerationFailureKeepsPrompt)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    os->setEnumerationFailure("simulated enumeration failure");

    QueryFailureSessionResult result;
    const SessionRunReport report = runQueryFailureSession(
        kQueryFailureEnumerationPort, std::move(os), "--all-processes\nhelp\n", result);
    if (report.status == SessionRunStatus::PortUnavailable) {
        GTEST_SKIP() << report.detail;
    }
    ASSERT_TRUE(report.status == SessionRunStatus::Completed) << report.detail;

    expectFailureThenHelp(result,
                          "Failed to enumerate processes: simulated enumeration failure");
}

/// 未知命令（`list` 属契约 §3 未交付命令）→ 固定未知命令文本（protocol §5）。
TEST(RemoteSessionQueryFailure, UnknownCommandKeepsPrompt)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    os->addProcess(4321, "alpha.exe");

    QueryFailureSessionResult result;
    const SessionRunReport report = runQueryFailureSession(
        kQueryFailureUnknownCommandPort, std::move(os), "list\nhelp\n", result);
    if (report.status == SessionRunStatus::PortUnavailable) {
        GTEST_SKIP() << report.detail;
    }
    ASSERT_TRUE(report.status == SessionRunStatus::Completed) << report.detail;

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
    const SessionRunReport report = runQueryFailureSession(
        kQueryFailureOversizePort, std::move(os), "--all-processes\nhelp\n", result);
    if (report.status == SessionRunStatus::PortUnavailable) {
        GTEST_SKIP() << report.detail;
    }
    ASSERT_TRUE(report.status == SessionRunStatus::Completed) << report.detail;

    expectFailureThenHelp(result, kOversizeResultText);
}

/// 进程名含非法 UTF-8 字节 → `Process name cannot be encoded as UTF-8.`（cli.md §6）。
TEST(RemoteSessionQueryFailure, UnencodableProcessNameKeepsPrompt)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    os->addProcess(4325, invalidUtf8ProcessName());

    QueryFailureSessionResult result;
    const SessionRunReport report = runQueryFailureSession(
        kQueryFailureUnencodablePort, std::move(os), "--search-process 4325\nhelp\n",
        result);
    if (report.status == SessionRunStatus::PortUnavailable) {
        GTEST_SKIP() << report.detail;
    }
    ASSERT_TRUE(report.status == SessionRunStatus::Completed) << report.detail;

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
    const SessionRunReport report = runQueryFailureSession(
        kQueryFailureEnumerationUnencodablePort, std::move(os), "--all-processes\nhelp\n",
        result);
    if (report.status == SessionRunStatus::PortUnavailable) {
        GTEST_SKIP() << report.detail;
    }
    ASSERT_TRUE(report.status == SessionRunStatus::Completed) << report.detail;

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
    const SessionRunReport report = runQueryFailureSession(
        kQueryFailureDoubleFailurePort, std::move(os), "--search-process 4326\nhelp\n",
        result);
    if (report.status == SessionRunStatus::PortUnavailable) {
        GTEST_SKIP() << report.detail;
    }
    ASSERT_TRUE(report.status == SessionRunStatus::Completed) << report.detail;

    expectFailureThenHelp(result, kUnencodableNameText);

    const std::string clientOut = normalizedLineEndings(result.out);
    EXPECT_EQ(clientOut.find(kOversizeResultText), std::string::npos)
        << "cli.md §6: a result that is both unencodable and oversized must report the "
           "encoding failure first; stdout=["
        << clientOut << "]";
}

/// 同一 `--all-processes` 列表同时不可编码且超限 → 先报告列表不可编码
/// （cli.md §6 末段；protocol §5“先校验文本编码并报告不可编码，编码成功后才检查长度”），
/// 且不得报告超限（与单名称双失败用例并列覆盖列表路径）。
TEST(RemoteSessionQueryFailure, UnencodableListTakesPriorityOverOversize)
{
    auto os = std::make_unique<ConfigurablePlatformOS>();
    std::size_t outputBytes = 0;
    for (int index = 0; index < kOversizeProcessCount; ++index) {
        const tpe::platform::Pid_t pid =
            static_cast<tpe::platform::Pid_t>(20000 + index);
        const std::string name = oversizeProcessName(index);
        os->addProcess(pid, name);
        outputBytes += std::string("PID: ").size() + std::to_string(pid).size() +
                       std::string(" ProcessName: ").size() + name.size() + 1;
    }
    const std::string invalidName = invalidUtf8ProcessName();
    os->addProcess(4326, invalidName);
    outputBytes += std::string("PID: ").size() + std::string("4326").size() +
                   std::string(" ProcessName: ").size() + invalidName.size() + 1;
    // RESPONSE payload = 1(status) + 2(promptLen) + 4("TPE>") + 4(outputLen) + output。
    ASSERT_GT(outputBytes + 11, static_cast<std::size_t>(tpe::remote::kMaxPayloadBytes))
        << "fixture sanity: the process list must exceed one RESPONSE payload";

    QueryFailureSessionResult result;
    const SessionRunReport report = runQueryFailureSession(
        kQueryFailureListDoubleFailurePort, std::move(os), "--all-processes\nhelp\n",
        result);
    if (report.status == SessionRunStatus::PortUnavailable) {
        GTEST_SKIP() << report.detail;
    }
    ASSERT_TRUE(report.status == SessionRunStatus::Completed) << report.detail;

    expectFailureThenHelp(result, kUnencodableListText);

    const std::string clientOut = normalizedLineEndings(result.out);
    EXPECT_EQ(clientOut.find(kOversizeResultText), std::string::npos)
        << "cli.md §6: a list that is both unencodable and oversized must report the "
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

// ---------------------------------------------------------------------------
// T032 单连接辅助
// ---------------------------------------------------------------------------

/// 以原始 socket 完成 HELLO/WELCOME，得到一个活动会话。
bool openRawSession(RawSocket& socket, std::uint16_t port, std::string& failure)
{
    if (!socket.connectTo(port, failure)) {
        return false;
    }
    if (!socket.sendFrame(tpe::remote::MessageType::Hello, toBytes("TPE"))) {
        failure = "failed to send HELLO";
        return false;
    }
    tpe::remote::Frame reply;
    if (!socket.receiveFrame(kWaitLimit, reply, failure)) {
        return false;
    }
    if (reply.header.type != tpe::remote::MessageType::Welcome) {
        failure = "expected WELCOME while opening a raw session, got type " +
                  std::to_string(static_cast<int>(reply.header.type));
        return false;
    }
    if (!reply.payload.empty()) {
        failure = "WELCOME payload must be empty";
        return false;
    }
    return true;
}

/// 在清理完成后启动一个可用的 Client（有界重试；每次被 BUSY 拒绝都继续等待）。
/// 返回 nullptr 时 failure 记录最后一次原因；调用方负责最终断言。
struct AcceptedClientStart {
    std::unique_ptr<ClientRun> client;
    std::string failure;
};

AcceptedClientStart connectClientAfterCleanup(std::uint16_t port,
                                              std::chrono::milliseconds budget)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::string lastFailure = "no connection attempted";
    while (std::chrono::steady_clock::now() < deadline) {
        auto candidate = std::make_unique<ClientRun>("127.0.0.1", port);
        candidate->start();
        if (waitUntil(
                [&candidate] {
                    return candidate->out().find("TPE>") != std::string::npos;
                },
                std::chrono::milliseconds{2000})) {
            return {std::move(candidate), std::string{}};
        }
        if (!candidate->waitDone(std::chrono::milliseconds{2000})) {
            return {nullptr,
                    "candidate client neither reached 'TPE>' nor exited within 2 s"};
        }
        candidate->join();
        lastFailure = "candidate exited with " + std::to_string(candidate->exitCode()) +
                      "; stderr=[" + candidate->err() + "]";
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    return {nullptr, "server did not accept a client before the budget expired; last: " +
                         lastFailure};
}

// ---------------------------------------------------------------------------
// 套件 RemoteSessionSingleConnection — US3 单连接（T032）
//
// 占用判定以连接事件被处理时的状态为准（protocol §3）：Active/长查询期间的
// 第二连接收到 BUSY 并退出 1，首会话不受影响；清理完成后被处理的连接被接纳。
// 长查询由 BlockingPlatformOS 注入（目标进程名称读取阻塞）；所有等待有界。
// ---------------------------------------------------------------------------

/// Active 期间第二连接被处理时立即收到 BUSY；首会话保持可用（help 正常）。
/// 该用例复用 US1 已实现的行为，作为回归基线记录，不为制造 RED 改实现。
TEST(RemoteSessionSingleConnection, ActiveSessionSecondClientGetsBusyImmediately)
{
    ScopedRemoteStreams streams;
    LocalServer server(kActiveBusyPort);
    if (!server.start()) {
        if (isPortUnavailable(kActiveBusyPort)) {
            GTEST_SKIP() << "TCP port " << kActiveBusyPort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kActiveBusyPort;
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    ClientRun first("127.0.0.1", kActiveBusyPort);
    first.start();
    if (!waitUntil([&first] { return first.out().find("TPE>") != std::string::npos; },
                   kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        first.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "first client did not display 'TPE>' within 5 s; stdout=[" << first.out()
               << "] stderr=[" << first.err() << "]";
    }

    const auto connectedAt = std::chrono::steady_clock::now();
    ClientRun second("127.0.0.1", kActiveBusyPort);
    second.start();
    if (!second.waitDone(kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        second.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "second client did not return within 5 s; stdout=[" << second.out()
               << "] stderr=[" << second.err() << "]";
    }
    ASSERT_TRUE(second.join());
    const auto reactedAfter = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - connectedAt);

    EXPECT_EQ(second.exitCode(), 1) << "BUSY must end the second client with exit code 1";
    EXPECT_NE(second.err().find("Connection failed."), std::string::npos)
        << "second client stderr=[" << second.err() << "]";
    EXPECT_NE(second.err().find("Server is busy."), std::string::npos)
        << "second client stderr=[" << second.err() << "]";
    // T036 评审修复 M1：BUSY 必须在处理连接事件时立即写回；1 s 上界（远高于
    // 实测数十毫秒）仍足以在实现退化为延迟拒绝时失败。
    EXPECT_LT(reactedAfter, std::chrono::milliseconds{1000})
        << "the server must send BUSY when it processes the connection event (not after an "
           "idle delay); reaction took "
        << reactedAfter.count() << " ms";

    // 首会话不受影响：查询与 help 仍可用。
    stdinBuffer().pushInput("help\n");
    if (!waitUntil(
            [&first] { return first.out().find(kHelpText) != std::string::npos; }, kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        first.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "first client must remain usable after the BUSY rejection; stdout=["
               << first.out() << "] stderr=[" << first.err() << "]";
    }

    stdinBuffer().pushInput("exit\n");
    ASSERT_TRUE(first.waitDone(kWaitLimit)) << "first client did not exit; stdout=["
                                            << first.out() << "]";
    ASSERT_TRUE(first.join());
    EXPECT_EQ(first.exitCode(), 0);

    server.stop();
}

/// 长查询（>5 秒）期间第二连接到达：首会话保持占用直到该连接事件被处理；
/// 第二 Client 收到 BUSY 并退出 1；首查询完成后首 Client 仍可用（help）。
TEST(RemoteSessionSingleConnection, LongRunningQueryKeepsSessionBusyAndFirstClientUsable)
{
    ScopedRemoteStreams streams;
    auto os = std::make_unique<BlockingPlatformOS>();
    BlockingPlatformOS* const platform = os.get();
    LocalServer server(kLongQueryPort, std::move(os));
    ScopedPlatformRelease releaseOnExit(*platform);

    if (!server.start()) {
        if (isPortUnavailable(kLongQueryPort)) {
            GTEST_SKIP() << "TCP port " << kLongQueryPort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kLongQueryPort;
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    ClientRun first("127.0.0.1", kLongQueryPort);
    first.start();
    if (!waitUntil([&first] { return first.out().find("TPE>") != std::string::npos; },
                   kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        first.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "first client did not display 'TPE>' within 5 s; stdout=[" << first.out()
               << "] stderr=[" << first.err() << "]";
    }

    stdinBuffer().pushInput("--all-processes\n");
    if (!platform->waitForNameReads(1, kLongWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        first.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "the long query did not start within 10 s; client stdout=[" << first.out()
               << "] client stderr=[" << first.err() << "]";
    }

    ClientRun second("127.0.0.1", kLongQueryPort);
    second.start();
    std::this_thread::sleep_for(kLongQueryDuration);
    EXPECT_TRUE(platform->isBlocked())
        << "the query must keep executing more than 5 s after the second connection arrived";

    platform->release();

    if (!second.waitDone(kLongWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        second.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "second client did not return within 10 s after the long query finished; "
                  "stdout=["
               << second.out() << "] stderr=[" << second.err() << "]";
    }
    ASSERT_TRUE(second.join());
    EXPECT_EQ(second.exitCode(), 1)
        << "BUSY (sent when the queued connection event is processed) must exit 1; stderr=["
        << second.err() << "]";
    EXPECT_NE(second.err().find("Server is busy."), std::string::npos)
        << "second client stderr=[" << second.err() << "]";

    // 首会话未被影响：查询结果正常渲染并回到 TPE>。
    if (!waitUntil(
            [&first] {
                const std::string out = normalizedLineEndings(first.out());
                return out.find("blocking.exe") != std::string::npos &&
                       out.find("TPE>", out.find("blocking.exe")) != std::string::npos;
            },
            kLongWaitLimit)) {
        server.stop();
        FAIL() << "first client must render the query result after the long query finishes; "
                  "stdout=["
               << first.out() << "] stderr=[" << first.err() << "]";
    }

    stdinBuffer().pushInput("help\n");
    if (!waitUntil([&first] { return first.out().find(kHelpText) != std::string::npos; },
                   kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        first.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "first client must remain usable after the long query; stdout=["
               << first.out() << "] stderr=[" << first.err() << "]";
    }

    stdinBuffer().pushInput("exit\n");
    ASSERT_TRUE(first.waitDone(kWaitLimit)) << "first client did not exit; stdout=["
                                            << first.out() << "]";
    ASSERT_TRUE(first.join());
    EXPECT_EQ(first.exitCode(), 0);

    server.stop();
}

/// 首会话断开且清理完成后才被处理的连接被接纳（处理时状态唯一规则）。
TEST(RemoteSessionSingleConnection, ConnectionProcessedAfterCleanupIsAccepted)
{
    ScopedRemoteStreams streams;
    LocalServer server(kCleanupAcceptPort);
    if (!server.start()) {
        if (isPortUnavailable(kCleanupAcceptPort)) {
            GTEST_SKIP() << "TCP port " << kCleanupAcceptPort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kCleanupAcceptPort;
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    ClientRun first("127.0.0.1", kCleanupAcceptPort);
    first.start();
    if (!waitUntil([&first] { return first.out().find("TPE>") != std::string::npos; },
                   kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        first.waitDone(std::chrono::milliseconds{1000});
        FAIL() << "first client did not display 'TPE>' within 5 s; stdout=[" << first.out()
               << "] stderr=[" << first.err() << "]";
    }
    stdinBuffer().pushInput("exit\n");
    if (!first.waitDone(kWaitLimit)) {
        stdinBuffer().interrupt();
        server.stop();
        FAIL() << "first client did not exit within 5 s; stdout=[" << first.out()
               << "] stderr=[" << first.err() << "]";
    }
    ASSERT_TRUE(first.join());
    ASSERT_EQ(first.exitCode(), 0)
        << "fixture sanity: the first session must end normally before reconnecting";

    // 清理完成后的连接必须被接纳；若仍处于清理/占用则收到 BUSY，有界重试。
    AcceptedClientStart accepted = connectClientAfterCleanup(kCleanupAcceptPort, kLongWaitLimit);
    ASSERT_TRUE(accepted.client != nullptr) << accepted.failure;

    stdinBuffer().pushInput("help\n");
    ASSERT_TRUE(waitUntil(
        [&accepted] { return accepted.client->out().find(kHelpText) != std::string::npos; },
        kWaitLimit))
        << "the accepted client must start from a clean, usable prompt; stdout=["
        << accepted.client->out() << "] stderr=[" << accepted.client->err() << "]";

    stdinBuffer().pushInput("exit\n");
    ASSERT_TRUE(accepted.client->waitDone(kWaitLimit));
    ASSERT_TRUE(accepted.client->join());
    EXPECT_EQ(accepted.client->exitCode(), 0);

    server.stop();
}

/// 时点边界 A（缩短期限模拟 30 秒规则）：首字节及时、完整帧在首字节后期限内 → BUSY。
TEST(RemoteSessionSingleConnection, TimingBoundaryABusyWhenFirstByteAndFrameAreOnTime)
{
    ScopedRemoteStreams streams;
    const std::vector<std::uint8_t> busyFrame =
        tpe::remote::encodeFrame(tpe::remote::MessageType::Busy, toBytes("Server is busy."));
    ScriptedServer server([busyFrame](SocketHandle socket, std::string& failure) {
        if (!consumeClientHello(socket, failure)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
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
    timeouts.firstByte = std::chrono::milliseconds{500};
    timeouts.frameComplete = std::chrono::milliseconds{400};
    ClientRun client("127.0.0.1", server.port(), timeouts);
    client.start();

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    ASSERT_EQ(client.exitCode(), 1);
    EXPECT_NE(client.err().find("Server is busy."), std::string::npos)
        << "a first byte inside the deadline and a complete frame inside the frame deadline "
           "must be reported as BUSY; stderr=["
        << client.err() << "]";
    EXPECT_EQ(client.err().find("No response from server."), std::string::npos)
        << "a timely first byte must not be reported as silence; stderr=[" << client.err()
        << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// 时点边界 B：首字节迟到（晚于 30 秒语义的注入期限）→ 静默超时退出 1。
TEST(RemoteSessionSingleConnection, TimingBoundaryBSilentTimeoutWhenFirstByteIsLate)
{
    ScopedRemoteStreams streams;
    const std::vector<std::uint8_t> busyFrame =
        tpe::remote::encodeFrame(tpe::remote::MessageType::Busy, toBytes("Server is busy."));
    ScriptedServer server([busyFrame](SocketHandle socket, std::string& failure) {
        if (!consumeClientHello(socket, failure)) {
            return false;
        }
        // 首字节 600 ms：晚于注入的 500 ms 期限（保留 > Windows 计时粒度的余量）。
        std::this_thread::sleep_for(std::chrono::milliseconds{600});
        sendAllBytes(socket, busyFrame.data(), busyFrame.size());
        waitForPeerEvent(socket, std::chrono::milliseconds{1000});
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    tpe::remote::ClientTimeouts timeouts;
    timeouts.firstByte = std::chrono::milliseconds{500};
    ClientRun client("127.0.0.1", server.port(), timeouts);
    client.start();

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    ASSERT_EQ(client.exitCode(), 1);
    EXPECT_NE(client.err().find("No response from server."), std::string::npos)
        << "a late first byte must be reported as silence; stderr=[" << client.err() << "]";
    EXPECT_EQ(client.err().find("Server is busy."), std::string::npos)
        << "a late first byte must not be treated as BUSY; stderr=[" << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// 时点边界 C：首字节及时但完整帧超过首字节后期限 → incomplete message、退出 1。
TEST(RemoteSessionSingleConnection, TimingBoundaryCIncompleteWhenFrameExceedsDeadline)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!consumeClientHello(socket, failure)) {
            return false;
        }
        const std::vector<std::uint8_t> busyFrame =
            tpe::remote::encodeFrame(tpe::remote::MessageType::Busy,
                                     toBytes("Server is busy."));
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        // 只发送帧头（首字节已到达），其余字节永不到达。
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
    timeouts.firstByte = std::chrono::milliseconds{3000};
    timeouts.frameComplete = std::chrono::milliseconds{300};
    ClientRun client("127.0.0.1", server.port(), timeouts);
    client.start();

    ASSERT_TRUE(client.waitDone(std::chrono::milliseconds{1500}))
        << "client must return after the injected frameComplete deadline (300 ms)";
    ASSERT_TRUE(client.join());
    ASSERT_EQ(client.exitCode(), 1);
    EXPECT_NE(client.err().find("Server sent an incomplete message."), std::string::npos)
        << "client stderr=[" << client.err() << "]";
    EXPECT_EQ(client.err().find("Server is busy."), std::string::npos)
        << "an incomplete frame must not be treated as BUSY; stderr=[" << client.err()
        << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

// ---------------------------------------------------------------------------
// 套件 RemoteSessionDraining — US3 清理与命令处置（T033）
//
// Draining 构造（T035 评审修复 C1）：通过
// RemoteServer::setDrainingCleanupDelayForTesting 注入 500 ms 清理延迟，把
// “会话已结束、清理尚未完成”的窗口拉长到可观测：
//   1) 首会话执行长查询期间另写 BYE（新读取事件，命令完成后才会被处理）；
//   2) 等待 Server stdout 出现 disconnect/ok（证明 BYE 已被处理、进入 Draining）；
//   3) 窗口内发起第二连接：被处理的连接事件必须收到 BUSY，且 Server 记
//      reject/busy（位置在 disconnect/ok 之后，排除“会话仍 Active 时被拒”）；
//   4) 清理完成后发起新连接：必须被接纳（WELCOME → TPE>）。
// 生产默认清理延迟仍为 10 ms，接缝只存储覆盖值，不改变清理语义。
//
// FR-012（T035 评审修复 C2）：协议规定 Client 每次只发送一条 COMMAND 并等待
// RESPONSE，因此一条 COMMAND 完成后不得在同一回调或任何定时器里“续行”执行
// 同一接收事件中的后续帧。libhv 在定时器已过期时会先处理定时器、跳过本轮 IO
// （hloop_process_events 的 `goto process_timers`），所以 1 ms 续行定时器可能
// 先于已排队的 close 事件执行“未开始”的命令。现实现不再续行：剩余字节一律视为
// “未开始”，只由后续新的读取事件处理；断开时随会话丢弃。
// 未开始（完整帧仍在接收缓冲区/不完整帧）在断开后不得执行；已开始（已进入
// 执行函数）允许自然结束，写回失败忽略。计数由 BlockingPlatformOS 的名称读取
// 次数提供。所有等待有界。
// ---------------------------------------------------------------------------

/// 在 Server 捕获输出中查找 `command=<event>` 且 `result=<result>` 的行起点
/// （logging.md §1 字段顺序固定）；未找到返回 npos。
std::size_t findServerLogLine(const std::string& text, const std::string& event,
                              const std::string& result, std::size_t from = 0)
{
    std::size_t position = text.find("command=", from);
    while (position != std::string::npos) {
        const std::size_t lineEnd = text.find('\n', position);
        const std::string line = text.substr(
            position, lineEnd == std::string::npos ? std::string::npos : lineEnd - position);
        if (line.find("command=" + event) != std::string::npos &&
            line.find("result=" + result) != std::string::npos) {
            return position;
        }
        if (lineEnd == std::string::npos) {
            return std::string::npos;
        }
        position = text.find("command=", lineEnd);
    }
    return std::string::npos;
}

/// Draining 窗口：首会话以 BYE 结束并保持占用；窗口内被处理的连接收到 BUSY，
/// 清理完成后的连接被接纳。
TEST(RemoteSessionDraining, NewConnectionDuringDrainingGetsBusyThenAcceptedAfterCleanup)
{
    ScopedRemoteStreams streams;
    auto os = std::make_unique<BlockingPlatformOS>();
    BlockingPlatformOS* const platform = os.get();
    LocalServer server(kDrainingBusyPort, std::move(os));
    ScopedPlatformRelease releaseOnExit(*platform);
    // 拉长 Draining 观测窗口（生产默认 10 ms；见套件注释） 。
    server.instance().setDrainingCleanupDelayForTesting(std::chrono::milliseconds{500});

    if (!server.start()) {
        if (isPortUnavailable(kDrainingBusyPort)) {
            GTEST_SKIP() << "TCP port " << kDrainingBusyPort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kDrainingBusyPort;
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    std::string failure;
    RawSocket first;
    ASSERT_TRUE(openRawSession(first, kDrainingBusyPort, failure)) << failure;

    // COMMAND 与 BYE 分开写：BYE 在命令执行期间到达，属于“后续新的读取事件”，
    // 命令完成后才会被处理（C2 的不续行规则不允许它随 COMMAND 的读取事件先执行）。
    ASSERT_TRUE(first.sendFrame(tpe::remote::MessageType::Command,
                                toBytes("--all-processes")))
        << "failed to send COMMAND";

    ASSERT_TRUE(platform->waitForNameReads(1, kLongWaitLimit))
        << "the long query must start within 10 s";
    ASSERT_TRUE(platform->waitForBlocked(kWaitLimit))
        << "the long query must be executing while BYE arrives";
    ASSERT_TRUE(first.sendFrame(tpe::remote::MessageType::Bye, {}))
        << "failed to send BYE";

    platform->release();

    // 进入 Draining 的前提：BYE 已被处理（disconnect/ok）。清理定时器随即开始计时。
    ASSERT_TRUE(waitUntil(
        [] {
            return findServerLogLine(capturedServerOut(), "disconnect", "ok") !=
                   std::string::npos;
        },
        kWaitLimit))
        << "BYE must be handled on the next read event and recorded as disconnect/ok; "
           "server stdout=["
        << capturedServerOut() << "]";
    const std::size_t disconnectAt =
        findServerLogLine(capturedServerOut(), "disconnect", "ok");
    ASSERT_NE(disconnectAt, std::string::npos);

    // 清理窗口内被处理的连接必须收到 BUSY。若未进入 Draining（BYE 后立即释放会话），
    // 该连接会被当作新会话等待 HELLO（收不到 BUSY）—— 这正是本用例要排除的
    // “只是在 Active 时 BUSY”。
    RawSocket second;
    ASSERT_TRUE(second.connectTo(kDrainingBusyPort, failure)) << failure;

    tpe::remote::Frame reply;
    ASSERT_TRUE(second.receiveFrame(std::chrono::milliseconds{3000}, reply, failure))
        << "a connection processed while the server is Draining must receive BUSY within "
           "the cleanup window; failure="
        << failure;
    EXPECT_EQ(static_cast<int>(reply.header.type),
              static_cast<int>(tpe::remote::MessageType::Busy))
        << "the Draining reply must be BUSY";
    EXPECT_EQ(std::string(reply.payload.begin(), reply.payload.end()), "Server is busy.")
        << "BUSY payload must be the fixed text";

    // Server 日志顺序佐证：reject/busy 出现在首会话 disconnect/ok 之后，说明该连接是
    // 在会话结束后的 Draining 中被处理，而不是会话仍 Active 时被拒。
    ASSERT_TRUE(waitUntil(
        [disconnectAt] {
            return findServerLogLine(capturedServerOut(), "reject", "busy", disconnectAt) !=
                   std::string::npos;
        },
        kWaitLimit))
        << "Server must record reject/busy after disconnect/ok while Draining; stdout=["
        << capturedServerOut() << "]";
    const std::size_t busyAt =
        findServerLogLine(capturedServerOut(), "reject", "busy", disconnectAt);
    ASSERT_NE(busyAt, std::string::npos);

    second.close();
    first.close();

    EXPECT_EQ(platform->nameReadCount(), 1)
        << "the started command must execute exactly once (write-back failure ignored)";
    // 37d8cf4：每次会话结束恰好 1 条 disconnect（Draining 期间自身的 close 事件不记录）。
    EXPECT_EQ(countOccurrences(capturedServerOut(), "command=disconnect"), 1)
        << "each session must record exactly one disconnect line; stdout=["
        << capturedServerOut() << "]";

    // 清理完成后被处理的新连接必须被接纳（有界重试；期间 BUSY 属预期）。
    AcceptedClientStart accepted = connectClientAfterCleanup(kDrainingBusyPort, kLongWaitLimit);
    ASSERT_TRUE(accepted.client != nullptr) << accepted.failure;
    EXPECT_NE(findServerLogLine(capturedServerOut(), "accept", "ok", busyAt),
              std::string::npos)
        << "the connection accepted after cleanup must be recorded after the Draining "
           "reject/busy; stdout=["
        << capturedServerOut() << "]";
    EXPECT_EQ(platform->nameReadCount(), 1)
        << "no discarded command may run during cleanup";

    stdinBuffer().pushInput("exit\n");
    ASSERT_TRUE(accepted.client->waitDone(kWaitLimit));
    ASSERT_TRUE(accepted.client->join());
    EXPECT_EQ(accepted.client->exitCode(), 0);

    server.stop();
}

/// FR-012 / C2 回归拦截：同一读取事件中的第二条完整 COMMAND 在首条 RESPONSE 之后
/// 不得被任何“续行”机制执行（同回调续行或定时器续行都会使计数变为 2、多出 RESPONSE）；
/// 断开时仍未开始的缓冲帧直接丢弃，不得因续行先于 close 而执行。
TEST(RemoteSessionDraining, BufferedCommandFrameIsDiscardedAfterDisconnect)
{
    ScopedRemoteStreams streams;
    auto os = std::make_unique<BlockingPlatformOS>();
    BlockingPlatformOS* const platform = os.get();
    LocalServer server(kBufferedCommandPort, std::move(os));
    ScopedPlatformRelease releaseOnExit(*platform);

    if (!server.start()) {
        if (isPortUnavailable(kBufferedCommandPort)) {
            GTEST_SKIP() << "TCP port " << kBufferedCommandPort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kBufferedCommandPort;
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    std::string failure;
    RawSocket first;
    ASSERT_TRUE(openRawSession(first, kBufferedCommandPort, failure)) << failure;

    std::vector<std::uint8_t> frames =
        tpe::remote::encodeFrame(tpe::remote::MessageType::Command,
                                 toBytes("--all-processes"));
    const std::vector<std::uint8_t> secondFrame =
        tpe::remote::encodeFrame(tpe::remote::MessageType::Command,
                                 toBytes("--all-processes"));
    frames.insert(frames.end(), secondFrame.begin(), secondFrame.end());
    ASSERT_TRUE(first.sendBytes(frames)) << "failed to send two COMMAND frames";

    ASSERT_TRUE(platform->waitForNameReads(1, kLongWaitLimit))
        << "the first command must start within 10 s";
    ASSERT_TRUE(platform->waitForBlocked(kWaitLimit))
        << "the first command must still be executing when the peer disconnects";

    platform->release();

    // 首条 RESPONSE 到达 = 首条命令已完成；此刻第二条完整帧仍在 Server 的接收缓冲。
    tpe::remote::Frame reply;
    ASSERT_TRUE(first.receiveFrame(kWaitLimit, reply, failure))
        << "the first command must produce a RESPONSE; failure=" << failure;
    EXPECT_EQ(static_cast<int>(reply.header.type),
              static_cast<int>(tpe::remote::MessageType::Response))
        << "the first completed command must answer with RESPONSE";

    // 回归拦截点 1：没有新的读取事件时，缓冲帧不得被续行执行（旧实现的 1 ms
    // 定时器会在此窗口内执行第二条命令并回第二条 RESPONSE）。
    tpe::remote::Frame unexpected;
    std::string noReplyFailure;
    const bool resumed = first.receiveFrame(std::chrono::milliseconds{500}, unexpected,
                                            noReplyFailure);
    EXPECT_FALSE(resumed)
        << "a COMMAND buffered in the same read event must not be resumed after the first "
           "RESPONSE; received message type "
        << static_cast<int>(unexpected.header.type);
    EXPECT_EQ(platform->nameReadCount(), 1)
        << "exactly one command per read event may enter the execution function";

    // 回归拦截点 2：断开后仍未开始的缓冲帧必须丢弃（不得因续行先于 close 而执行）。
    first.close();

    AcceptedClientStart accepted = connectClientAfterCleanup(kBufferedCommandPort, kLongWaitLimit);
    ASSERT_TRUE(accepted.client != nullptr) << accepted.failure;
    EXPECT_EQ(platform->nameReadCount(), 1)
        << "a complete COMMAND frame still in the receive buffer when the peer disconnects "
           "was not started and must not be executed";

    stdinBuffer().pushInput("exit\n");
    ASSERT_TRUE(accepted.client->waitDone(kWaitLimit));
    ASSERT_TRUE(accepted.client->join());
    EXPECT_EQ(accepted.client->exitCode(), 0);

    server.stop();
}

/// FR-012：不完整帧（仍在接收缓冲区、尚未构成完整帧）断开后不执行；计数为 0。
/// 该路径不依赖 Draining 实现，作为回归基线记录。
TEST(RemoteSessionDraining, IncompleteCommandFrameIsNotExecuted)
{
    ScopedRemoteStreams streams;
    auto os = std::make_unique<BlockingPlatformOS>();
    BlockingPlatformOS* const platform = os.get();
    LocalServer server(kIncompleteCommandPort, std::move(os));
    ScopedPlatformRelease releaseOnExit(*platform);

    if (!server.start()) {
        if (isPortUnavailable(kIncompleteCommandPort)) {
            GTEST_SKIP() << "TCP port " << kIncompleteCommandPort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kIncompleteCommandPort;
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    std::string failure;
    RawSocket first;
    ASSERT_TRUE(openRawSession(first, kIncompleteCommandPort, failure)) << failure;

    const std::vector<std::uint8_t> frame =
        tpe::remote::encodeFrame(tpe::remote::MessageType::Command,
                                 toBytes("--all-processes"));
    const std::size_t partial = tpe::remote::kFrameLengthBytes + 2;
    ASSERT_TRUE(first.sendBytes(
        std::vector<std::uint8_t>(frame.begin(), frame.begin() + partial)))
        << "failed to send the partial COMMAND frame";
    first.close();

    AcceptedClientStart accepted = connectClientAfterCleanup(kIncompleteCommandPort, kWaitLimit);
    ASSERT_TRUE(accepted.client != nullptr) << accepted.failure;
    EXPECT_EQ(platform->nameReadCount(), 0)
        << "a partial COMMAND frame must never enter the execution function";

    stdinBuffer().pushInput("exit\n");
    ASSERT_TRUE(accepted.client->waitDone(kWaitLimit));
    ASSERT_TRUE(accepted.client->join());
    EXPECT_EQ(accepted.client->exitCode(), 0);

    server.stop();
}

/// FR-012：已开始命令在断开后允许自然结束（计数 1），写回失败被忽略；
/// 清理完成后新连接被接纳。该路径不依赖 Draining 实现，作为回归基线记录。
TEST(RemoteSessionDraining, StartedCommandFinishesAfterDisconnect)
{
    ScopedRemoteStreams streams;
    auto os = std::make_unique<BlockingPlatformOS>();
    BlockingPlatformOS* const platform = os.get();
    LocalServer server(kStartedCommandPort, std::move(os));
    ScopedPlatformRelease releaseOnExit(*platform);

    if (!server.start()) {
        if (isPortUnavailable(kStartedCommandPort)) {
            GTEST_SKIP() << "TCP port " << kStartedCommandPort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kStartedCommandPort;
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    std::string failure;
    RawSocket first;
    ASSERT_TRUE(openRawSession(first, kStartedCommandPort, failure)) << failure;
    ASSERT_TRUE(first.sendFrame(tpe::remote::MessageType::Command,
                                toBytes("--all-processes")))
        << "failed to send COMMAND";

    ASSERT_TRUE(platform->waitForNameReads(1, kLongWaitLimit))
        << "the command must start within 10 s";
    ASSERT_TRUE(platform->waitForBlocked(kWaitLimit))
        << "the command must still be executing when the client disconnects";

    first.close(); // 断开发生在命令已进入执行函数之后
    platform->release();

    ASSERT_TRUE(waitUntil([platform] { return !platform->isBlocked(); }, kLongWaitLimit))
        << "the started command must finish naturally after the disconnect";

    AcceptedClientStart accepted = connectClientAfterCleanup(kStartedCommandPort, kLongWaitLimit);
    ASSERT_TRUE(accepted.client != nullptr) << accepted.failure;
    EXPECT_EQ(platform->nameReadCount(), 1)
        << "the started command must execute exactly once; write-back failure must be "
           "ignored";

    stdinBuffer().pushInput("exit\n");
    ASSERT_TRUE(accepted.client->waitDone(kWaitLimit));
    ASSERT_TRUE(accepted.client->join());
    EXPECT_EQ(accepted.client->exitCode(), 0);

    server.stop();
}

// ---------------------------------------------------------------------------
// 套件 RemoteSessionExitBye — US3 退出、断线与 BYE（T034）
//
// 固定文本按契约逐字复制（tests 不跨 TU 引用实现内部常量）：
//   - cli.md §7 / protocol.md §7：确认超时、非空 BYE、断线原因文本。
// 脚本化 Server 精确控制 Server 侧行为；所有等待有界（≤5 s）。
// ---------------------------------------------------------------------------

/// 脚本片段：消费 HELLO 并回 WELCOME。
bool completeScriptedHandshake(SocketHandle socket, std::string& failure)
{
    if (!consumeClientHello(socket, failure)) {
        return false;
    }
    if (!sendFrameBytes(socket, tpe::remote::MessageType::Welcome, {})) {
        failure = "failed to send WELCOME";
        return false;
    }
    return true;
}

/// 脚本片段：读取 Client 的 BYE，要求载荷为空。
bool expectClientBye(SocketHandle socket, std::string& failure)
{
    tpe::remote::Frame bye;
    if (!receiveFrameBefore(socket, kWaitLimit, bye, failure)) {
        return false;
    }
    if (bye.header.type != tpe::remote::MessageType::Bye) {
        failure = "expected BYE from the client, got type " +
                  std::to_string(static_cast<int>(bye.header.type));
        return false;
    }
    if (!bye.payload.empty()) {
        failure = "BYE payload must be empty";
        return false;
    }
    return true;
}

/// 断线固定文本（cli.md §7；与实现逐字一致，tests 不跨 TU 引用内部常量）。
constexpr const char* kServerClosedText = "The server closed the connection.";

/// 在脚本化 Server 上运行一个 Client 到 TPE>；失败时由调用方清理。
bool waitForClientPrompt(ClientRun& client)
{
    return waitUntil([&client] { return client.out().find("TPE>") != std::string::npos; },
                     kWaitLimit);
}

/// `exit` → Client 发送 BYE、Server 回 BYE 确认 → 退出码 0（cli.md §7）。
/// T023 已实现该路径；本用例显式断言 BYE 帧，作为回归基线。
TEST(RemoteSessionExitBye, ExitSendsByeAndReturnsZero)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!completeScriptedHandshake(socket, failure)) {
            return false;
        }
        if (!expectClientBye(socket, failure)) {
            return false;
        }
        if (!sendFrameBytes(socket, tpe::remote::MessageType::Bye, {})) {
            failure = "failed to send BYE confirmation";
            return false;
        }
        waitForPeerEvent(socket, std::chrono::milliseconds{2000});
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();
    if (!waitForClientPrompt(client)) {
        stdinBuffer().interrupt();
        FAIL() << "client did not display 'TPE>' within 5 s; stdout=[" << client.out()
               << "] stderr=[" << client.err() << "]";
    }
    stdinBuffer().pushInput("exit\n");

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    EXPECT_EQ(client.exitCode(), 0) << "a confirmed BYE must exit 0; stderr=["
                                    << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// 标准输入 EOF → 与 exit 相同的 BYE 确认 → 退出码 0（cli.md §7；quickstart 场景 11）。
TEST(RemoteSessionExitBye, EofSendsByeAndReturnsZero)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!completeScriptedHandshake(socket, failure)) {
            return false;
        }
        if (!expectClientBye(socket, failure)) {
            return false;
        }
        if (!sendFrameBytes(socket, tpe::remote::MessageType::Bye, {})) {
            failure = "failed to send BYE confirmation";
            return false;
        }
        waitForPeerEvent(socket, std::chrono::milliseconds{2000});
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();
    if (!waitForClientPrompt(client)) {
        stdinBuffer().interrupt();
        FAIL() << "client did not display 'TPE>' within 5 s; stdout=[" << client.out()
               << "] stderr=[" << client.err() << "]";
    }
    stdinBuffer().releaseWith(std::string{}); // EOF

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    EXPECT_EQ(client.exitCode(), 0) << "EOF with a confirmed BYE must exit 0; stderr=["
                                    << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// 已发送 BYE 但确认前连接断开 → 退出码 1（cli.md §7：未收到确认即断开）。
TEST(RemoteSessionExitBye, DisconnectBeforeByeConfirmationReturnsOne)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!completeScriptedHandshake(socket, failure)) {
            return false;
        }
        if (!expectClientBye(socket, failure)) {
            return false;
        }
        return true; // 不回 BYE，直接关闭
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();
    if (!waitForClientPrompt(client)) {
        stdinBuffer().interrupt();
        FAIL() << "client did not display 'TPE>' within 5 s; stdout=[" << client.out()
               << "] stderr=[" << client.err() << "]";
    }
    stdinBuffer().pushInput("exit\n");

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    EXPECT_EQ(client.exitCode(), 1)
        << "a disconnect before BYE confirmation must exit 1; stdout=[" << client.out()
        << "] stderr=[" << client.err() << "]";
    EXPECT_FALSE(client.err().empty())
        << "the disconnect must be reported on standard error";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// 等待响应时断线 → 立即显示原因并以 1 结束（cli.md §7；quickstart 场景 6）。
TEST(RemoteSessionExitBye, DisconnectWhileWaitingForResponseIsReportedImmediately)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!completeScriptedHandshake(socket, failure)) {
            return false;
        }
        tpe::remote::Frame command;
        if (!receiveFrameBefore(socket, kWaitLimit, command, failure)) {
            return false;
        }
        if (command.header.type != tpe::remote::MessageType::Command) {
            failure = "expected COMMAND before closing, got type " +
                      std::to_string(static_cast<int>(command.header.type));
            return false;
        }
        return true; // 不回 RESPONSE，直接关闭
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();
    if (!waitForClientPrompt(client)) {
        stdinBuffer().interrupt();
        FAIL() << "client did not display 'TPE>' within 5 s; stdout=[" << client.out()
               << "] stderr=[" << client.err() << "]";
    }

    const auto sentAt = std::chrono::steady_clock::now();
    stdinBuffer().pushInput("help\n");

    // T036 评审修复 M4：断线在等待 RESPONSE 期间到达时必须立即唤醒 run()，
    // 不得等到下一次输入。上界收紧到 800 ms（实测数十毫秒，保留负载容差）。
    ASSERT_TRUE(client.waitDone(std::chrono::milliseconds{800}))
        << "a disconnect while waiting for RESPONSE must be reported immediately; "
           "stdout=["
        << client.out() << "] stderr=[" << client.err() << "]";
    ASSERT_TRUE(client.join());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sentAt);
    EXPECT_EQ(client.exitCode(), 1);
    EXPECT_FALSE(client.err().empty()) << "the disconnect must be reported";
    EXPECT_LT(elapsed, std::chrono::milliseconds{800})
        << "the report must not wait for another input; elapsed=" << elapsed.count()
        << " ms";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// 等待输入时断线 → 下一次普通命令后报告并以 1 结束（cli.md §7）。
/// T037 已实现“输入返回后先报告已记录失败”；T036 评审修复 M5 进一步断言报告
/// 为固定断线文本（而非仅非空）。
TEST(RemoteSessionExitBye, DisconnectWhileWaitingForInputIsReportedAfterNextCommand)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!completeScriptedHandshake(socket, failure)) {
            return false;
        }
        // 让 Client 先进入 TPE> 等待输入，再关闭连接。
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();
    if (!waitForClientPrompt(client)) {
        stdinBuffer().interrupt();
        FAIL() << "client did not display 'TPE>' within 5 s; stdout=[" << client.out()
               << "] stderr=[" << client.err() << "]";
    }

    // 等待 Server 关闭连接（等待输入期间断线），再送入下一条普通命令。
    std::this_thread::sleep_for(std::chrono::milliseconds{800});
    stdinBuffer().pushInput("help\n");

    ASSERT_TRUE(client.waitDone(kWaitLimit))
        << "the next command after a disconnect must end the client; stdout=[" << client.out()
        << "] stderr=[" << client.err() << "]";
    ASSERT_TRUE(client.join());
    EXPECT_EQ(client.exitCode(), 1);
    EXPECT_FALSE(client.err().empty()) << "the disconnect must be reported";
    // 固定文本：等待输入期间断线不得被当作正常结束或通用错误。
    EXPECT_NE(client.err().find(kServerClosedText), std::string::npos)
        << "the disconnect must be reported with the fixed text; stderr=["
        << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// BYE 确认超时且连接仍在 → 显示固定文本、主动关闭、退出 1（cli.md §7；场景 18）。
TEST(RemoteSessionExitBye, ByeConfirmationTimeoutReturnsOneWhileConnectionStaysOpen)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!completeScriptedHandshake(socket, failure)) {
            return false;
        }
        if (!expectClientBye(socket, failure)) {
            return false;
        }
        // 不回 BYE，保持连接打开超过 1 秒确认期限。
        std::this_thread::sleep_for(std::chrono::milliseconds{2500});
        waitForPeerEvent(socket, std::chrono::milliseconds{1000});
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();
    if (!waitForClientPrompt(client)) {
        stdinBuffer().interrupt();
        FAIL() << "client did not display 'TPE>' within 5 s; stdout=[" << client.out()
               << "] stderr=[" << client.err() << "]";
    }

    const auto exitAt = std::chrono::steady_clock::now();
    stdinBuffer().pushInput("exit\n");

    // T036 评审修复 I1：确认期限固定 1 s；上界 1.5 s 确保实现退化为 3 s 时失败。
    ASSERT_TRUE(client.waitDone(std::chrono::milliseconds{1500}))
        << "the client must return after the 1 s confirmation deadline (a longer fallback "
           "must fail); stdout=["
        << client.out() << "] stderr=[" << client.err() << "]";
    ASSERT_TRUE(client.join());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - exitAt);
    EXPECT_EQ(client.exitCode(), 1);
    EXPECT_NE(client.err().find("Server did not acknowledge the session end."),
              std::string::npos)
        << "client stderr=[" << client.err() << "]";
    EXPECT_GE(elapsed, std::chrono::milliseconds{800})
        << "the client must wait for the 1 s deadline; elapsed=" << elapsed.count() << " ms";
    EXPECT_LT(elapsed, std::chrono::milliseconds{1500})
        << "the client must not linger beyond the 1 s deadline plus tolerance; elapsed="
        << elapsed.count() << " ms";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// 活动期收到 ERROR → 显示 Server 原因并退出 1（protocol.md §7）。
/// 断线/错误可能在等待输入时到达；下一次输入后必须报告原因（T037 前可能 RED）。
TEST(RemoteSessionExitBye, ActiveSessionErrorIsReportedWithReasonAndOne)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!completeScriptedHandshake(socket, failure)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        std::vector<std::uint8_t> payload{
            static_cast<std::uint8_t>(tpe::remote::ErrorCode::Internal)};
        const std::string reason = "simulated server failure";
        payload.insert(payload.end(), reason.begin(), reason.end());
        if (!sendFrameBytes(socket, tpe::remote::MessageType::Error, payload)) {
            failure = "failed to send ERROR";
            return false;
        }
        // T036 评审修复 M10：发送 ERROR 后等待对端关闭；任何后续 COMMAND/DATA
        // 都说明 Client 在已报告会话失败后仍发送了命令。
        std::string extra;
        const PeerEvent event =
            waitForPeerEvent(socket, std::chrono::milliseconds{3000}, &extra);
        if (event == PeerEvent::Data) {
            failure = "the client must not send COMMAND after an active-session ERROR; "
                      "received " +
                      std::to_string(extra.size()) + " byte(s)";
            return false;
        }
        if (event != PeerEvent::Closed) {
            failure = "the client must close the connection after reporting the ERROR";
            return false;
        }
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();
    if (!waitForClientPrompt(client)) {
        stdinBuffer().interrupt();
        FAIL() << "client did not display 'TPE>' within 5 s; stdout=[" << client.out()
               << "] stderr=[" << client.err() << "]";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{700});
    stdinBuffer().pushInput("help\n");

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    EXPECT_EQ(client.exitCode(), 1) << "an active-session ERROR must exit 1";
    EXPECT_NE(client.err().find("simulated server failure"), std::string::npos)
        << "the server-provided reason must be shown; stderr=[" << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// Server 发送载荷非空的 BYE → Client 显示固定文本、关闭、退出 1，且不回 ERROR
/// （protocol.md §2/§7；quickstart 场景 20 反向路径）。
TEST(RemoteSessionExitBye, NonEmptyByeFromServerIsInvalidSessionEnd)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!completeScriptedHandshake(socket, failure)) {
            return false;
        }
        tpe::remote::Frame command;
        if (!receiveFrameBefore(socket, kWaitLimit, command, failure)) {
            return false;
        }
        if (command.header.type != tpe::remote::MessageType::Command) {
            failure = "expected COMMAND before the invalid BYE, got type " +
                      std::to_string(static_cast<int>(command.header.type));
            return false;
        }
        if (!sendFrameBytes(socket, tpe::remote::MessageType::Bye, toBytes("x"))) {
            failure = "failed to send the non-empty BYE";
            return false;
        }
        std::string extra;
        const PeerEvent event =
            waitForPeerEvent(socket, std::chrono::milliseconds{3000}, &extra);
        if (event == PeerEvent::Data) {
            failure = "Client must not send any frame after an invalid session end; "
                      "received " +
                      std::to_string(extra.size()) + " byte(s)";
            return false;
        }
        if (event != PeerEvent::Closed) {
            failure = "Client must close the connection after an invalid session end";
            return false;
        }
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();
    if (!waitForClientPrompt(client)) {
        stdinBuffer().interrupt();
        FAIL() << "client did not display 'TPE>' within 5 s; stdout=[" << client.out()
               << "] stderr=[" << client.err() << "]";
    }
    stdinBuffer().pushInput("help\n"); // 让 Client 进入“等待响应”，接收非空 BYE

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    EXPECT_EQ(client.exitCode(), 1);
    EXPECT_NE(client.err().find("Server sent an invalid session end."), std::string::npos)
        << "client stderr=[" << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

/// 原始 RawSocket 活动会话发送非空 BYE → Server 实际发送 ERROR(6) 并关闭。
/// 契约声明该应答由 US3（T036）实现；当前实现已在 T029 的处理路径中覆盖，
/// 实测若直接通过则记录为回归基线，不为制造 RED 修改实现。
TEST(RemoteSessionExitBye, RawActiveClientSendingNonEmptyByeGetsErrorSixAndClose)
{
    ScopedRemoteStreams streams;
    LocalServer server(kRawNonEmptyByePort, std::make_unique<SnapshotPlatformOS>());
    if (!server.start()) {
        if (isPortUnavailable(kRawNonEmptyByePort)) {
            GTEST_SKIP() << "TCP port " << kRawNonEmptyByePort
                         << " is unavailable (bound by another process or reserved by the "
                            "system); skipping instead of misreporting a genuine port "
                            "conflict as a failure.";
        }
        FAIL() << "RemoteServer::start() must succeed on free port " << kRawNonEmptyByePort;
    }
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    std::string failure;
    RawSocket client;
    ASSERT_TRUE(openRawSession(client, kRawNonEmptyByePort, failure)) << failure;

    ASSERT_TRUE(client.sendFrame(tpe::remote::MessageType::Bye, toBytes("x")))
        << "failed to send the non-empty BYE";

    tpe::remote::Frame reply;
    ASSERT_TRUE(client.receiveFrame(kWaitLimit, reply, failure))
        << "the server must answer a non-empty BYE with ERROR; " << failure;
    ASSERT_EQ(static_cast<int>(reply.header.type),
              static_cast<int>(tpe::remote::MessageType::Error))
        << "a non-empty BYE from an active client must produce ERROR";
    ASSERT_FALSE(reply.payload.empty());
    EXPECT_EQ(reply.payload.at(0),
              static_cast<std::uint8_t>(tpe::remote::ErrorCode::InvalidPayload))
        << "the ERROR code must be 6 (invalid payload)";
    EXPECT_EQ(client.waitForPeer(kWaitLimit), PeerEvent::Closed)
        << "the server must close the connection after ERROR(6)";

    // T036 评审修复 I5（logging.md §1/§4）：活动期非空 BYE 恰好记 1 条
    // disconnect/error，且不得再记 disconnect/disconnected（Draining 期间自身
    // 关闭事件必须被忽略）。
    const std::string serverOut = capturedServerOut();
    const std::size_t errorLine = findServerLogLine(serverOut, "disconnect", "error");
    ASSERT_NE(errorLine, std::string::npos)
        << "the server must log a disconnect/error line; server stdout=[" << serverOut
        << "]";
    EXPECT_EQ(findServerLogLine(serverOut, "disconnect", "error", errorLine + 1),
              std::string::npos)
        << "exactly one disconnect/error line is allowed; server stdout=[" << serverOut
        << "]";
    EXPECT_EQ(findServerLogLine(serverOut, "disconnect", "disconnected"), std::string::npos)
        << "a non-empty BYE must not be logged as an unexpected disconnect; server "
           "stdout=["
        << serverOut << "]";

    server.stop();
}

/// 断线与 EOF 竞态 → 断线优先，退出码 1（cli.md §7；quickstart 场景 13）。
/// T036 评审修复 I2：Server 先半关闭发送方向（对端看到断线）并保持接收方向；
/// 若 Client 误按 EOF 正常结束路径发送 BYE（任何数据），脚本立即判失败。这证明
/// 1 与非空 stderr 来自断线报告，而不是“EOF → BYE → 确认前断线”的普通收尾。
TEST(RemoteSessionExitBye, DisconnectWinsOverEofRace)
{
    ScopedRemoteStreams streams;
    ScriptedServer server([](SocketHandle socket, std::string& failure) {
        if (!completeScriptedHandshake(socket, failure)) {
            return false;
        }
        // 半关闭：对端立即可见断线；本端仍能读取 Client 的后续字节（若有）。
        if (!shutdownSendSide(socket)) {
            failure = "failed to shut down the send direction";
            return false;
        }
        std::string extra;
        const PeerEvent event =
            waitForPeerEvent(socket, std::chrono::milliseconds{3000}, &extra);
        if (event == PeerEvent::Data) {
            failure = "the client must not send BYE when the disconnect wins over EOF; "
                      "received " +
                      std::to_string(extra.size()) + " byte(s)";
            return false;
        }
        if (event != PeerEvent::Closed) {
            failure = "the client must close the connection after reporting the disconnect";
            return false;
        }
        return true;
    });
    ASSERT_TRUE(server.start()) << "scripted server must bind 127.0.0.1";

    ClientRun client("127.0.0.1", server.port());
    client.start();
    if (!waitForClientPrompt(client)) {
        stdinBuffer().interrupt();
        FAIL() << "client did not display 'TPE>' within 5 s; stdout=[" << client.out()
               << "] stderr=[" << client.err() << "]";
    }

    // 先让 Client 的事件循环处理 FIN，再释放 EOF：只有“断线已先被观察”时，
    // 才允许断言断线优先（无输出可观测，用有界等待固定竞态方向）。
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
    stdinBuffer().releaseWith(std::string{});

    ASSERT_TRUE(client.waitDone(kWaitLimit)) << "client did not return within 5 s";
    ASSERT_TRUE(client.join());
    EXPECT_EQ(client.exitCode(), 1)
        << "a disconnect observed before EOF must exit 1 (disconnect wins); stdout=["
        << client.out() << "] stderr=[" << client.err() << "]";
    EXPECT_NE(client.err().find(kServerClosedText), std::string::npos)
        << "the disconnect must be reported with the fixed text instead of a normal "
           "exit; stderr=["
        << client.err() << "]";

    ASSERT_TRUE(server.waitScriptDone(kWaitLimit)) << "scripted server did not finish";
    EXPECT_TRUE(server.scriptSucceeded()) << server.scriptFailure();
}

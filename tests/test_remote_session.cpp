/**
 * test_remote_session.cpp — 远程会话回环握手测试（T020 RED；T022/T023 实现后转绿）
 *
 * 依据：
 *   - specs/001-remote-connection-phase-01/contracts/protocol.md §2/§3/§4
 *   - specs/001-remote-connection-phase-01/contracts/cli.md §2/§7
 *   - specs/001-remote-connection-phase-01/quickstart.md 场景 1/2/9/10/17
 *   - .superpowers/sdd/remote-connection-phase-01/t020-brief.md
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
 *   - 端口预检：8008/18080 已被其它进程占用时 GTEST_SKIP()，避免把真实端口冲突
 *     误报为骨架失败。
 *
 * 约束：仅 GoogleTest + 标准库；不启动子进程；除 127.0.0.1 回环外不访问网络。
 * 骨架期预期 RED：RemoteServer::start() 恒 false、RemoteClient::run() 恒 1。
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
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "ProcessEngine.hpp"
#include "RemoteClient.hpp"
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
#include <streambuf>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

/// 单次等待上限（brief：所有等待最多 5 秒）。
constexpr std::chrono::milliseconds kWaitLimit{5000};

/// 契约 §1：Server 省略端口时使用 8008；自定义端口用例固定 18080。
constexpr std::uint16_t kDefaultPort = 8008;
constexpr std::uint16_t kCustomPort = 18080;

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
// 端口预检（区分“真实端口冲突”与“骨架 start() 失败”）
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

/// 尝试在 INADDR_ANY:port 上 bind 一个临时 TCP socket（不设置 SO_REUSEADDR）：
/// bind 成功 = 端口空闲；WSAEADDRINUSE/EADDRINUSE = 被占用；其它错误 = 无法判定。
PortState probeTcpPort(std::uint16_t port)
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
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    const int result = ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const int error = (result == 0) ? 0 : ::WSAGetLastError();
    ::closesocket(handle);
    if (result == 0) {
        return PortState::Free;
    }
    return (error == WSAEADDRINUSE) ? PortState::Busy : PortState::Unknown;
#else
    const int handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle < 0) {
        return PortState::Unknown;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    const int result = ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const int error = errno;
    ::close(handle);
    if (result == 0) {
        return PortState::Free;
    }
    return (error == EADDRINUSE) ? PortState::Busy : PortState::Unknown;
#endif
}

/// brief 第 4 条：端口被其它进程占用时跳过并输出原因（仅限真实冲突）。
void skipIfPortBusy(std::uint16_t port)
{
    if (probeTcpPort(port) == PortState::Busy) {
        GTEST_SKIP() << "TCP port " << port
                     << " is already in use by another process; skipping instead of "
                        "misreporting a genuine port conflict as a skeleton failure.";
    }
}

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

    ~LocalServer() { m_server.stop(); }

    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;

    bool start() { return m_server.start(); }
    void stop() { m_server.stop(); }
    tpe::remote::RemoteServer& instance() { return m_server; }

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
    ClientRun(std::string host, std::uint16_t port)
        : m_host(std::move(host))
        , m_port(port)
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
        m_thread = std::thread([capture, host, port]() {
            const std::thread::id id = std::this_thread::get_id();
            stdoutRouter().registerClient(id, capture->out);
            stderrRouter().registerClient(id, capture->err);
            {
                tpe::remote::RemoteClient client(host, port);
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
    std::shared_ptr<ClientCapture> m_capture;
    std::thread m_thread;
};

// ---------------------------------------------------------------------------
// 共用场景：Server 启动 → Client 握手进入 TPE> → exit 正常退出
// ---------------------------------------------------------------------------

void expectSuccessfulHandshake(std::uint16_t port)
{
    skipIfPortBusy(port);

    ScopedRemoteStreams streams;
    LocalServer server(port);

    ASSERT_TRUE(server.start())
        << "RemoteServer::start() must succeed on free port " << port
        << " (skeleton currently returns false: expected RED)";
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

} // namespace

// ---------------------------------------------------------------------------
// 套件 RemoteSession — 回环握手（brief 用例 1–5）
// ---------------------------------------------------------------------------

/// brief 用例 1：自定义端口 start() 成功、isRunning() 为 true；
/// stop() 后为 false；可重复 stop()。
TEST(RemoteSession, ServerStartsAndStops)
{
    skipIfPortBusy(kCustomPort);

    ScopedRemoteStreams streams;
    LocalServer server(kCustomPort);

    ASSERT_TRUE(server.start())
        << "RemoteServer::start() must succeed on free port " << kCustomPort
        << " (skeleton currently returns false: expected RED)";
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

/// brief 用例 3：自定义端口 18080 的回环握手（quickstart 场景 2）。
TEST(RemoteSession, CustomPortHandshake)
{
    expectSuccessfulHandshake(kCustomPort);
}

/// brief 用例 4：首 Client 停留在 TPE> 时，第二 Client 收到 BUSY；
/// 首 Client 之后仍能正常退出（quickstart 场景 4；protocol §3）。
TEST(RemoteSession, SecondClientDuringHandshakeGetsBusy)
{
    skipIfPortBusy(kCustomPort);

    ScopedRemoteStreams streams;
    LocalServer server(kCustomPort);

    ASSERT_TRUE(server.start())
        << "RemoteServer::start() must succeed on free port " << kCustomPort
        << " (skeleton currently returns false: expected RED)";
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    ClientRun first("127.0.0.1", kCustomPort);
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
    ClientRun second("127.0.0.1", kCustomPort);
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
    skipIfPortBusy(kCustomPort);

    ScopedRemoteStreams streams;
    LocalServer server(kCustomPort);

    ASSERT_TRUE(server.start())
        << "RemoteServer::start() must succeed on free port " << kCustomPort
        << " (skeleton currently returns false: expected RED)";
    ASSERT_TRUE(waitUntil([&server] { return server.instance().isRunning(); }, kWaitLimit))
        << "isRunning() must become true within 5 s after start() returned true";

    ClientRun client("127.0.0.1", kCustomPort);
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

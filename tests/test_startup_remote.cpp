// T012：远程启动参数与入口分流契约测试（TDD RED，T018/T024 实现后转绿）。
// T040：远程启动失败输出流与退出码测试（TDD RED，T042 实现后转绿）。
// 依据：specs/001-remote-connection-phase-01/contracts/cli.md §1/§2/§8、
//       src/startup/startup_remote.hpp、src/startup/startup.hpp。
// 约束：只用 GoogleTest；除 127.0.0.1 回环外不访问网络；不启动子进程；
//       Server 成功启动路径不测试（主循环会阻塞，直至外部终止）。
//
// 骨架期预期 RED：
//   - parseRemoteStartupArgs 恒返回错误 "not implemented" → 解析成功类用例失败；
//   - startup.cpp 尚未识别 --server/--connect → 注入替身的调用次数为 0。
//
// 注入替身一律经 RAII 复位为默认入口（nullptr）；run() 期间写入 stdout/stderr 的
// 内容由 RAII 捕获丢弃，避免污染测试输出（不改变被测语义）。

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
#include <hv/hsocket.h>

#include "startup.hpp"
#include "startup_remote.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace {

using tpe::app::RemoteMode;
using tpe::app::RemoteStartupOptions;

/// 分流替身记录：调用次数、收到的参数、模拟退出码。
/// 函数指针无法捕获状态，因此记录放在匿名命名空间（每个用例开始前重置）。
struct RemoteEntryCapture {
    int calls = 0;
    std::vector<std::string> args;
    int exitCode = 0;
};

RemoteEntryCapture g_capture;

/// 替身入口：只记录调用与参数，绝不监听或连接。
int recordingRemoteEntry(const std::vector<std::string>& args)
{
    ++g_capture.calls;
    g_capture.args = args;
    return g_capture.exitCode;
}

/// 注入远程入口替身，并在作用域结束时无条件复位为默认入口。
class ScopedRemoteEntry {
public:
    explicit ScopedRemoteEntry(tpe::app::RemoteEntryFn entry)
    {
        tpe::app::setRemoteEntryForTesting(entry);
    }

    ~ScopedRemoteEntry() { tpe::app::setRemoteEntryForTesting(nullptr); }

    ScopedRemoteEntry(const ScopedRemoteEntry&) = delete;
    ScopedRemoteEntry& operator=(const ScopedRemoteEntry&) = delete;
};

/// 捕获 run() 期间写入 stdout/stderr 的内容并丢弃。
/// 仅改变流的落点，不改变被测语义；析构时恢复原缓冲。
class ScopedStreamSilencer {
public:
    ScopedStreamSilencer()
    {
        m_oldOut = std::cout.rdbuf(m_out.rdbuf());
        m_oldErr = std::cerr.rdbuf(m_err.rdbuf());
    }

    ~ScopedStreamSilencer()
    {
        std::cout.rdbuf(m_oldOut);
        std::cerr.rdbuf(m_oldErr);
    }

    ScopedStreamSilencer(const ScopedStreamSilencer&) = delete;
    ScopedStreamSilencer& operator=(const ScopedStreamSilencer&) = delete;

private:
    std::ostringstream m_out;
    std::ostringstream m_err;
    std::streambuf* m_oldOut = nullptr;
    std::streambuf* m_oldErr = nullptr;
};

/// 以 C 风格 argv 调用应用入口（args 不含程序名）。
int runApp(const std::vector<std::string>& args)
{
    const std::string programName = "tpe_tests";
    std::vector<std::string> storage = args;
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    argv.push_back(const_cast<char*>(programName.c_str()));
    for (std::string& arg : storage) {
        argv.push_back(arg.data());
    }
    return tpe::app::run(static_cast<int>(argv.size()), argv.data());
}

/// 断言解析失败且原因非空（契约 §1/§8：参数错误按用法错误处理）。
void expectParseError(const std::vector<std::string>& args)
{
    const auto result = tpe::app::parseRemoteStartupArgs(args);
    ASSERT_FALSE(result.has_value()) << "expected a usage error, got a parsed value";
    EXPECT_FALSE(result.error().empty()) << "error reason must not be empty";
}

} // namespace

// ---------------------------------------------------------------------------
// 套件 RemoteStartupArgs — 远程启动参数解析（contracts/cli.md §1/§8）
// ---------------------------------------------------------------------------

/// §1：Server 省略端口时使用 8008。
TEST(RemoteStartupArgs, ServerOnlyDefaultsToPort8008)
{
    const auto result = tpe::app::parseRemoteStartupArgs({"--server"});

    ASSERT_TRUE(result.has_value()) << "expected --server to parse successfully";
    const RemoteStartupOptions& options = result.value();
    EXPECT_EQ(options.mode, RemoteMode::Server);
    EXPECT_EQ(options.port, std::uint16_t{8008});
}

/// §1：Server 显式端口覆盖默认值。
TEST(RemoteStartupArgs, ServerWithExplicitPortUsesIt)
{
    const auto result = tpe::app::parseRemoteStartupArgs({"--server", "-port", "9000"});

    ASSERT_TRUE(result.has_value()) << "expected --server -port 9000 to parse successfully";
    const RemoteStartupOptions& options = result.value();
    EXPECT_EQ(options.mode, RemoteMode::Server);
    EXPECT_EQ(options.port, std::uint16_t{9000});
}

/// §1：端口下边界 1 是合法取值。
TEST(RemoteStartupArgs, ServerWithMinimumPortParses)
{
    const auto result = tpe::app::parseRemoteStartupArgs({"--server", "-port", "1"});

    ASSERT_TRUE(result.has_value()) << "expected --server -port 1 to parse successfully";
    const RemoteStartupOptions& options = result.value();
    EXPECT_EQ(options.mode, RemoteMode::Server);
    EXPECT_EQ(options.port, std::uint16_t{1});
}

/// §1：端口上边界 65535 是合法取值。
TEST(RemoteStartupArgs, ServerWithMaximumPortParses)
{
    const auto result = tpe::app::parseRemoteStartupArgs({"--server", "-port", "65535"});

    ASSERT_TRUE(result.has_value()) << "expected --server -port 65535 to parse successfully";
    const RemoteStartupOptions& options = result.value();
    EXPECT_EQ(options.mode, RemoteMode::Server);
    EXPECT_EQ(options.port, std::uint16_t{65535});
}

/// §1：Client 需要 IPv4 字面量地址与端口。
TEST(RemoteStartupArgs, ClientWithHostAndPortParses)
{
    const auto result =
        tpe::app::parseRemoteStartupArgs({"--connect", "127.0.0.1", "-port", "9000"});

    ASSERT_TRUE(result.has_value()) << "expected --connect 127.0.0.1 -port 9000 to parse";
    const RemoteStartupOptions& options = result.value();
    EXPECT_EQ(options.mode, RemoteMode::Client);
    EXPECT_EQ(options.host, "127.0.0.1");
    EXPECT_EQ(options.port, std::uint16_t{9000});
}

/// §1：--server 与 --connect 互斥，并存为用法错误。
TEST(RemoteStartupArgs, ServerAndConnectTogetherAreRejected)
{
    expectParseError({"--server", "--connect", "127.0.0.1", "-port", "9000"});
}

/// §1：互斥与次序无关——先 --connect 后 --server 同样是用法错误。
TEST(RemoteStartupArgs, ServerAfterConnectIsRejected)
{
    expectParseError({"--connect", "127.0.0.1", "-port", "9000", "--server"});
}

/// §1：重复 --server 为用法错误。
TEST(RemoteStartupArgs, DuplicateServerIsRejected)
{
    expectParseError({"--server", "--server"});
}

/// §1：重复 --connect 为用法错误。
TEST(RemoteStartupArgs, DuplicateConnectIsRejected)
{
    expectParseError(
        {"--connect", "127.0.0.1", "-port", "9000", "--connect", "127.0.0.1", "-port", "9000"});
}

/// §1：端口只能是 1–65535 的十进制整数。
TEST(RemoteStartupArgs, OutOfRangeOrNonNumericPortIsRejected)
{
    for (const char* port : {"0", "65536", "abc"}) {
        SCOPED_TRACE(std::string("-port ") + port);
        expectParseError({"--server", "-port", port});
    }
}

/// §1/§8：-port 缺取值（Server 侧悬空）为用法错误。
TEST(RemoteStartupArgs, DanglingPortValueIsRejected)
{
    expectParseError({"--server", "-port"});
}

/// §1/§8：Client 缺地址（-port 前没有 IP）为用法错误。
TEST(RemoteStartupArgs, ClientWithoutAddressIsRejected)
{
    expectParseError({"--connect", "-port", "9000"});
}

/// §1/§8：Client 缺端口为用法错误。
TEST(RemoteStartupArgs, ClientWithoutPortIsRejected)
{
    expectParseError({"--connect", "127.0.0.1"});
}

/// §1：Client 地址必须是 IPv4 字面量，不是任意主机名/非法字面量。
TEST(RemoteStartupArgs, InvalidIpv4LiteralIsRejected)
{
    expectParseError({"--connect", "999.1.1.1", "-port", "9000"});
}

/// §1：主机名不是 IPv4 字面量，--connect localhost 属用法错误。
TEST(RemoteStartupArgs, HostnameInsteadOfIpv4LiteralIsRejected)
{
    expectParseError({"--connect", "localhost", "-port", "9000"});
}

/// §8：未知参数为用法错误。
TEST(RemoteStartupArgs, UnknownArgumentIsRejected)
{
    expectParseError({"--bogus"});
}

/// §1：远程模式必须显式指定角色，空参数列表为用法错误。
TEST(RemoteStartupArgs, EmptyArgumentListIsRejected)
{
    expectParseError(std::vector<std::string>{});
}

// ---------------------------------------------------------------------------
// 套件 StartupDispatch — 入口分流（有界、不监听；T024 前为 RED）
// ---------------------------------------------------------------------------

/// brief 第 10 条：--server 触发远程替身 1 次，参数原样，返回值透传。
TEST(StartupDispatch, ServerArgumentInvokesInjectedRemoteEntry)
{
    g_capture = RemoteEntryCapture{};
    g_capture.exitCode = 42;
    ScopedRemoteEntry injected(&recordingRemoteEntry);
    ScopedStreamSilencer silencer;

    const int rc = runApp({"--server"});

    EXPECT_EQ(g_capture.calls, 1);
    EXPECT_EQ(g_capture.args, std::vector<std::string>({"--server"}));
    EXPECT_EQ(rc, 42);
}

/// brief 第 11 条：--connect 触发远程替身 1 次，参数原样，返回值透传。
TEST(StartupDispatch, ConnectArgumentInvokesInjectedRemoteEntry)
{
    g_capture = RemoteEntryCapture{};
    g_capture.exitCode = 7;
    ScopedRemoteEntry injected(&recordingRemoteEntry);
    ScopedStreamSilencer silencer;

    const int rc = runApp({"--connect", "127.0.0.1", "-port", "9000"});

    EXPECT_EQ(g_capture.calls, 1);
    EXPECT_EQ(g_capture.args,
              std::vector<std::string>({"--connect", "127.0.0.1", "-port", "9000"}));
    EXPECT_EQ(rc, 7);
}

/// brief 第 12 条：本地 CLI 路径不受影响——--help 不触发远程替身且返回 0。
TEST(StartupDispatch, HelpDoesNotInvokeRemoteEntry)
{
    g_capture = RemoteEntryCapture{};
    ScopedRemoteEntry injected(&recordingRemoteEntry);
    ScopedStreamSilencer silencer;

    const int rc = runApp({"--help"});

    EXPECT_EQ(g_capture.calls, 0);
    EXPECT_EQ(rc, 0);
}

// ===========================================================================
// T040：远程启动失败输出流与退出码（contracts/cli.md §2/§8；US4）
//
// 覆盖三类失败：
//   1) 参数错误           → 原因 stderr、用法 stdout、退出码 2（基线）
//   2) 端口被占用         → 具体原因 stderr、退出码 1（T042 前原因不具体，预期 RED）
//   3) 目标端口无监听进程 → `Connection failed.` + 原因 stderr、退出码 1（基线）
//
// 约束：
//   - run() 在独立线程中执行并以 5 s 有界等待；超时 detach 后 FAIL，绝不做无界 join；
//   - “端口占用”由本进程预绑定监听 socket 构造（Windows 用 SO_EXCLUSIVEADDRUSE，
//     阻止 libhv 的 SO_REUSEADDR 复用）；“连接失败”由本进程预绑定但不 listen 的
//     loopback socket 构造（连接必被拒绝，且不会被其它进程抢占）；
//   - 不测试 Server 成功启动路径（主循环会持续运行）。
// ===========================================================================

namespace {

using namespace std::chrono_literals;

/// 单次等待上限（brief：所有等待最多 5 秒）。
constexpr std::chrono::milliseconds kWaitLimit{5000};

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
// 线程安全 stdout/stderr 捕获
// ---------------------------------------------------------------------------

class StartupTextSink {
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

/// 进程级单例（故意不回收）：超时 detach 的 run() 线程仍可安全写入。
StartupTextSink& startupStdoutSink()
{
    static StartupTextSink* sink = new StartupTextSink();
    return *sink;
}

StartupTextSink& startupStderrSink()
{
    static StartupTextSink* sink = new StartupTextSink();
    return *sink;
}

class StartupSinkStreambuf : public std::streambuf {
public:
    explicit StartupSinkStreambuf(StartupTextSink& sink)
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
    StartupTextSink& m_sink;
};

StartupSinkStreambuf& startupStdoutBuffer()
{
    static StartupSinkStreambuf* buffer = new StartupSinkStreambuf(startupStdoutSink());
    return *buffer;
}

StartupSinkStreambuf& startupStderrBuffer()
{
    static StartupSinkStreambuf* buffer = new StartupSinkStreambuf(startupStderrSink());
    return *buffer;
}

/// RAII 捕获 std::cout/std::cerr：构造时清空，析构时恢复原缓冲。
class ScopedStreamCapture {
public:
    ScopedStreamCapture()
    {
        startupStdoutSink().clear();
        startupStderrSink().clear();
        std::cout.flush();
        std::cerr.flush();
        m_oldOut = std::cout.rdbuf(&startupStdoutBuffer());
        m_oldErr = std::cerr.rdbuf(&startupStderrBuffer());
        std::cout.clear();
        std::cerr.clear();
    }

    ~ScopedStreamCapture()
    {
        std::cout.flush();
        std::cerr.flush();
        std::cout.rdbuf(m_oldOut);
        std::cerr.rdbuf(m_oldErr);
        std::cout.clear();
        std::cerr.clear();
    }

    ScopedStreamCapture(const ScopedStreamCapture&) = delete;
    ScopedStreamCapture& operator=(const ScopedStreamCapture&) = delete;

    std::string out() const { return startupStdoutSink().text(); }
    std::string err() const { return startupStderrSink().text(); }

private:
    std::streambuf* m_oldOut = nullptr;
    std::streambuf* m_oldErr = nullptr;
};

// ---------------------------------------------------------------------------
// 平台 socket 工具与端口构造
// ---------------------------------------------------------------------------

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

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

bool ensureSocketsReady()
{
    static const WinsockGuard* guard = new WinsockGuard();
    return guard->ok();
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

bool ensureSocketsReady()
{
    return true;
}
#endif

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

bool fetchBoundPort(SocketHandle handle, std::uint16_t& port)
{
    sockaddr_in bound{};
#ifdef _WIN32
    int length = sizeof(bound);
#else
    socklen_t length = sizeof(bound);
#endif
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        return false;
    }
    port = ntohs(bound.sin_port);
    return port != 0;
}

/// 预绑定并 listen 的 0.0.0.0 临时端口：模拟“端口被占用”。
/// Windows 先设置 SO_EXCLUSIVEADDRUSE，阻止 libhv 以 SO_REUSEADDR 复用同一端口；
/// Linux 上处于 LISTEN 的端口本身就会让后续 bind 返回 EADDRINUSE。
class BoundListeningPort {
public:
    BoundListeningPort() = default;
    ~BoundListeningPort() { closeSocketHandle(m_handle); }

    BoundListeningPort(const BoundListeningPort&) = delete;
    BoundListeningPort& operator=(const BoundListeningPort&) = delete;

    bool open(std::uint16_t& port, std::string& failure)
    {
        if (!ensureSocketsReady()) {
            failure = "socket subsystem unavailable";
            return false;
        }
        m_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_handle == kInvalidSocket) {
            failure = "socket() failed";
            return false;
        }
#ifdef _WIN32
        const BOOL exclusive = TRUE;
        if (::setsockopt(m_handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                         reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
            failure = "setsockopt(SO_EXCLUSIVEADDRUSE) failed";
            closeSocketHandle(m_handle);
            m_handle = kInvalidSocket;
            return false;
        }
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(m_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            failure = "bind() failed";
            closeSocketHandle(m_handle);
            m_handle = kInvalidSocket;
            return false;
        }
        if (::listen(m_handle, 1) != 0) {
            failure = "listen() failed";
            closeSocketHandle(m_handle);
            m_handle = kInvalidSocket;
            return false;
        }
        if (!fetchBoundPort(m_handle, port)) {
            failure = "getsockname() failed";
            closeSocketHandle(m_handle);
            m_handle = kInvalidSocket;
            return false;
        }
        return true;
    }

private:
    SocketHandle m_handle = kInvalidSocket;
};

/// 预绑定但不 listen 的 loopback 临时端口：连接必被拒绝（RST），
/// 同时该端口不会被其它进程抢占，避免连接失败用例竞态。
class BoundClosedPort {
public:
    BoundClosedPort() = default;
    ~BoundClosedPort() { closeSocketHandle(m_handle); }

    BoundClosedPort(const BoundClosedPort&) = delete;
    BoundClosedPort& operator=(const BoundClosedPort&) = delete;

    bool open(std::uint16_t& port, std::string& failure)
    {
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
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(m_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            failure = "bind() failed";
            closeSocketHandle(m_handle);
            m_handle = kInvalidSocket;
            return false;
        }
        // 故意不调用 listen()：connect 会立即得到 ECONNREFUSED。
        if (!fetchBoundPort(m_handle, port)) {
            failure = "getsockname() failed";
            closeSocketHandle(m_handle);
            m_handle = kInvalidSocket;
            return false;
        }
        return true;
    }

private:
    SocketHandle m_handle = kInvalidSocket;
};

#ifdef _WIN32
constexpr int kExpectedConnectRefused = WSAECONNREFUSED;
#else
constexpr int kExpectedConnectRefused = ECONNREFUSED;
#endif

/// 对同一受控拒绝端口执行一次独立 connect，最多等待 6 秒，返回平台原生错误码。
/// 仅用于 T053 的独立探针；受控端口由 BoundClosedPort 持有。
int probeConnectErrorCode(std::uint16_t port)
{
    constexpr int kPending = -999;
    auto result = std::make_shared<std::atomic<int>>(kPending);
    std::thread worker([port, result] {
        SocketHandle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (handle == kInvalidSocket) {
            result->store(-1);
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const int connected =
            ::connect(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        const int error = (connected == 0) ? 0 : socket_errno();
        closeSocketHandle(handle);
        result->store(error);
    });
    worker.detach();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{6};
    while (result->load() == kPending && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    const int error = result->load();
    return error == kPending ? -1 : error;
}

/// 原因文本归一化：大小写、换行、首尾空白、标点与连续空白差异不计。
std::string normalizeReasonText(std::string text)
{
    for (char& character : text) {
        if (character == '\r' || character == '\n' || character == '\t') {
            character = ' ';
        } else {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        }
    }
    while (!text.empty() &&
           (std::isspace(static_cast<unsigned char>(text.back())) || text.back() == '.' ||
            text.back() == ',' || text.back() == ';')) {
        text.pop_back();
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    std::string collapsed;
    bool previousSpace = false;
    for (char character : text) {
        const bool space = std::isspace(static_cast<unsigned char>(character)) != 0;
        if (space && previousSpace) {
            continue;
        }
        collapsed.push_back(space ? ' ' : character);
        previousSpace = space;
    }
    return collapsed;
}

/// 只接受唯一一条 `Connection failed. Failed to connect to <ip>:<port>: ` 诊断行的
/// 非空原因字段；其它行出现 refusing/refused 等文字不影响该判据。
bool extractFailedConnectReason(const std::string& stderrText, std::uint16_t port,
                               std::string& reason)
{
    const std::string prefix = "Connection failed. Failed to connect to 127.0.0.1:" +
                               std::to_string(port) + ": ";
    std::size_t matches = 0;
    std::string candidate;
    std::size_t begin = 0;
    while (begin <= stderrText.size()) {
        const std::size_t end = stderrText.find('\n', begin);
        std::string line = stderrText.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind(prefix, 0) == 0) {
            ++matches;
            candidate = line.substr(prefix.size());
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    if (matches != 1 || candidate.empty()) {
        return false;
    }
    reason = candidate;
    return true;
}

/// `runApp` 的跨线程结果（shared_ptr 保证超时 detach 后仍有效）。
struct RunOutcome {
    std::atomic<bool> done{false};
    std::atomic<int> exitCode{-999};
};

/// 在独立线程中调用 runApp，并以 kWaitLimit 有界等待；超时 detach 后返回 false。
bool runAppBounded(const std::vector<std::string>& args, const std::shared_ptr<RunOutcome>& outcome)
{
    std::thread worker([args, outcome] {
        outcome->exitCode.store(runApp(args), std::memory_order_relaxed);
        outcome->done.store(true, std::memory_order_release);
    });
    if (!waitUntil([&outcome] { return outcome->done.load(std::memory_order_acquire); },
                   kWaitLimit)) {
        worker.detach(); // 有界失败：绝不对可能永不返回的线程做无界 join
        return false;
    }
    worker.join();
    return true;
}

std::string toLowerAscii(std::string text)
{
    for (char& character : text) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return text;
}

/// 绑定失败原因是否点明“端口已被占用”（而非仅重复端口号）。
/// 契约 §8 要求“具体原因”；跨平台允许系统错误文本或原生错误码。
bool mentionsAddressInUse(const std::string& text)
{
    const std::string lower = toLowerAscii(text);
    return lower.find("in use") != std::string::npos ||
           lower.find("already") != std::string::npos ||
           lower.find("10048") != std::string::npos ||
           lower.find("eaddrinuse") != std::string::npos ||
           lower.find("normally permitted") != std::string::npos ||
           lower.find("address is not available") != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// 失败输出流与退出码（cli.md §2/§8）
// ---------------------------------------------------------------------------

/// 参数错误：具体原因写标准错误、用法写标准输出、退出码 2（契约 §1/§8）。
TEST(RemoteStartupFailureOutput, ArgumentErrorWritesReasonToStderrAndUsageToStdout)
{
    ScopedStreamCapture capture;

    const int rc = runApp({"--server", "-port", "0"});

    EXPECT_EQ(rc, 2) << "用法错误必须以退出码 2 结束";
    EXPECT_FALSE(capture.err().empty()) << "标准错误必须包含具体原因";
    EXPECT_NE(toLowerAscii(capture.err()).find("port"), std::string::npos)
        << "原因应点明非法端口；stderr=[" << capture.err() << "]";
    EXPECT_NE(capture.out().find("Usage:"), std::string::npos)
        << "标准输出必须包含用法；stdout=[" << capture.out() << "]";
    EXPECT_NE(capture.out().find("--server"), std::string::npos)
        << "stdout=[" << capture.out() << "]";
    EXPECT_NE(capture.out().find("--connect"), std::string::npos)
        << "stdout=[" << capture.out() << "]";
    EXPECT_EQ(capture.err().find("Usage:"), std::string::npos)
        << "用法不得写入标准错误；stderr=[" << capture.err() << "]";
}

/// 端口被占用：具体原因写标准错误、退出码 1，且不得宣告开始监听（契约 §8）。
TEST(RemoteStartupFailureOutput, OccupiedPortWritesSpecificReasonToStderrAndReturnsOne)
{
    BoundListeningPort listener;
    std::uint16_t port = 0;
    std::string failure;
    ASSERT_TRUE(listener.open(port, failure)) << failure;

    ScopedStreamCapture capture;
    const auto outcome = std::make_shared<RunOutcome>();
    ASSERT_TRUE(runAppBounded({"--server", "-port", std::to_string(port)}, outcome))
        << "run() must return within 5 s instead of blocking on a successful listener";

    EXPECT_EQ(outcome->exitCode.load(std::memory_order_relaxed), 1)
        << "绑定失败必须以退出码 1 结束";
    const std::string error = capture.err();
    EXPECT_FALSE(error.empty()) << "标准错误必须包含具体原因";
    EXPECT_NE(error.find(std::to_string(port)), std::string::npos)
        << "原因应包含目标端口；stderr=[" << error << "]";
    // T042 前默认入口只输出 "Failed to listen on port <n>."（不含系统原因）：
    // 本断言预期 RED。
    EXPECT_TRUE(mentionsAddressInUse(error))
        << "绑定失败必须给出具体原因（端口占用/系统错误），当前仅重复端口号；stderr=["
        << error << "]";
    EXPECT_EQ(capture.out().find("Waiting for connection ..."), std::string::npos)
        << "启动失败不得输出监听提示；stdout=[" << capture.out() << "]";
}

/// 目标端口无监听进程：`Connection failed.` + 原因写标准错误、退出码 1（契约 §2/§8）。
TEST(RemoteStartupFailureOutput, ConnectionRefusedWritesConnectionFailedToStderrAndReturnsOne)
{
    BoundClosedPort closed;
    std::uint16_t port = 0;
    std::string failure;
    ASSERT_TRUE(closed.open(port, failure)) << failure;

    ScopedStreamCapture capture;
    const auto outcome = std::make_shared<RunOutcome>();
    ASSERT_TRUE(runAppBounded({"--connect", "127.0.0.1", "-port", std::to_string(port)},
                              outcome))
        << "run() must return within 5 s when the target port is closed";

    EXPECT_EQ(outcome->exitCode.load(std::memory_order_relaxed), 1)
        << "连接失败必须以退出码 1 结束";
    const std::string error = capture.err();
    EXPECT_NE(error.find("Connection failed."), std::string::npos)
        << "标准错误必须含固定前缀；stderr=[" << error << "]";
    EXPECT_EQ(capture.out().find("Connection failed."), std::string::npos)
        << "失败文本属于标准错误；stdout=[" << capture.out() << "]";
    EXPECT_EQ(capture.out().find("TPE>"), std::string::npos)
        << "连接未建立时不得进入 TPE>；stdout=[" << capture.out() << "]";
    EXPECT_EQ(error.find("The server closed the connection."), std::string::npos)
        << "连接未建立不得误报为已建立后关闭；stderr=[" << error << "]";

    // 独立探针必须先确认该受控端口确实返回平台拒绝码（Windows 10061、
    // POSIX ECONNREFUSED），再核对应用输出同一诊断行的原因字段。
    const int nativeError = probeConnectErrorCode(port);
    ASSERT_EQ(nativeError, kExpectedConnectRefused)
        << "受控拒绝端口必须返回平台拒绝码；port=" << port;
    const std::string expectedReason = socket_strerror(nativeError);
    ASSERT_FALSE(expectedReason.empty()) << "平台拒绝码必须有错误文本";

    std::string actualReason;
    ASSERT_TRUE(extractFailedConnectReason(error, port, actualReason))
        << "必须有一条 Connection failed. Failed to connect to 127.0.0.1:" << port
        << ": <reason> 诊断行；stderr=[" << error << "]";
    const std::string normalizedActual = normalizeReasonText(actualReason);
    const std::string normalizedExpected = normalizeReasonText(expectedReason);
    EXPECT_NE(normalizedActual.find(normalizedExpected), std::string::npos)
        << "同一诊断行的原因必须与独立探针的平台错误文本相符；expected=["
        << expectedReason << "] actual=[" << actualReason << "] stderr=[" << error << "]";
}

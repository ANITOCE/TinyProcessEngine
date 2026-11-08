// T012：远程启动参数与入口分流契约测试（TDD RED，T018/T024 实现后转绿）。
// 依据：specs/001-remote-connection-phase-01/contracts/cli.md §1/§2/§8、
//       src/startup/startup_remote.hpp、src/startup/startup.hpp。
// 约束：只用 GoogleTest；不监听、不连接、不访问网络/进程/时钟/文件系统。
//
// 骨架期预期 RED：
//   - parseRemoteStartupArgs 恒返回错误 "not implemented" → 解析成功类用例失败；
//   - startup.cpp 尚未识别 --server/--connect → 注入替身的调用次数为 0。
//
// 注入替身一律经 RAII 复位为默认入口（nullptr）；run() 期间写入 stdout/stderr 的
// 内容由 RAII 捕获丢弃，避免污染测试输出（不改变被测语义）。

#include <gtest/gtest.h>

#include "startup.hpp"
#include "startup_remote.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
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

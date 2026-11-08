/**
 * test_remote_command_executor.cpp — 远程 TPE> 命令分类与执行契约测试
 *                                   （T011 RED；T017 实现后转绿）
 *
 * 依据：
 *   - specs/001-remote-connection-phase-01/contracts/cli.md §3 / §4 / §6
 *   - specs/001-remote-connection-phase-01/data-model.md §4
 *   - include/TinyProcessEngine/RemoteCommandExecutor.hpp
 *   - .superpowers/sdd/remote-connection-phase-01/t011-brief.md（lead 范围裁决）
 *
 * 范围：只覆盖 classifyRemoteCommand 与 executeRemoteCommand 的可测路径。
 *   - `--all-processes` 成功输出与枚举失败依赖 ProcessEngine::processListText()
 *     （T013/T016 才引入）→ 不在本任务覆盖，由 T013/T016 覆盖、T030 集成验证。
 *   - 超限 / 不可编码 / “先编码后长度”属于 Client 发送前校验（T027/T028）
 *     → 不在本任务覆盖。
 *
 * 约束：只用 GoogleTest；fake 平台不打开真实进程，不访问网络、时钟或文件系统；
 *       不调用 ProcessEngine::getProcessList()（会打印 stdout）。
 */

#include <gtest/gtest.h>

#include "Platform.hpp"
#include "ProcessEngine.hpp"
#include "RemoteCommandExecutor.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using tpe::PlatformError;
using tpe::ProcessEngine;
using tpe::Result;
using tpe::platform::Pid_t;
using tpe::platform::PlatformOS;
using tpe::platform::PlatformProcess;
using tpe::remote::RemoteCommandKind;
using tpe::remote::RemoteCommandResult;
using tpe::remote::ResponseStatus;

namespace {

/// 最小伪进程：只承载 PID 与名称（空名称 = 平台读不到进程名）。
/// 不提供可扫描内存；getCheatablePages/read/write 一律返回“不支持”，
/// 保证查询类测试不会触碰真实内存。
class FakeQueryProcess : public PlatformProcess {
public:
    FakeQueryProcess(Pid_t pid, std::string name)
        : PlatformProcess(pid, std::move(name))
    {
    }

    std::vector<tpe::MemoryPage> getCheatablePages() const override { return {}; }

    Result<tpe::Memory, PlatformError> read(tpe::MemoryPage) const override
    {
        return Result<tpe::Memory, PlatformError>::error(notSupported());
    }

    Result<void, PlatformError> write(tpe::Address, const tpe::Memory&) override
    {
        return Result<void, PlatformError>::error(notSupported());
    }

private:
    PlatformError notSupported() const
    {
        return PlatformError{"FakeQueryProcess", static_cast<unsigned long>(getPid()), 0,
                             "not supported"};
    }
};

/// 平台替身：按 PID 维护伪进程；`open` 记录调用次数，
/// 用于断言“参数错误在访问引擎/平台之前返回”。
class FakeQueryOS : public PlatformOS {
public:
    std::shared_ptr<FakeQueryProcess> addProcess(Pid_t pid, std::string name)
    {
        auto process = std::make_shared<FakeQueryProcess>(pid, std::move(name));
        ProcessList.push_back(process);
        return process;
    }

    std::shared_ptr<PlatformProcess> open(Pid_t pid) override
    {
        ++m_openCalls;
        for (const auto& process : ProcessList) {
            if (process->getPid() == pid) {
                return process;
            }
        }
        return nullptr; // 不存在的 PID：模拟平台打开失败
    }

    Result<std::vector<Pid_t>, PlatformError> getAllProcessesPid() override
    {
        std::vector<Pid_t> pids;
        for (const auto& process : ProcessList) {
            pids.push_back(process->getPid());
        }
        return Result<std::vector<Pid_t>, PlatformError>::success(std::move(pids));
    }

    void getAllProcesses(std::vector<Pid_t>) override {}

    /// `open` 调用次数；查询参数错误时必须保持 0。
    std::size_t openCalls() const { return m_openCalls; }

private:
    std::size_t m_openCalls = 0;
};

/// 引擎 + fake OS 组合；保留 fake OS 裸指针以断言调用计数。
struct FakeEngine {
    FakeQueryOS* os = nullptr;
    std::unique_ptr<ProcessEngine> engine;
};

FakeEngine makeEngine()
{
    auto os = std::make_unique<FakeQueryOS>();
    FakeQueryOS* const raw = os.get();
    auto engine = std::make_unique<ProcessEngine>(std::move(os));
    return FakeEngine{raw, std::move(engine)};
}

/// 捕获并丢弃 std::cerr：若 T017 经 ProcessEngine::openProcess 查询，
/// 未命中时引擎会向 stderr 打印诊断；该诊断不属于命令 output 契约。
class ScopedCerrSilencer {
public:
    ScopedCerrSilencer() : m_old(std::cerr.rdbuf(m_sink.rdbuf())) {}
    ~ScopedCerrSilencer() { std::cerr.rdbuf(m_old); }
    ScopedCerrSilencer(const ScopedCerrSilencer&) = delete;
    ScopedCerrSilencer& operator=(const ScopedCerrSilencer&) = delete;

private:
    std::ostringstream m_sink;
    std::streambuf* const m_old;
};

/// contracts/cli.md §4 帮助文本正文：4 行，逐行对齐空格与契约一致；
/// 不含 `TPE> help` 回显行，不含尾随空行。
constexpr const char* kHelpText =
    "--all-processes        List processes on the server.\n"
    "--search-process <PID> Show the name of a process on the server.\n"
    "help                   Show this help.\n"
    "exit                   End this client session.";

/// contracts/cli.md §3 未知命令文本（含 --open-process 等未交付命令）。
constexpr const char* kUnknownCommandText =
    "Unknown command. Type 'help' for available commands.";

/// 参数错误只锁定契约 §6 要求的“用法与原因”：输出非空且提到命令名；
/// 具体措辞由 T017 定稿，测试不锁定，避免过度约束。
void expectSearchProcessUsageError(const RemoteCommandResult& result)
{
    EXPECT_EQ(result.status, ResponseStatus::CommandError);
    EXPECT_FALSE(result.output.empty()) << "usage error must explain the problem";
    EXPECT_NE(result.output.find("--search-process"), std::string::npos)
        << "usage error must mention the command, got: " << result.output;
}

RemoteCommandResult execute(std::string_view line, ProcessEngine& engine)
{
    return tpe::remote::executeRemoteCommand(line, engine);
}

std::string searchProcessLine(unsigned long pid)
{
    return "--search-process " + std::to_string(pid);
}

} // namespace

// ---------------------------------------------------------------------------
// 1) 命令分类（contracts/cli.md §3）
// ---------------------------------------------------------------------------

TEST(RemoteCommandExecutor, ClassifyAllProcesses)
{
    EXPECT_EQ(tpe::remote::classifyRemoteCommand("--all-processes"),
              RemoteCommandKind::AllProcesses);
}

TEST(RemoteCommandExecutor, ClassifySearchProcessWithPid)
{
    EXPECT_EQ(tpe::remote::classifyRemoteCommand("--search-process 123"),
              RemoteCommandKind::SearchProcess);
}

TEST(RemoteCommandExecutor, ClassifyHelp)
{
    EXPECT_EQ(tpe::remote::classifyRemoteCommand("help"), RemoteCommandKind::Help);
    EXPECT_EQ(tpe::remote::classifyRemoteCommand("--help"), RemoteCommandKind::Help);
}

TEST(RemoteCommandExecutor, ClassifyExit)
{
    EXPECT_EQ(tpe::remote::classifyRemoteCommand("exit"), RemoteCommandKind::Exit);
}

TEST(RemoteCommandExecutor, ClassifyUndeliveredCommandsAreUnknown)
{
    // Phase 01 未交付的命令与空输入均按未知命令处理（contracts/cli.md §3）。
    const char* const lines[] = {
        "--open-process 1", "new-scan", "next-scan", "list", "write", "undo", "",
    };
    for (const char* line : lines) {
        SCOPED_TRACE(std::string("line: '") + line + "'");
        EXPECT_EQ(tpe::remote::classifyRemoteCommand(line), RemoteCommandKind::Unknown);
    }
}

TEST(RemoteCommandExecutor, ClassifyPaddedCommandsAreUnknown)
{
    // 命令按整行精确匹配：前后带空格的已知命令不是已交付命令。
    const char* const lines[] = {" --all-processes ", "  help  ", "exit "};
    for (const char* line : lines) {
        SCOPED_TRACE(std::string("line: '") + line + "'");
        EXPECT_EQ(tpe::remote::classifyRemoteCommand(line), RemoteCommandKind::Unknown);
    }
}

// ---------------------------------------------------------------------------
// 2) help / --help（contracts/cli.md §4）
// ---------------------------------------------------------------------------

TEST(RemoteCommandExecutor, HelpReturnsContractHelpText)
{
    for (const char* line : {"help", "--help"}) {
        FakeEngine fake = makeEngine();
        SCOPED_TRACE(std::string("line: ") + line);

        const RemoteCommandResult result = execute(line, *fake.engine);

        EXPECT_EQ(result.status, ResponseStatus::Continue);
        EXPECT_EQ(result.output, kHelpText)
            << "help output must match contracts/cli.md §4 exactly (4 lines, "
               "no prompt echo, no trailing blank line)";
    }
}

// ---------------------------------------------------------------------------
// 3) 未知命令（contracts/cli.md §3）
// ---------------------------------------------------------------------------

TEST(RemoteCommandExecutor, UnknownCommandReportsContractText)
{
    for (const char* line : {"--open-process 1", ""}) {
        FakeEngine fake = makeEngine();
        SCOPED_TRACE(std::string("line: '") + line + "'");

        const RemoteCommandResult result = execute(line, *fake.engine);

        EXPECT_EQ(result.status, ResponseStatus::CommandError);
        EXPECT_EQ(result.output, kUnknownCommandText);
    }
}

// ---------------------------------------------------------------------------
// 4) --search-process 参数错误（contracts/cli.md §6）
// ---------------------------------------------------------------------------

TEST(RemoteCommandExecutor, SearchProcessWithoutPidReportsUsageAndSkipsEngine)
{
    FakeEngine fake = makeEngine();
    fake.os->addProcess(static_cast<Pid_t>(4321), "fake.exe");

    const RemoteCommandResult result = execute("--search-process", *fake.engine);

    expectSearchProcessUsageError(result);
    // 缺参数不得访问引擎/平台：fake 的 open 调用计数必须保持 0，
    // 同时不得崩溃（能走到这里即已执行完毕）。
    EXPECT_EQ(fake.os->openCalls(), std::size_t{0});
}

TEST(RemoteCommandExecutor, SearchProcessWithNonNumericPidReportsUsage)
{
    FakeEngine fake = makeEngine();
    fake.os->addProcess(static_cast<Pid_t>(4321), "fake.exe");

    const RemoteCommandResult result = execute("--search-process abc", *fake.engine);

    expectSearchProcessUsageError(result);
    EXPECT_EQ(fake.os->openCalls(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// 5) --search-process 查询失败（contracts/cli.md §6）
// ---------------------------------------------------------------------------

TEST(RemoteCommandExecutor, SearchProcessUnknownPidReportsNotFound)
{
    FakeEngine fake = makeEngine();
    fake.os->addProcess(static_cast<Pid_t>(1111), "other.exe");
    ScopedCerrSilencer silence; // 抑制引擎未命中时可能写往 stderr 的诊断

    const RemoteCommandResult result = execute(searchProcessLine(4321UL), *fake.engine);

    EXPECT_EQ(result.status, ResponseStatus::CommandError);
    EXPECT_EQ(result.output, "Process with PID 4321 not found.");
}

TEST(RemoteCommandExecutor, SearchProcessEmptyNameReportsUnreadableName)
{
    FakeEngine fake = makeEngine();
    // 现有平台接口下“进程名不可读”的约定：getProcessName() 返回空串。
    fake.os->addProcess(static_cast<Pid_t>(4321), "");

    const RemoteCommandResult result = execute(searchProcessLine(4321UL), *fake.engine);

    EXPECT_EQ(result.status, ResponseStatus::CommandError);
    EXPECT_EQ(result.output, "Failed to read the process name for PID 4321.");
}

// ---------------------------------------------------------------------------
// 6) --search-process 成功（contracts/cli.md §3 / §6）
// ---------------------------------------------------------------------------

TEST(RemoteCommandExecutor, SearchProcessReturnsNameOnSingleLine)
{
    FakeEngine fake = makeEngine();
    fake.os->addProcess(static_cast<Pid_t>(4321), "fake.exe");

    const RemoteCommandResult result = execute(searchProcessLine(4321UL), *fake.engine);

    EXPECT_EQ(result.status, ResponseStatus::Continue);
    EXPECT_EQ(result.output, "fake.exe")
        << "successful query output must be the process name only (no prompt, "
           "no trailing newline)";
}

/**
 * test_startup_cli.cpp — CLI 装配层枚举失败信号测试(Phase 05 · US6 · 缺陷⑧)
 *
 * 覆盖(FR-023/FR-024;C-P5):
 *   - 平台枚举失败 → `runCliWithEngine({"--all-processes"}, engine)` 向 stderr
 *     输出 `Failed to enumerate processes: <message>` 并返回 kExitRuntimeError(1),
 *     不得以空列表 + 退出码 0 伪装成功;
 *   - 枚举成功 → 既有 `PID: N ProcessName: X` 输出格式不变且返回 kExitOk(0)。
 *
 * 注入接缝:`ProcessEngine(std::unique_ptr<PlatformOS>)` + `runCliWithEngine`。
 * 本文件在两个平台均编译运行(runCli/runCliWithEngine 为跨平台装配层)。
 */

#include <gtest/gtest.h>

#include "CliParser.hpp"
#include "Platform.hpp"
#include "ProcessEngine.hpp"
#include "startup_cli.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using tpe::MemoryPage;
using tpe::PlatformError;
using tpe::ProcessEngine;
using tpe::Result;
using tpe::platform::Pid_t;
using tpe::platform::PlatformOS;
using tpe::platform::PlatformProcess;

namespace {

/// 伪进程:承载 PID 与名称(列表展示路径用);
/// 可选配置单页可扫描内存(US1 集成驱动:`--unknown` 首扫需读取页面取快照)。
/// 未配置内存时行为与既有实现一致(无页、读写报 not supported),不影响既有 `--all-processes` 用例。
class FakeStartupProcess : public PlatformProcess {
public:
    FakeStartupProcess(Pid_t pid, std::string name)
        : PlatformProcess(pid, std::move(name)) {}

    /// 配置单页可扫描区域(基址 + 大小,字节初值 0)。
    void configureMemory(tpe::Address base, tpe::Size size)
    {
        m_base = base;
        m_size = size;
        m_bytes.assign(static_cast<std::size_t>(size), 0);
        m_hasMemory = true;
    }

    tpe::Memory& bytes() { return m_bytes; }

    std::vector<MemoryPage> getCheatablePages() const override
    {
        if (!m_hasMemory) {
            return {};
        }
        return {MemoryPage(m_base, m_size)};
    }

    Result<tpe::Memory, PlatformError> read(MemoryPage page) const override
    {
        if (!contains(page.start, page.size)) {
            return Result<tpe::Memory, PlatformError>::error(notSupported());
        }
        const std::size_t offset = static_cast<std::size_t>(page.start - m_base);
        return Result<tpe::Memory, PlatformError>::success(
            tpe::Memory(m_bytes.begin() + offset, m_bytes.begin() + offset + page.size));
    }

    Result<void, PlatformError> write(tpe::Address address, const tpe::Memory& value) override
    {
        if (!contains(address, value.size())) {
            return Result<void, PlatformError>::error(notSupported());
        }
        std::copy(value.begin(), value.end(),
                  m_bytes.begin() + static_cast<std::size_t>(address - m_base));
        return Result<void, PlatformError>::success();
    }

private:
    bool contains(tpe::Address address, tpe::Size length) const
    {
        if (!m_hasMemory || address < m_base) {
            return false;
        }
        const tpe::Size offset = address - m_base;
        return offset <= m_size && length <= m_size - offset;
    }

    PlatformError notSupported() const
    {
        return PlatformError{"FakeStartupProcess",
                             static_cast<unsigned long>(getPid()), 0, "not supported"};
    }

    bool m_hasMemory = false;
    tpe::Address m_base = 0;
    tpe::Size m_size = 0;
    tpe::Memory m_bytes;
};

/// 平台替身(US6/缺陷⑧):可配置进程列表 / 枚举错误;`open` 按 PID 返回伪进程。
class FakePlatformOS : public PlatformOS {
public:
    std::shared_ptr<FakeStartupProcess> addProcess(Pid_t pid, std::string name)
    {
        auto process = std::make_shared<FakeStartupProcess>(pid, std::move(name));
        ProcessList.push_back(process);
        return process;
    }

    /// 模拟平台枚举失败:平台实现于失败时填充 enumerationError(缺陷⑧契约)。
    void setEnumerationError(PlatformError err) { m_enumerationError = std::move(err); }

    std::shared_ptr<PlatformProcess> open(Pid_t pid) override
    {
        for (const auto& process : ProcessList) {
            if (process->getPid() == pid) {
                return process;
            }
        }
        return nullptr;
    }

    Result<std::vector<Pid_t>, PlatformError> getAllProcessesPid() override
    {
        if (m_enumerationError.has_value()) {
            return Result<std::vector<Pid_t>, PlatformError>::error(*m_enumerationError);
        }
        std::vector<Pid_t> pids;
        for (const auto& process : ProcessList) {
            pids.push_back(process->getPid());
        }
        return Result<std::vector<Pid_t>, PlatformError>::success(std::move(pids));
    }

    void getAllProcesses(std::vector<Pid_t>) override {}
};

std::vector<std::string> allProcessesArgs()
{
    return {"--all-processes"};
}

/// REPL 脚本驱动:`--open-process <pid>` 进入 REPL,逐行喂入脚本,
/// 捕获 stdout 并返回(输出, 退出码)。
struct ReplRun {
    int rc = 0;
    std::string output;
};

ReplRun runReplScript(Pid_t pid, const std::string& script, ProcessEngine& engine)
{
    std::ostringstream capturedOut;
    std::istringstream input(script);

    std::streambuf* const oldOut = std::cout.rdbuf(capturedOut.rdbuf());
    std::streambuf* const oldIn = std::cin.rdbuf(input.rdbuf());
    const int rc = tpe::app::runCliWithEngine(
        {"--open-process", std::to_string(static_cast<unsigned long>(pid))}, engine);
    std::cin.rdbuf(oldIn);
    std::cout.rdbuf(oldOut);

    return ReplRun{rc, capturedOut.str()};
}

} // namespace

/// FR-024/C-P5:枚举失败 → stderr 明确失败信息 + 运行期失败退出码(1)。
TEST(RunCliAllProcessesTest, RunCliAllProcessesFailsWithRuntimeError)
{
    auto os = std::make_unique<FakePlatformOS>();
    os->setEnumerationError(
        PlatformError{"EnumProcesses", 0, 5, "EnumProcesses failed (simulated)"});
    ProcessEngine engine(std::move(os));

    std::ostringstream capturedErr;
    std::streambuf* const oldErr = std::cerr.rdbuf(capturedErr.rdbuf());
    const int rc = tpe::app::runCliWithEngine(allProcessesArgs(), engine);
    std::cerr.rdbuf(oldErr);

    EXPECT_EQ(rc, tpe::cli::kExitRuntimeError);
    EXPECT_NE(capturedErr.str().find("Failed to enumerate processes"), std::string::npos)
        << "stderr must carry a clear enumeration failure message, got: "
        << capturedErr.str();
}

/// FR-024/C-P5:枚举成功 → 既有输出格式 + 退出码 0(成功但空列表亦为合法结果)。
TEST(RunCliAllProcessesTest, RunCliAllProcessesSucceedsWithFakeList)
{
    auto os = std::make_unique<FakePlatformOS>();
    os->addProcess(static_cast<Pid_t>(4242), "fake.exe");
    ProcessEngine engine(std::move(os));

    std::ostringstream capturedOut;
    std::streambuf* const oldOut = std::cout.rdbuf(capturedOut.rdbuf());
    const int rc = tpe::app::runCliWithEngine(allProcessesArgs(), engine);
    std::cout.rdbuf(oldOut);

    EXPECT_EQ(rc, tpe::cli::kExitOk);
    EXPECT_EQ(capturedOut.str(), "PID: 4242 ProcessName: fake.exe\n");
}

// ---------------------------------------------------------------------------
// US1(Phase 07 · T005 红)— `new-scan --unknown` REPL 集成驱动
//   C-D1 / FR-001–006:执行真实首扫(不再占位);结果可 list(Total 计数);
//   提示符切换为 `<名>-unknown-<类型>`(无值段);退出码 0。
// ---------------------------------------------------------------------------

TEST(RunCliReplUnknownScan, NewScanUnknownExecutesRealFirstScan)
{
    auto os = std::make_unique<FakePlatformOS>();
    auto process = os->addProcess(static_cast<Pid_t>(4242), "fake.exe");
    process->configureMemory(0x1000, 0x40); // 64 字节单页 → i32 候选 16 个
    ProcessEngine engine(std::move(os));

    const ReplRun run = runReplScript(static_cast<Pid_t>(4242),
                                      "new-scan --unknown\nlist --all\nexit\n", engine);

    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_EQ(run.output.find("not implemented"), std::string::npos)
        << "user-visible output must not contain placeholder text, got:\n" << run.output;
    EXPECT_NE(run.output.find("Total: 16 matches"), std::string::npos)
        << "list --all must show candidate total for the first scan, got:\n" << run.output;
    EXPECT_NE(run.output.find("-unknown-"), std::string::npos)
        << "prompt must switch to the unknown scan type, got:\n" << run.output;
}

// ---------------------------------------------------------------------------
// US2(Phase 07 · T009 红)— `new-scan --greater / --less` REPL 集成驱动
//   C-D2/C-D3 / FR-007–011:首轮严格 GT/LT(相等不保留)、提示符带值段、
//   缺值/非法值用法错误状态不变、零匹配会话可用、全程无占位文案、退出码 0。
// ---------------------------------------------------------------------------

namespace {

/// 在伪进程内存缓冲的页内偏移处写入小端 i32(测试内存布置用)。
void putI32(tpe::Memory& bytes, std::size_t offset, std::int32_t value)
{
    const std::uint32_t raw = static_cast<std::uint32_t>(value);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<tpe::Byte>((raw >> (8 * i)) & 0xFFu);
    }
}

/// 布置“比较边界页”:64 字节单页、16 个 i32 槽位——
/// 0x1000=50 / 0x1004=99 / 0x1008=100 / 0x100C=101 / 0x1010=200,其余槽位=100。
/// `--greater 100` → 仅 101、200(2 条);`--less 100` → 仅 50、99(2 条);==100 一律不保留。
void configureComparisonLayout(FakeStartupProcess& process)
{
    for (std::size_t slot = 0; slot < 16; ++slot) {
        putI32(process.bytes(), slot * 4, 100);
    }
    putI32(process.bytes(), 0x00, 50);
    putI32(process.bytes(), 0x04, 99);
    putI32(process.bytes(), 0x0C, 101);
    putI32(process.bytes(), 0x10, 200);
}

} // namespace

TEST(RunCliReplComparisonScan, GreaterScanKeepsStrictlyGreaterAndShowsValuePrompt)
{
    auto os = std::make_unique<FakePlatformOS>();
    auto process = os->addProcess(static_cast<Pid_t>(4242), "fake.exe");
    process->configureMemory(0x1000, 0x40);
    configureComparisonLayout(*process);
    ProcessEngine engine(std::move(os));

    const ReplRun run = runReplScript(static_cast<Pid_t>(4242),
                                      "new-scan --greater 100\nlist --all\nexit\n", engine);

    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_EQ(run.output.find("not implemented"), std::string::npos)
        << "user-visible output must not contain placeholder text, got:\n" << run.output;
    EXPECT_NE(run.output.find("-greater-i32-100"), std::string::npos)
        << "prompt must show greater scan type and comparison value, got:\n" << run.output;
    EXPECT_NE(run.output.find("Total: 2 matches"), std::string::npos)
        << "strict GT must keep exactly the two >100 values, got:\n" << run.output;
    EXPECT_NE(run.output.find("0x000000000000100C | 101"), std::string::npos) << run.output;
    EXPECT_NE(run.output.find("0x0000000000001010 | 200"), std::string::npos) << run.output;
    EXPECT_EQ(run.output.find(" | 100"), std::string::npos)
        << "value == 100 must not be kept (strict GT), got:\n" << run.output;
    EXPECT_EQ(run.output.find(" | 99"), std::string::npos) << run.output;
    EXPECT_EQ(run.output.find(" | 50"), std::string::npos) << run.output;
}

TEST(RunCliReplComparisonScan, LessScanKeepsStrictlyLessAndShowsValuePrompt)
{
    auto os = std::make_unique<FakePlatformOS>();
    auto process = os->addProcess(static_cast<Pid_t>(4242), "fake.exe");
    process->configureMemory(0x1000, 0x40);
    configureComparisonLayout(*process);
    ProcessEngine engine(std::move(os));

    const ReplRun run = runReplScript(static_cast<Pid_t>(4242),
                                      "new-scan --less 100\nlist --all\nexit\n", engine);

    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_EQ(run.output.find("not implemented"), std::string::npos)
        << "user-visible output must not contain placeholder text, got:\n" << run.output;
    EXPECT_NE(run.output.find("-less-i32-100"), std::string::npos)
        << "prompt must show less scan type and comparison value, got:\n" << run.output;
    EXPECT_NE(run.output.find("Total: 2 matches"), std::string::npos)
        << "strict LT must keep exactly the two <100 values, got:\n" << run.output;
    EXPECT_NE(run.output.find("0x0000000000001000 | 50"), std::string::npos) << run.output;
    EXPECT_NE(run.output.find("0x0000000000001004 | 99"), std::string::npos) << run.output;
    EXPECT_EQ(run.output.find(" | 100"), std::string::npos)
        << "value == 100 must not be kept (strict LT), got:\n" << run.output;
    EXPECT_EQ(run.output.find(" | 101"), std::string::npos) << run.output;
    EXPECT_EQ(run.output.find(" | 200"), std::string::npos) << run.output;
}

TEST(RunCliReplComparisonScan, MissingValueIsUsageErrorAndStateUnchanged)
{
    // 先建立一次真实会话与提示符状态(--unknown),再验证缺值不执行、不改变状态
    auto os = std::make_unique<FakePlatformOS>();
    auto process = os->addProcess(static_cast<Pid_t>(4242), "fake.exe");
    process->configureMemory(0x1000, 0x40);
    ProcessEngine engine(std::move(os));

    const ReplRun run = runReplScript(
        static_cast<Pid_t>(4242),
        "new-scan --unknown\nnew-scan --greater\nlist --all\nexit\n", engine);

    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_EQ(run.output.find("not implemented"), std::string::npos)
        << "user-visible output must not contain placeholder text, got:\n" << run.output;
    EXPECT_EQ(run.output.find("-greater-"), std::string::npos)
        << "missing value must not switch the scan type, got:\n" << run.output;
    EXPECT_NE(run.output.find("fake.exe-unknown-i32> Missing value for new-scan."),
              std::string::npos)
        << "usage error must keep the previous prompt state, got:\n" << run.output;
    EXPECT_NE(run.output.find("Usage: new-scan"), std::string::npos) << run.output;
    EXPECT_NE(run.output.find("Total: 16 matches"), std::string::npos)
        << "previous session must stay intact (no scan executed), got:\n" << run.output;
}

TEST(RunCliReplComparisonScan, InvalidValueIsRejectedAndStateUnchanged)
{
    auto os = std::make_unique<FakePlatformOS>();
    auto process = os->addProcess(static_cast<Pid_t>(4242), "fake.exe");
    process->configureMemory(0x1000, 0x40);
    ProcessEngine engine(std::move(os));

    const ReplRun run = runReplScript(
        static_cast<Pid_t>(4242),
        "new-scan --unknown\nnew-scan --greater abc\nlist --all\nexit\n", engine);

    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_EQ(run.output.find("not implemented"), std::string::npos)
        << "user-visible output must not contain placeholder text, got:\n" << run.output;
    EXPECT_EQ(run.output.find("-greater-"), std::string::npos)
        << "invalid value must not switch the scan type, got:\n" << run.output;
    EXPECT_NE(run.output.find("fake.exe-unknown-i32> Invalid value for i32:"),
              std::string::npos)
        << "invalid value must reuse the equal-scan error style and keep state, got:\n"
        << run.output;
    EXPECT_NE(run.output.find("Usage: new-scan"), std::string::npos) << run.output;
    EXPECT_NE(run.output.find("Total: 16 matches"), std::string::npos)
        << "previous session must stay intact (no scan executed), got:\n" << run.output;
}

TEST(RunCliReplComparisonScan, ZeroMatchesLeavesUsableSession)
{
    // 空结果契约(list 于 total==0):逐字 `No matches to display.`(无 Total 行);
    // 会话保持可用:随后 `new-scan 100` 正常执行
    auto os = std::make_unique<FakePlatformOS>();
    auto process = os->addProcess(static_cast<Pid_t>(4242), "fake.exe");
    process->configureMemory(0x1000, 0x40);
    for (std::size_t slot = 0; slot < 16; ++slot) {
        putI32(process->bytes(), slot * 4, 100);
    }
    ProcessEngine engine(std::move(os));

    const ReplRun run = runReplScript(
        static_cast<Pid_t>(4242),
        "new-scan --greater 2147483647\nlist --all\nnew-scan 100\nlist --all\nexit\n", engine);

    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_EQ(run.output.find("not implemented"), std::string::npos)
        << "user-visible output must not contain placeholder text, got:\n" << run.output;
    EXPECT_NE(run.output.find("-greater-i32-2147483647"), std::string::npos)
        << "prompt must carry the extreme comparison value, got:\n" << run.output;
    EXPECT_NE(run.output.find("No matches to display."), std::string::npos)
        << "empty result set must produce the existing empty-result message, got:\n"
        << run.output;
    EXPECT_EQ(run.output.find("No scan results available"), std::string::npos)
        << "zero-match scan must still create a usable session, got:\n" << run.output;
    EXPECT_NE(run.output.find("Total: 16 matches"), std::string::npos)
        << "follow-up equal scan must work in the same session, got:\n" << run.output;
    EXPECT_NE(run.output.find("-equal-i32-100"), std::string::npos) << run.output;
}

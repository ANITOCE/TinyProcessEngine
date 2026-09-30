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
#include "ScanSession.hpp"
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

    /// 标记字节区间不可读(US3/T016b:模拟扫描后页已释放——`list` 实时读取该地址失败);
    /// 仅影响 read 路径(扫描前配置会使整页读取失败;用例按需在扫描后调用)。
    void setRangeUnreadable(tpe::Address address, tpe::Size length)
    {
        m_unreadable.emplace_back(address, length);
    }

    std::vector<MemoryPage> getCheatablePages() const override
    {
        if (!m_hasMemory) {
            return {};
        }
        return {MemoryPage(m_base, m_size)};
    }

    Result<tpe::Memory, PlatformError> read(MemoryPage page) const override
    {
        if (!contains(page.start, page.size) || overlapsUnreadable(page)) {
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

    /// 读取页与任一不可读区间重叠 → 读取失败(page [start,+size) 与区间 [start,+len) 相交)。
    bool overlapsUnreadable(const MemoryPage& page) const
    {
        for (const MemoryPage& range : m_unreadable) {
            if (page.start < range.start + range.size && range.start < page.start + page.size) {
                return true;
            }
        }
        return false;
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
    std::vector<MemoryPage> m_unreadable; // 不可读字节区间(US3/T016b)
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

/// US4(T022/T024):REPL 输出不得含任何实现状态说明(占位机制已退役)。
/// 按 "implemented" 词根作更宽覆盖——涵盖占位语及一切实现状态说明变体。
void expectNoImplementationWording(const std::string& output)
{
    EXPECT_EQ(output.find("implemented"), std::string::npos)
        << "user-visible output must not contain implementation-status wording, got:\n"
        << output;
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
    expectNoImplementationWording(run.output);
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
    expectNoImplementationWording(run.output);
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
    expectNoImplementationWording(run.output);
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
    expectNoImplementationWording(run.output);
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
    expectNoImplementationWording(run.output);
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
    expectNoImplementationWording(run.output);
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

// ---------------------------------------------------------------------------
// 跨故事一致性规则(Phase 07 · T013 红)— FR-026 / FR-027(C-D4 / C-D5)
//   ① 类型一致性(FR-026/C-D4):会话数值类型仅由 `new-scan` 设定;`next-scan`
//      传同类旗标 = 无操作(正常执行),传异类旗标 = 用法错误(原因句 + Usage,
//      不执行、不改变会话状态)。现行为:类型旗标可任意切换 —— 红。
//   ② string 仅等值扫描(FR-012/FR-027/C-D5):`new-scan --unknown/--greater/--less`
//      与 `--string` 组合(旗标或会话继承)、`next-scan --changed/--unchanged/
//      --greater/--less` 在 string 会话中 = 用法错误显式拒绝;
//      现行为:静默 0 结果(不拒绝)—— 红。
// ---------------------------------------------------------------------------

namespace {

/// T013 一致性布局:64 字节单页——0x00/0x04 各一个 i32 = 100(等值首扫 2 命中);
/// 0x10 起 5 字节 "hello"(string 等值首扫 1 命中);其余字节为 0。
void configureConsistencyLayout(FakeStartupProcess& process)
{
    putI32(process.bytes(), 0x00, 100);
    putI32(process.bytes(), 0x04, 100);
    const char kHello[] = "hello";
    for (std::size_t i = 0; i < sizeof(kHello) - 1; ++i) {
        process.bytes()[0x10 + i] = static_cast<tpe::Byte>(kHello[i]);
    }
}

/// 跨故事一致性用例驱动:新建伪进程(一致性布局)+ 引擎,运行 REPL 脚本。
ReplRun runConsistencyScript(const std::string& script)
{
    auto os = std::make_unique<FakePlatformOS>();
    auto process = os->addProcess(static_cast<Pid_t>(4242), "fake.exe");
    process->configureMemory(0x1000, 0x40);
    configureConsistencyLayout(*process);
    ProcessEngine engine(std::move(os));
    return runReplScript(static_cast<Pid_t>(4242), script, engine);
}

} // namespace

TEST(RunCliReplConsistency, NextScanTypeMismatchIsRejectedAndStateUnchanged)
{
    // FR-026/C-D4:会话 i32;先经 `next-scan --i32 --greater`(同类旗标、正常执行;
    // 相对快照 0 命中但保留值段 `100`,并建立单级 undo 栈),再输入
    // `next-scan --i16 --changed` → 原因句+Usage;不执行:提示符(含 lastValue 段)、
    // 匹配集与 undo 栈全部保持。
    const ReplRun run = runConsistencyScript(
        "new-scan 100\n"
        "next-scan --i32 --greater\n"
        "next-scan --i16 --changed\n"
        "list --all\n"
        "undo\n"
        "list --all\n"
        "exit\n");

    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_NE(run.output.find("fake.exe-greater-i32-100> next-scan cannot change the value type "
                              "(current: i32, requested: i16).\nUsage: next-scan"),
              std::string::npos)
        << "type mismatch must print the fixed reason sentence + usage on the next line, got:\n"
        << run.output;
    EXPECT_EQ(run.output.find("-changed-i16"), std::string::npos)
        << "rejected command must not switch the session value type, got:\n" << run.output;
    EXPECT_NE(run.output.find("fake.exe-greater-i32-100> No matches to display."), std::string::npos)
        << "rejection must not execute a scan (previous round's result set intact), got:\n"
        << run.output;
    EXPECT_NE(run.output.find("fake.exe-greater-i32-100> Total: 2 matches"), std::string::npos)
        << "undo after rejection must restore the pre-next-scan round (2 matches), got:\n"
        << run.output;
}

TEST(RunCliReplConsistency, NextScanSameTypeFlagExecutesNormally)
{
    // FR-026/C-D4:同类旗标为无操作——正常执行。静态内存下 changed 过滤为 0 条,
    // 提示符切换为 changed-i32;全程无拒绝文案。
    const ReplRun run = runConsistencyScript(
        "new-scan 100\n"
        "next-scan --i32 --changed\n"
        "list --all\n"
        "exit\n");

    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_EQ(run.output.find("cannot change the value type"), std::string::npos) << run.output;
    EXPECT_EQ(run.output.find("Usage: next-scan"), std::string::npos) << run.output;
    EXPECT_NE(run.output.find("-changed-i32"), std::string::npos)
        << "same-type flag must execute normally and switch scan type, got:\n" << run.output;
    EXPECT_NE(run.output.find("No matches to display."), std::string::npos)
        << "static memory must yield zero changed results (normal execution), got:\n"
        << run.output;
}

TEST(RunCliReplConsistency, StringSessionRejectsNonEqualNextScanConditions)
{
    // FR-027/C-D5:string 会话下四个非等值条件一律用法错误显式拒绝(含 Usage;
    // 不得以空结果替代);拒绝后提示符(含 lastValue)与匹配集不变。
    struct Case {
        const char* command;
        const char* flag;
    };
    const Case cases[] = {
        {"next-scan --changed", "--changed"},
        {"next-scan --unchanged", "--unchanged"},
        {"next-scan --greater", "--greater"},
        {"next-scan --less", "--less"},
    };

    for (const Case& one : cases) {
        SCOPED_TRACE(one.flag);
        const ReplRun run = runConsistencyScript(std::string("new-scan --string hello\n") +
                                                 one.command + "\nlist --all\nexit\n");

        EXPECT_EQ(run.rc, tpe::cli::kExitOk);
        EXPECT_NE(run.output.find(std::string("Scan type ") + one.flag +
                                  " does not support --string.\nUsage: next-scan"),
                  std::string::npos)
            << "string session must explicitly reject " << one.flag
            << " with the fixed reason + usage, got:\n" << run.output;
        EXPECT_EQ(run.output.find(std::string("-") + (one.flag + 2) + "-"), std::string::npos)
            << "rejected command must not switch the scan type, got:\n" << run.output;
        EXPECT_NE(run.output.find("fake.exe-equal-string-hello> Total: 1 matches"),
                  std::string::npos)
            << "prompt, lastValue and result set must stay unchanged after rejection, got:\n"
            << run.output;
    }
}

TEST(RunCliReplConsistency, StringEqualPathKeepsWorking)
{
    // FR-027/C-D5:`--string` + `--equal`(next-scan 显式 --string)维持现状可用——
    // 正常执行、保留原匹配、无任何拒绝文案。
    const ReplRun run = runConsistencyScript(
        "new-scan --string hello\n"
        "next-scan --string hello\n"
        "list --all\n"
        "exit\n");

    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_EQ(run.output.find("does not support --string."), std::string::npos) << run.output;
    EXPECT_EQ(run.output.find("cannot change the value type"), std::string::npos) << run.output;
    EXPECT_EQ(run.output.find("No matches to display."), std::string::npos)
        << "string equal chain must keep the match, got:\n" << run.output;
    EXPECT_NE(run.output.find("-equal-string-hello"), std::string::npos) << run.output;
    EXPECT_NE(run.output.find("Total: 1 matches"), std::string::npos)
        << "string equal chain must keep the match, got:\n" << run.output;
}

TEST(RunCliReplConsistency, NewScanGreaterWithStringFlagIsRejected)
{
    // FR-012/FR-027/C-D5:显式旗标组合 `new-scan --greater --string 5` → 用法错误
    // (原因句+Usage);不执行、不改变会话状态(既有 equal/i32 会话原样保留)。
    const ReplRun run = runConsistencyScript(
        "new-scan 100\n"
        "new-scan --greater --string 5\n"
        "list --all\n"
        "exit\n");

    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_NE(run.output.find("Scan type --greater does not support --string.\nUsage: new-scan"),
              std::string::npos)
        << "explicit --string combination must be rejected with reason + usage, got:\n"
        << run.output;
    EXPECT_EQ(run.output.find("-greater-"), std::string::npos)
        << "rejected command must not switch the scan type, got:\n" << run.output;
    EXPECT_NE(run.output.find("fake.exe-equal-i32-100> Total: 2 matches"), std::string::npos)
        << "previous session state must stay intact, got:\n" << run.output;
}

TEST(RunCliReplConsistency, NewScanGreaterInStringSessionIsRejected)
{
    // FR-012/FR-027/C-D5:会话继承来源——string 会话输入 `new-scan --greater 5`
    // (命令未带 --string 旗标,当前类型仍为 string)→ 同款拒绝;状态不变。
    const ReplRun run = runConsistencyScript(
        "new-scan --string hello\n"
        "new-scan --greater 5\n"
        "list --all\n"
        "exit\n");

    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_NE(run.output.find("Scan type --greater does not support --string.\nUsage: new-scan"),
              std::string::npos)
        << "session-inherited string type must be rejected with reason + usage, got:\n"
        << run.output;
    EXPECT_EQ(run.output.find("-greater-"), std::string::npos)
        << "rejected command must not switch the scan type, got:\n" << run.output;
    EXPECT_NE(run.output.find("fake.exe-equal-string-hello> Total: 1 matches"), std::string::npos)
        << "string session state must stay intact, got:\n" << run.output;
}

// ---------------------------------------------------------------------------
// US3(Phase 07 · T016 红)— `list` 实时重读 REPL 集成驱动
//   C-D6/E4/INV-L(FR-013–018):值列 = 展示时读取的当前内存值(非扫描快照);
//   不可读 / 零宽 → 值列逐字 `??`(行数与 `Total` 不变、列表不中断);
//   `write` 后 `list` 反映新值。现行为:显示扫描时快照、(b)(d) 无 `??` 路径 —— 红。
//   说明:目标侧变化无法由 REPL 命令触发,(a)(b)(d) 分值两段运行——第一段扫描后退出,
//   随后直接操作伪进程内存,第二段(同一引擎、会话保持)执行 `list`。
// ---------------------------------------------------------------------------

TEST(RunCliReplListLive, ListShowsCurrentMemoryValueNotSnapshot)
{
    // (a) 实时值 vs 快照:扫描(快照=100)→ 目标内存变化(104)→ `list --all` 显示 104。
    auto os = std::make_unique<FakePlatformOS>();
    auto process = os->addProcess(static_cast<Pid_t>(4242), "fake.exe");
    process->configureMemory(0x1000, 0x40);
    putI32(process->bytes(), 0x00, 100);
    ProcessEngine engine(std::move(os));

    const ReplRun scan = runReplScript(static_cast<Pid_t>(4242),
                                       "new-scan 100\nlist --all\nexit\n", engine);
    ASSERT_EQ(scan.rc, tpe::cli::kExitOk);
    ASSERT_NE(scan.output.find("Total: 1 matches"), std::string::npos) << scan.output;
    ASSERT_NE(scan.output.find("0x0000000000001000 | 100"), std::string::npos)
        << "scan-time display must show the current value 100, got:\n" << scan.output;

    putI32(process->bytes(), 0x00, 104); // 扫描之后:目标进程自身变化

    const ReplRun run = runReplScript(static_cast<Pid_t>(4242), "list --all\nexit\n", engine);
    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_NE(run.output.find("Total: 1 matches"), std::string::npos) << run.output;
    EXPECT_NE(run.output.find("0x0000000000001000 | 104"), std::string::npos)
        << "list must show the live value read at display time, got:\n" << run.output;
    EXPECT_EQ(run.output.find("0x0000000000001000 | 100"), std::string::npos)
        << "stale scan snapshot must not be displayed, got:\n" << run.output;
}

TEST(RunCliReplListLive, UnreadableAddressShowsLiteralPlaceholderKeepingRowsAndTotal)
{
    // (b) 不可读 → `??`:0x1000 / 0x1004 均为 i32 100(等值首扫 2 命中);扫描后仅
    // 0x1000 置不可读 → 该行值列逐字 `??`;行数与 `Total` 不变,0x1004 行照常(不中断)。
    auto os = std::make_unique<FakePlatformOS>();
    auto process = os->addProcess(static_cast<Pid_t>(4242), "fake.exe");
    process->configureMemory(0x1000, 0x40);
    putI32(process->bytes(), 0x00, 100);
    putI32(process->bytes(), 0x04, 100);
    ProcessEngine engine(std::move(os));

    const ReplRun scan = runReplScript(static_cast<Pid_t>(4242), "new-scan 100\nexit\n", engine);
    ASSERT_EQ(scan.rc, tpe::cli::kExitOk);

    process->setRangeUnreadable(0x1000, 4); // 扫描后:页已释放(仅 0x1000 不可读)

    const ReplRun run = runReplScript(static_cast<Pid_t>(4242), "list --all\nexit\n", engine);
    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_NE(run.output.find("Total: 2 matches"), std::string::npos)
        << "Total must keep the match count regardless of read failures, got:\n" << run.output;
    EXPECT_NE(run.output.find("0x0000000000001000 | ??"), std::string::npos)
        << "unreadable row must show the literal ?? placeholder, got:\n" << run.output;
    EXPECT_NE(run.output.find("0x0000000000001004 | 100"), std::string::npos)
        << "later rows must still be shown (list must not abort), got:\n" << run.output;

    std::size_t rowCount = 0;
    for (std::size_t pos = 0; (pos = run.output.find("  0x", pos)) != std::string::npos; ++pos) {
        ++rowCount;
    }
    EXPECT_EQ(rowCount, 2u) << "row count must stay unchanged, got:\n" << run.output;
}

TEST(RunCliReplListLive, WriteThenListReflectsWrittenValue)
{
    // (c) write → list:写入 200 后对应行显示 200(FR-017;与快照 100 可区分)。
    auto os = std::make_unique<FakePlatformOS>();
    auto process = os->addProcess(static_cast<Pid_t>(4242), "fake.exe");
    process->configureMemory(0x1000, 0x40);
    putI32(process->bytes(), 0x00, 100);
    putI32(process->bytes(), 0x04, 7);
    ProcessEngine engine(std::move(os));

    const ReplRun run = runReplScript(static_cast<Pid_t>(4242),
                                      "new-scan 100\nwrite 0x1000 200\nlist --all\nexit\n", engine);

    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_NE(run.output.find("Total: 1 matches"), std::string::npos) << run.output;
    EXPECT_NE(run.output.find("0x0000000000001000 | 200"), std::string::npos)
        << "list after write must reflect the written value, got:\n" << run.output;
    EXPECT_EQ(run.output.find("0x0000000000001000 | 100"), std::string::npos)
        << "stale snapshot must not be displayed after write, got:\n" << run.output;
}

TEST(RunCliReplListLive, ZeroWidthSnapshotRecordShowsPlaceholder)
{
    // (d) 零宽快照(snapshot_size == 0)→ 视为不可读,值列 `??`。
    // 该形态 CLI 命令无法产生(E3:本 Phase 新路径不产生无快照记录),经公开会话 API
    // 直接提交注入,锁定展示层边界规则。
    auto os = std::make_unique<FakePlatformOS>();
    auto process = os->addProcess(static_cast<Pid_t>(4242), "fake.exe");
    process->configureMemory(0x1000, 0x40);
    putI32(process->bytes(), 0x00, 100);
    ProcessEngine engine(std::move(os));

    const ReplRun scan = runReplScript(static_cast<Pid_t>(4242), "new-scan 100\nexit\n", engine);
    ASSERT_EQ(scan.rc, tpe::cli::kExitOk);

    tpe::ScanSession* session = engine.session();
    ASSERT_NE(session, nullptr);
    session->commitFirstScan({tpe::ScanRecord(0x1000)}); // snapshot_size == 0

    const ReplRun run = runReplScript(static_cast<Pid_t>(4242), "list --all\nexit\n", engine);
    EXPECT_EQ(run.rc, tpe::cli::kExitOk);
    EXPECT_NE(run.output.find("Total: 1 matches"), std::string::npos) << run.output;
    EXPECT_NE(run.output.find("0x0000000000001000 | ??"), std::string::npos)
        << "zero-width snapshot record must show the literal ?? placeholder, got:\n"
        << run.output;
}

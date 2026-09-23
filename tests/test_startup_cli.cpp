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

#include "CliParser.h"
#include "Platform.h"
#include "ProcessEngine.h"
#include "startup_cli.h"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

/// 伪进程:仅承载 PID 与名称(列表展示路径用)。
class FakeStartupProcess : public PlatformProcess {
public:
    FakeStartupProcess(Pid_t pid, std::string name)
        : PlatformProcess(pid, std::move(name)) {}

    std::vector<MemoryPage> getCheatablePages() const override { return {}; }

    Result<tpe::Memory, PlatformError> read(MemoryPage) const override
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
        return PlatformError{"FakeStartupProcess",
                             static_cast<unsigned long>(getPid()), 0, "not supported"};
    }
};

/// 平台替身(US6/缺陷⑧):可配置进程列表 / 枚举错误。
class FakePlatformOS : public PlatformOS {
public:
    void addProcess(Pid_t pid, std::string name)
    {
        ProcessList.push_back(std::make_shared<FakeStartupProcess>(pid, std::move(name)));
    }

    /// 模拟平台枚举失败:平台实现于失败时填充 enumerationError(缺陷⑧契约)。
    void setEnumerationError(PlatformError err) { m_enumerationError = std::move(err); }

    std::shared_ptr<PlatformProcess> open(Pid_t) override { return nullptr; }

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

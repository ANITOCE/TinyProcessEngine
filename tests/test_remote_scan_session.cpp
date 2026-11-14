#include <gtest/gtest.h>

#include "Platform.hpp"
#include "ProcessEngine.hpp"
#include "RemoteCommandExecutor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tpe::Memory;
using tpe::PlatformError;
using tpe::ProcessEngine;
using tpe::Result;
using tpe::platform::Pid_t;
using tpe::platform::PlatformOS;
using tpe::platform::PlatformProcess;
using tpe::remote::RemoteCommandExecutor;
using tpe::remote::RemoteLayer;
using tpe::remote::ResponseStatus;

class FakeMemoryProcess : public PlatformProcess {
public:
    FakeMemoryProcess(Pid_t pid, std::string name, std::vector<tpe::Byte> bytes)
        : PlatformProcess(pid, std::move(name))
        , m_bytes(std::move(bytes))
    {
    }

    std::vector<tpe::MemoryPage> getCheatablePages() const override
    {
        if (m_bytes.empty()) {
            return {};
        }
        return {tpe::MemoryPage{0x1000, m_bytes.size()}};
    }

    Result<Memory, PlatformError> read(tpe::MemoryPage page) const override
    {
        if (page.start < 0x1000 || page.start + page.size > 0x1000 + m_bytes.size()) {
            return Result<Memory, PlatformError>::error(
                PlatformError{"read", static_cast<unsigned long>(getPid()), 0, "bad page"});
        }
        const std::size_t offset = static_cast<std::size_t>(page.start - 0x1000);
        return Result<Memory, PlatformError>::success(
            Memory(m_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                   m_bytes.begin() + static_cast<std::ptrdiff_t>(offset + page.size)));
    }

    Result<void, PlatformError> write(tpe::Address address, const Memory& value) override
    {
        if (address < 0x1000 || address + value.size() > 0x1000 + m_bytes.size()) {
            return Result<void, PlatformError>::error(
                PlatformError{"write", static_cast<unsigned long>(getPid()), 0, "bad address"});
        }
        const std::size_t offset = static_cast<std::size_t>(address - 0x1000);
        std::copy(value.begin(), value.end(), m_bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        return Result<void, PlatformError>::success();
    }

    bool isAlive() const override { return m_alive; }
    void setAlive(bool alive) { m_alive = alive; }

private:
    std::vector<tpe::Byte> m_bytes;
    bool m_alive = true;
};

class FakeMemoryOS : public PlatformOS {
public:
    std::shared_ptr<FakeMemoryProcess> addProcess(Pid_t pid, std::string name,
                                                  std::vector<tpe::Byte> bytes)
    {
        auto process = std::make_shared<FakeMemoryProcess>(pid, std::move(name), std::move(bytes));
        ProcessList.push_back(process);
        return process;
    }

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
        std::vector<Pid_t> pids;
        for (const auto& process : ProcessList) {
            pids.push_back(process->getPid());
        }
        return Result<std::vector<Pid_t>, PlatformError>::success(std::move(pids));
    }

    void getAllProcesses(std::vector<Pid_t>) override {}
};

} // namespace

TEST(RemoteScanSession, OpenScanListWriteAndExit)
{
    auto os = std::make_unique<FakeMemoryOS>();
    FakeMemoryOS* raw = os.get();
    std::vector<tpe::Byte> bytes(64, 0);
    const std::int32_t initial = 100;
    std::memcpy(bytes.data(), &initial, sizeof(initial));
    auto process = raw->addProcess(4321, "mem.exe", std::move(bytes));
    ASSERT_NE(process, nullptr);
    ProcessEngine engine(std::move(os));
    RemoteCommandExecutor executor(engine);

    const auto opened = executor.execute("--open-process 4321");
    EXPECT_EQ(opened.status, ResponseStatus::Continue);
    EXPECT_EQ(executor.layer(), RemoteLayer::Scan);
    EXPECT_EQ(executor.prompt(), "mem.exe-equal-i32>");

    const auto first = executor.execute("new-scan 100");
    EXPECT_EQ(first.status, ResponseStatus::Continue);
    EXPECT_EQ(executor.prompt(), "mem.exe-equal-i32-100>");

    const auto next = executor.execute("next-scan --unchanged");
    EXPECT_EQ(next.status, ResponseStatus::Continue);
    EXPECT_EQ(executor.prompt(), "mem.exe-unchanged-i32>");

    const auto listed = executor.execute("list --all");
    EXPECT_EQ(listed.status, ResponseStatus::Continue);
    EXPECT_NE(listed.output.find("0x0000000000001000"), std::string::npos);
    EXPECT_NE(listed.output.find("100"), std::string::npos);

    const auto written = executor.execute("write 0x1000 200");
    EXPECT_EQ(written.status, ResponseStatus::Continue);
    EXPECT_EQ(written.output, "Wrote 200 to 0x0000000000001000.");

    const auto after = executor.execute("list --all");
    EXPECT_EQ(after.status, ResponseStatus::Continue);
    EXPECT_NE(after.output.find("200"), std::string::npos);

    const auto exited = executor.execute("exit");
    EXPECT_EQ(exited.status, ResponseStatus::Continue);
    EXPECT_EQ(executor.layer(), RemoteLayer::Connection);
    EXPECT_EQ(executor.prompt(), "TPE>");
    EXPECT_FALSE(engine.hasProcess());
}

TEST(RemoteScanSession, TargetUnavailableIsReportedAsCommandError)
{
    auto os = std::make_unique<FakeMemoryOS>();
    FakeMemoryOS* raw = os.get();
    std::vector<tpe::Byte> bytes(64, 0);
    const std::int32_t initial = 100;
    std::memcpy(bytes.data(), &initial, sizeof(initial));
    auto process = raw->addProcess(4321, "mem.exe", std::move(bytes));
    ProcessEngine engine(std::move(os));
    RemoteCommandExecutor executor(engine);

    ASSERT_EQ(executor.execute("--open-process 4321").status, ResponseStatus::Continue);
    process->setAlive(false);

    const auto result = executor.execute("new-scan 100");
    EXPECT_EQ(result.status, ResponseStatus::CommandError);
    EXPECT_EQ(result.output, "The target process is no longer available.");
    EXPECT_EQ(executor.prompt(), "mem.exe-equal-i32>");
}

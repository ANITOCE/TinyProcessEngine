/**
 * test_linux_process.cpp — Linux 进程名获取集成测试
 *
 * 依赖: tpe_test_target (测试辅助进程) 或系统 sleep 命令
 * 仅在 __linux__ 下编译
 */

#ifdef __linux__

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>

#include "LinuxOS.h"
#include "LinuxProcess.h"

// ============================================================
// 测试 Fixture: 启动辅助进程，提供 PID 供测试
// ============================================================
class LinuxProcessNameTest : public ::testing::Test {
protected:
    pid_t m_childPid = -1;

    void SetUp() override {
        m_childPid = fork();
        if (m_childPid == 0) {
            // 子进程: 执行 sleep
            execl("/bin/sleep", "sleep", "5", nullptr);
            _exit(127);  // exec 失败
        }
        ASSERT_GT(m_childPid, 0) << "fork failed";
    }

    void TearDown() override {
        if (m_childPid > 0) {
            kill(m_childPid, SIGTERM);
            waitpid(m_childPid, nullptr, 0);
            m_childPid = -1;
        }
    }
};

// ============================================================
// US1 Acceptance: 验证 sleep 进程名
// ============================================================
TEST_F(LinuxProcessNameTest, OpenReturnsCorrectProcessName)
{
    LinuxOS os;
    auto proc = os.open(static_cast<Pid_t>(m_childPid));
    ASSERT_NE(proc, nullptr);
    std::string name = proc->getProcessName();
    EXPECT_EQ(name, "sleep");
    // 验证不含换行符
    EXPECT_EQ(name.find('\n'), std::string::npos);
}

TEST_F(LinuxProcessNameTest, ProcessNameIsNotUnknown)
{
    LinuxOS os;
    auto proc = os.open(static_cast<Pid_t>(m_childPid));
    ASSERT_NE(proc, nullptr);
    EXPECT_NE(proc->getProcessName(), "Unknown");
    EXPECT_NE(proc->getProcessName(), "<unknown>");
}

// ============================================================
// 进程名回退测试 (无需辅助进程，测试 readProcessName 内部逻辑)
// ============================================================
TEST(LinuxProcessNameFallbackTest, NonExistentProcessReturnsUnknown)
{
    // PID 99999 大概率不存在
    LinuxOS os;
    auto proc = os.open(99999);
    ASSERT_NE(proc, nullptr);
    // 进程名应为 "<unknown>" (而非 "Unknown")
    std::string name = proc->getProcessName();
    EXPECT_EQ(name, "<unknown>");
}

TEST(LinuxProcessNameFallbackTest, KernelThreadHasCommName)
{
    // PID 1 (init/systemd) 应该有有效的 comm 名称
    LinuxOS os;
    auto proc = os.open(1);
    ASSERT_NE(proc, nullptr);
    std::string name = proc->getProcessName();
    EXPECT_FALSE(name.empty());
    EXPECT_NE(name, "<unknown>");
}

#endif // __linux__

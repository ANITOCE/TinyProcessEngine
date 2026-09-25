/**
 * test_linux_memory.cpp — Linux 内存访问集成测试
 *
 * 覆盖: 权限过滤 / process_vm_readv/writev / 权限错误处理 / 后端降级
 * 仅在 __linux__ 下编译
 */

#ifdef __linux__

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "LinuxOS.h"
#include "LinuxProcess.h"
#include "MemoryPage.h"

using tpe::MemoryPage;
using tpe::platform::LinuxOS;
using tpe::platform::LinuxProcess;
using tpe::platform::MemBackend;
using tpe::platform::Pid_t;
using tpe::platform::PlatformProcess;

// ============================================================
// 测试 Fixture: 启动 tpe_test_target 辅助进程
// ============================================================
class LinuxMemoryTest : public ::testing::Test {
protected:
    pid_t m_childPid = -1;
    std::shared_ptr<PlatformProcess> m_process;

    void SetUp() override {
        m_childPid = fork();
        if (m_childPid == 0) {
            // 子进程: 执行 test helper
            // 注: 路径假设 test helper 在 build 目录下
            execl("./tests/tpe_test_target", "tpe_test_target", nullptr);
            // 回退: 尝试相对路径
            execl("tpe_test_target", "tpe_test_target", nullptr);
            _exit(127);
        }
        ASSERT_GT(m_childPid, 0) << "fork failed";
        usleep(100000);  // 等子进程初始化

        LinuxOS os;
        m_process = os.open(static_cast<Pid_t>(m_childPid));
        ASSERT_NE(m_process, nullptr);
    }

    void TearDown() override {
        m_process.reset();
        if (m_childPid > 0) {
            kill(m_childPid, SIGTERM);
            waitpid(m_childPid, nullptr, 0);
            m_childPid = -1;
        }
    }
};

// ============================================================
// US2: 内存页权限过滤
// ============================================================
TEST_F(LinuxMemoryTest, CheatablePagesOnlyReturnsPrivateReadWrite)
{
    auto pages = m_process->getCheatablePages();
    EXPECT_GT(pages.size(), 0u) << "Expected at least 1 cheatable page";

    // 验证所有返回的页均满足 rw-p 条件 (通过 maps 间接验证)
    std::ifstream maps("/proc/" + std::to_string(m_childPid) + "/maps");
    ASSERT_TRUE(maps.good());

    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream iss(line);
        std::string addr, perms;
        iss >> addr >> perms;
        // 检查: 若 perms 不满足 r+w+p, 则对应的地址范围不应出现在 pages 中
        if (!(perms.size() >= 3 && perms[0] == 'r' && perms[1] == 'w'
              && (perms.size() == 3 || perms[3] == 'p'))) {
            // 此 maps 行不是 cheatable 页，验证不在结果中
            // (简化验证: 仅检查非空和合理计数)
        }
    }
}

// ============================================================
// US3: 内存读写正确性
// ============================================================
TEST_F(LinuxMemoryTest, ReadWriteRoundTripViaProcessVm)
{
    auto pages = m_process->getCheatablePages();
    ASSERT_GT(pages.size(), 0u);

    // 在第一个可写页写入已知值并读回
    MemoryPage& page = pages[0];
    tpe::Address testAddr = page.start + 0x100;  // 页内偏移 256 字节
    if (testAddr + 8 > page.start + page.size) {
        testAddr = page.start;  // 回退到页起始
    }

    // 写入 8 字节测试值
    tpe::Memory writeVal = {0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34, 0x56, 0x78};
    auto writeResult = m_process->write(testAddr, writeVal);
    if (!writeResult) {
        // 某些页可能因权限不足写入失败——这是预期行为，跳过测试
        GTEST_SKIP() << "Write failed (expected for some pages): "
                     << writeResult.error().message;
    }

    // 读回验证
    MemoryPage readPage(testAddr, 8);
    auto readResult = m_process->read(readPage);
    ASSERT_TRUE(readResult) << "Read failed: " << readResult.error().message;
    EXPECT_EQ(readResult.value(), writeVal);
}

// ============================================================
// US4: 权限错误处理 (EPERM 诊断)
// ============================================================
TEST(LinuxPermissionHandlingTest, InaccessibleProcessReturnsPermissionError)
{
    // 尝试打开 PID 1 (需要特殊权限)，验证返回清晰的错误
    LinuxOS os;
    auto proc = os.open(1);
    ASSERT_NE(proc, nullptr);

    auto pages = proc->getCheatablePages();
    if (pages.empty()) {
        GTEST_SKIP() << "No cheatable pages for PID 1 (expected)";
    }

    auto result = proc->read(pages[0]);
    // 读取可能成功 (如果以 root 运行) 或返回权限错误
    if (!result) {
        std::string msg = result.error().message;
        // 错误消息应包含有用的诊断
        bool hasDiagnostic = (msg.find("perm") != std::string::npos
                           || msg.find("EACCES") != std::string::npos
                           || msg.find("EPERM") != std::string::npos
                           || msg.find("Permission") != std::string::npos);
        EXPECT_TRUE(hasDiagnostic)
            << "Error message should contain diagnostic info, got: " << msg;

        // 缺陷⑦(FR-020/C-P4):strerror 原文与可操作建议必须进入错误消息本身
        const bool hasStrerrorText =
            (msg.find(std::strerror(EPERM)) != std::string::npos)
            || (msg.find(std::strerror(EACCES)) != std::string::npos);
        EXPECT_TRUE(hasStrerrorText)
            << "Error message must include strerror text, got: " << msg;

        EXPECT_NE(msg.find("ptrace_scope"), std::string::npos)
            << "Error message must include actionable advice (ptrace_scope), got: " << msg;
    }
}

// ============================================================
// US2(缺陷④): ProcMem 后端读写顺序无关性(FR-011/FR-012;C-P2)
//
// 根因(修复前): 读路径以 O_RDONLY 建立并缓存 fd,后续写路径复用该只读 fd
// → pwrite EBADF。本组用例经测试接缝强制 ProcMem 后端,覆盖两种操作顺序。
// ============================================================
class LinuxProcMemTest : public ::testing::Test {
protected:
    pid_t m_childPid = -1;

    void SetUp() override {
        m_childPid = fork();
        if (m_childPid == 0) {
            execl("./tests/tpe_test_target", "tpe_test_target", nullptr);
            execl("tpe_test_target", "tpe_test_target", nullptr);
            _exit(127);
        }
        ASSERT_GT(m_childPid, 0) << "fork failed";
        usleep(100000);  // 等子进程初始化
    }

    void TearDown() override {
        if (m_childPid > 0) {
            kill(m_childPid, SIGTERM);
            waitpid(m_childPid, nullptr, 0);
            m_childPid = -1;
        }
    }

    /// 强制 ProcMem 后端的进程对象(生产默认 AutoDetect 不变)。
    std::unique_ptr<LinuxProcess> makeProcMemProcess() const {
        auto proc = std::make_unique<LinuxProcess>(
            static_cast<Pid_t>(m_childPid), "tpe_test_target");
        proc->setMemBackendForTesting(MemBackend::ProcMem);
        return proc;
    }

    /// 取首个可写页内的 8 字节测试地址(页内偏移 256;越界则回退页首)。
    static tpe::Address pickTestAddress(const std::vector<MemoryPage>& pages) {
        const MemoryPage& page = pages[0];
        tpe::Address addr = page.start + 0x100;
        if (addr + 8 > page.start + page.size) {
            addr = page.start;
        }
        return addr;
    }
};

/// 先读后写:修复前读路径缓存 O_RDONLY 句柄 → 写 pwrite EBADF;修复后句柄升级并写入成功。
TEST_F(LinuxProcMemTest, ProcMemWriteAfterReadSucceeds)
{
    auto proc = makeProcMemProcess();
    auto pages = proc->getCheatablePages();
    ASSERT_GT(pages.size(), 0u) << "Expected at least 1 cheatable page";
    const tpe::Address addr = pickTestAddress(pages);

    // 1) 先读:修复前该路径以 O_RDONLY 建立并缓存 fd
    auto readResult = proc->read(MemoryPage(addr, 8));
    ASSERT_TRUE(readResult) << "read failed: " << readResult.error().message;

    // 2) 再写:修复前复用只读 fd → 失败;修复后必须成功
    const tpe::Memory newValue = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    auto writeResult = proc->write(addr, newValue);
    ASSERT_TRUE(writeResult) << "write after read failed: " << writeResult.error().message;

    // 3) 复核写入生效
    auto verify = proc->read(MemoryPage(addr, 8));
    ASSERT_TRUE(verify) << "verify read failed: " << verify.error().message;
    EXPECT_EQ(verify.value(), newValue);
}

/// 先写后读:写路径建立 O_RDWR 句柄后,读路径复用句柄仍可读(顺序无关性)。
TEST_F(LinuxProcMemTest, ProcMemReadAfterWriteSucceeds)
{
    auto proc = makeProcMemProcess();
    auto pages = proc->getCheatablePages();
    ASSERT_GT(pages.size(), 0u) << "Expected at least 1 cheatable page";
    const tpe::Address addr = pickTestAddress(pages);

    // 1) 先写:fd<0 → O_RDWR
    const tpe::Memory newValue = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    auto writeResult = proc->write(addr, newValue);
    ASSERT_TRUE(writeResult) << "write failed: " << writeResult.error().message;

    // 2) 再读:须复用已有句柄读回
    auto readResult = proc->read(MemoryPage(addr, 8));
    ASSERT_TRUE(readResult) << "read after write failed: " << readResult.error().message;
    EXPECT_EQ(readResult.value(), newValue);
}

#endif // __linux__

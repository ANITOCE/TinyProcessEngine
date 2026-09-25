/**
 * test_windows_memory.cpp — Windows 进程写路径集成测试(Phase 05 · US2 · 缺陷③)
 *
 * 覆盖:
 *   - FR-008/C-P1:自身进程经 `WindowsOS::open` 打开后,`write` 必须成功且内容可见变化
 *     (修复前:打开未请求写权限 → WriteProcessMemory 必然 ACCESS_DENIED);
 *   - FR-009/C-P1:只读降级会话的 `write` 必须返回明确错误(消息含 `read-only`、
 *     native code = `ERROR_ACCESS_DENIED`),且不调用系统 API、不修改目标内存。
 *
 * 整文件 `_WIN32` 门控:非 Windows 构建下为空翻译单元(源文件注册见 tests/CMakeLists.txt)。
 */

#ifdef _WIN32

#include <gtest/gtest.h>

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <string>

#include "WindowsOS.hpp"
#include "WindowsProcess.hpp"

using tpe::PlatformError;
using tpe::Result;
using tpe::platform::Pid_t;
using tpe::platform::PlatformProcess;
using tpe::platform::WindowsOS;
using tpe::platform::WindowsProcess;

namespace {

/// 经生产打开路径(WindowsOS::open)打开指定进程,并转为具体类型以便操作只读标记。
std::shared_ptr<WindowsProcess> openProcess(Pid_t pid)
{
    WindowsOS os;
    std::shared_ptr<PlatformProcess> process = os.open(pid);
    if (process == nullptr) {
        return nullptr;
    }
    return std::dynamic_pointer_cast<WindowsProcess>(process);
}

} // namespace

/// FR-008/C-P1:读写权限打开 → 对自身进程的写入成功且目标内容变化。
TEST(WindowsWritePathTest, SelfProcessWriteSucceeds)
{
    std::shared_ptr<WindowsProcess> process = openProcess(::GetCurrentProcessId());
    ASSERT_NE(process, nullptr) << "WindowsOS::open(self) returned nullptr";

    volatile std::uint32_t target = 100;
    const tpe::Memory value = {0xE7, 0x03, 0x00, 0x00}; // 999(i32 小端)
    const tpe::Address address =
        reinterpret_cast<tpe::Address>(const_cast<std::uint32_t*>(&target));

    const Result<void, PlatformError> written = process->write(address, value);
    ASSERT_TRUE(written) << "write(self) failed: " << written.error().message;

    const std::uint32_t observed = target;
    EXPECT_EQ(observed, 999u) << "write must be visible in target memory";
}

/// FR-009/C-P1:只读降级会话 → write 返回明确错误(read-only + ERROR_ACCESS_DENIED),
/// 且内存不变(存根阶段:未拒绝 → 断言红)。
TEST(WindowsWritePathTest, ReadOnlyOpenRejectsWriteWithClearError)
{
    std::shared_ptr<WindowsProcess> process = openProcess(::GetCurrentProcessId());
    ASSERT_NE(process, nullptr) << "WindowsOS::open(self) returned nullptr";

    process->markReadOnly();
    ASSERT_TRUE(process->isReadOnly());

    volatile std::uint32_t target = 100;
    const tpe::Memory value = {0x01, 0x00, 0x00, 0x00};
    const tpe::Address address =
        reinterpret_cast<tpe::Address>(const_cast<std::uint32_t*>(&target));

    const Result<void, PlatformError> written = process->write(address, value);
    ASSERT_FALSE(written) << "read-only session must reject writes";
    EXPECT_NE(written.error().message.find("read-only"), std::string::npos)
        << "error message must mention read-only, got: " << written.error().message;
    EXPECT_EQ(written.error().native_code, static_cast<int>(ERROR_ACCESS_DENIED));

    const std::uint32_t observed = target;
    EXPECT_EQ(observed, 100u) << "read-only write must not modify memory";
}

#endif // _WIN32

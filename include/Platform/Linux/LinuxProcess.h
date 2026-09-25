#ifndef _LINUX_PROCESS_H_
#define _LINUX_PROCESS_H_

#include "Platform.h"

#ifdef __linux__

#include <sys/uio.h>      // process_vm_readv / process_vm_writev
#include <sys/ptrace.h>   // ptrace
#include <sys/wait.h>     // waitpid
#include <unistd.h>       // close, pread, pwrite, usleep

#include <string>

namespace tpe::platform {

// ============================================================
// MemBackend — 内存读写后端选择
// ============================================================
enum class MemBackend : uint8_t {
    AutoDetect = 0,   // 首次调用时探测并记录
    ProcessVm  = 1,   // 使用 process_vm_readv / process_vm_writev
    ProcMem    = 2    // 使用 /proc/PID/mem 文件 I/O（降级）
};

class LinuxProcess : public PlatformProcess
{
public:
    LinuxProcess() = default;
    LinuxProcess(Pid_t pid, std::string p_name);
    ~LinuxProcess() override;

    std::vector<MemoryPage> getCheatablePages() const override;
    Result<tpe::Memory, PlatformError> read(MemoryPage page) const override;
    Result<void, PlatformError> write(tpe::Address address, const tpe::Memory &value) override;

    // 测试接缝(US2/缺陷④):强制指定内存读写后端以覆盖 ProcMem 降级路径。
    // 仅赋值 m_memBackend,生产默认(AutoDetect)不变;生产代码不得调用。
    void setMemBackendForTesting(MemBackend backend) { m_memBackend = backend; }

private:
    // --- 内存页权限过滤 ---
    static bool isCheatablePage(const std::string& perms);

    // --- 内存读写后端 ---
    void detectMemBackend() const;
    Result<tpe::Memory, PlatformError> readViaProcessVm(MemoryPage page) const;
    Result<void, PlatformError> writeViaProcessVm(tpe::Address address, const tpe::Memory& value) const;
    Result<tpe::Memory, PlatformError> readViaProcMem(MemoryPage page) const;
    Result<void, PlatformError> writeViaProcMem(tpe::Address address, const tpe::Memory& value) const;

    // --- /proc/PID/mem 单 fd 惰性升级(缺陷④;FR-011/FR-012)---
    // needWrite=false:fd<0 时先试 O_RDWR,失败(无写权限)回退 O_RDONLY;
    // needWrite=true :fd<0 时 O_RDWR;已有只读 fd 时 close 后重开 O_RDWR。
    // 返回 false 表示打开失败(errno 保留失败原因)。
    bool ensureProcMemFd(bool needWrite) const;

    // --- ptrace 权限管理 ---
    Result<void, PlatformError> ensurePtraceAttached() const;
    void ensurePtraceDetached() const;

    // --- 状态成员 ---
    mutable MemBackend m_memBackend     = MemBackend::AutoDetect;
    mutable bool       m_ptraceAttached = false;
    mutable int        m_procMemFd          = -1;    // /proc/PID/mem 文件描述符缓存
    mutable bool       m_procMemFdWritable  = false; // 当前 fd 是否以 O_RDWR 打开
};

} // namespace tpe::platform

#endif // __linux__

#endif  // _LINUX_PROCESS_H_
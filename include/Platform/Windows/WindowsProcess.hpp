#pragma once

#include "Platform.hpp"
#include "HelpFunction.hpp"
#include "ScopedHandle.hpp"

#ifdef _WIN32

namespace tpe::platform {

class WindowsProcess : public PlatformProcess
{
public:
    WindowsProcess() = default;
    WindowsProcess(Pid_t pid);
    WindowsProcess(Pid_t pid, std::string p_name);
    WindowsProcess(Pid_t pid, std::string p_name, ScopedHandle handle) : PlatformProcess(pid, p_name), m_processHandle(std::move(handle)) {}
    ~WindowsProcess() = default;

    std::vector<MemoryPage> getCheatablePages() const override;
    Result<tpe::Memory, PlatformError> read(MemoryPage page) const override;
    Result<void, PlatformError> write(tpe::Address address, const tpe::Memory &value) override;

    // 只读会话标记(US2/缺陷③,FR-009):写权限不可得时降级只读打开。
    // 存根阶段仅记录标记;只读写拒绝语义在 T020 实现。
    bool isReadOnly() const { return m_readOnly; }
    void markReadOnly() { m_readOnly = true; }

protected:
    ScopedHandle m_processHandle;
    bool m_readOnly = false;
};

} // namespace tpe::platform

#endif // _WIN32
#ifndef _WINDOWS_PROCESS_H_
#define _WINDOWS_PROCESS_H_

#include "Platform.h"
#include "HelpFunction.h"
#include "ScopedHandle.h"

#ifdef _WIN32

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

protected:
    ScopedHandle m_processHandle;
};

#endif // _WIN32

#endif // _WINDOWS_PROCESS_H_
#ifndef _WINDOWS_OS_H
#define _WINDOWS_OS_H

#include "Platform.h"
#include "HelpFunction.h"
#include "WindowsProcess.h"

#ifdef _WIN32

namespace tpe::platform {

class WindowsOS : public PlatformOS
{
public:
    WindowsOS();
    ~WindowsOS();
    std::shared_ptr<PlatformProcess> open(Pid_t pid) override;

    // 缺陷⑧(FR-023/C-P5):枚举失败经返回值与 enumerationError() 上报。
    Result<std::vector<Pid_t>, PlatformError> getAllProcessesPid() override;

    void getAllProcesses(std::vector<Pid_t> allPid) override;
};

} // namespace tpe::platform

#endif // _WIN32

#endif // _WINDOWS_OS_H

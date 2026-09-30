#pragma once

#include "Platform.hpp"
#include "HelpFunction.hpp"
#include "WindowsProcess.hpp"

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

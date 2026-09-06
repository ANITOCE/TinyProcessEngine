#ifndef _WINDOWS_OS_H
#define _WINDOWS_OS_H

#include "Platform.h"
#include "HelpFunction.h"
#include "WindowsProcess.h"

#ifdef _WIN32

class WindowsOS : public PlatformOS
{
public:
    WindowsOS();
    ~WindowsOS();
    std::shared_ptr<PlatformProcess> open(Pid_t pid) override;

    std::vector<Pid_t> getAllProcessesPid() override;

    void getAllProcesses(std::vector<Pid_t> allPid) override;
};

#endif // _WIN32

#endif // _WINDOWS_OS_H

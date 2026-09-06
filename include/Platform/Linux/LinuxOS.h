#ifndef _LINUX_OS_H
#define _LINUX_OS_H

#include "Platform.h"

#ifdef __linux__

#include <string>

class LinuxOS : public PlatformOS
{
public:
    std::shared_ptr<PlatformProcess> open(Pid_t pid) override;
    std::vector<Pid_t> getAllProcessesPid() override;
    void getAllProcesses(std::vector<Pid_t> allPid) override;

private:
    static std::string readProcessName(Pid_t pid);
    static std::string extractBasename(const std::string& path);
};

#endif // __linux__

#endif // _LINUX_OS_H
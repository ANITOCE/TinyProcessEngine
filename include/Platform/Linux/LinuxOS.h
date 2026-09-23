#ifndef _LINUX_OS_H
#define _LINUX_OS_H

#include "Platform.h"

#ifdef __linux__

#include <string>

class LinuxOS : public PlatformOS
{
public:
    std::shared_ptr<PlatformProcess> open(Pid_t pid) override;
    // 缺陷⑧(FR-023/C-P5):枚举失败经返回值与 enumerationError() 上报。
    Result<std::vector<Pid_t>, PlatformError> getAllProcessesPid() override;
    void getAllProcesses(std::vector<Pid_t> allPid) override;

private:
    static std::string readProcessName(Pid_t pid);
    static std::string extractBasename(const std::string& path);
};

#endif // __linux__

#endif // _LINUX_OS_H
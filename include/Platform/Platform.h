#ifndef _PLATFORM_H_
#define _PLATFORM_H_

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "MemoryPage.h"
#include "TinyProcessEngine/Result.h"

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>

typedef DWORD Pid_t;
typedef HANDLE pHandle_t;

#elif defined(__linux__)
#include <unistd.h>
typedef int Pid_t;

#endif // __linux__ or win32

class PlatformProcess
{
protected:
    const Pid_t m_pid;
    std::string m_processName;

public:
    PlatformProcess() = default;
    PlatformProcess(Pid_t pid) : m_pid(pid) {}
    PlatformProcess(Pid_t pid, std::string p_name) : m_pid(pid), m_processName(p_name) {}
    virtual ~PlatformProcess() = default;

    virtual Pid_t getPid() const { return m_pid; }
    virtual std::string getProcessName() const { return m_processName; }

    virtual std::vector<MemoryPage> getCheatablePages() const = 0;
    virtual Result<tpe::Memory, PlatformError> read(MemoryPage page) const = 0;
    virtual Result<void, PlatformError> write(tpe::Address address, const tpe::Memory &value) = 0;
};

class PlatformOS
{
public:
    // std::vector<Pid_t> allPid = std::vector<Pid_t>(1024);
    std::vector<std::shared_ptr<PlatformProcess>> ProcessList;

    virtual std::shared_ptr<PlatformProcess> open(Pid_t pid) = 0;
    virtual std::vector<Pid_t> getAllProcessesPid() = 0;
    virtual void getAllProcesses(std::vector<Pid_t> allPid)= 0;

    virtual ~PlatformOS() = default;
};

std::shared_ptr<PlatformProcess> createPlatformProcess(Pid_t pid, std::string p_name);
std::unique_ptr<PlatformOS> createPlatformOS();
Pid_t str_to_pid(std::string str);

#endif // _PLATFORM_H_
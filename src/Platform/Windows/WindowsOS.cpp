#ifdef _WIN32

#include "WindowsOS.h"
#include "WindowsProcess.h"
#include "HelpFunction.h"

#include <TlHelp32.h>
#include <Psapi.h>

WindowsOS::WindowsOS()
{
    getAllProcesses(getAllProcessesPid());
}

WindowsOS::~WindowsOS()
{
    
}

std::vector<Pid_t> WindowsOS::getAllProcessesPid() {
    std::vector<Pid_t> processIds(1024);
    DWORD cbNeeded = 0;
    if (!EnumProcesses(processIds.data(), processIds.size() * sizeof(Pid_t), &cbNeeded))
    {
        return {};
    }
    size_t count = cbNeeded / sizeof(DWORD);
    processIds.resize(count);
    return processIds;
}

std::shared_ptr<PlatformProcess> WindowsOS::open(Pid_t pid)
{
    ScopedHandle hProcess(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!hProcess) {
        return nullptr;
    }
    return std::make_shared<WindowsProcess>(pid, "Unknown", std::move(hProcess));
}

// std::vector<std::shared_ptr<PlatformProcess>> WindowsOS::getAllProcesses(std::vector<Pid_t> AllProcessesPid)
// {
//     std::vector<std::shared_ptr<PlatformProcess>> processes;
//     for (auto pid : AllProcessesPid)
//     {
//         HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
//         if (hProcess) {
//             char processName[MAX_PATH] = "<unknown>";
//             // std::string processName = "<unknown>";
//             HMODULE hMod;
//             DWORD cbNeeded;
//             if (EnumProcessModules(hProcess, &hMod, sizeof(hMod), &cbNeeded)) {
//                 GetModuleBaseNameA(hProcess, hMod, processName, sizeof(processName));
//                 // 排除名为<unknown>的进程
//                 if (processName != "<unknown>" && (processes.empty() || processName != processes.back()->getProcessName())) {
//                     processes.push_back(std::make_shared<WindowsProcess>(pid, std::string(processName), hProcess));
//                 }
//             }
//             // 注意：这里不关闭 hProcess，由 WindowsProcess 管理释放
//         }
//     }
//     return processes;
// }

void WindowsOS::getAllProcesses(std::vector<Pid_t> allPid)
{
    if(allPid.empty()) {
       std::cerr << "PidList is empty!" << std::endl;
    }
    // std::vector<std::shared_ptr<PlatformProcess>> processes;
    for (auto pid : allPid) {
        this->ProcessList.push_back(std::make_shared<WindowsProcess>(pid));
    }
}

#endif // _WIN32
#ifdef _WIN32

#include "WindowsOS.h"
#include "WindowsProcess.h"
#include "HelpFunction.h"

#include <TlHelp32.h>
#include <Psapi.h>

WindowsOS::WindowsOS()
{
    auto pids = getAllProcessesPid();
    if (pids) {
        getAllProcesses(pids.value());
    }
}

WindowsOS::~WindowsOS()
{
    
}

Result<std::vector<Pid_t>, PlatformError> WindowsOS::getAllProcessesPid() {
    std::vector<Pid_t> processIds(1024);
    DWORD cbNeeded = 0;
    if (!EnumProcesses(processIds.data(), processIds.size() * sizeof(Pid_t), &cbNeeded))
    {
        // 红阶段(TDD,行为等价):失败仍返回空列表;绿色提交改为上报枚举错误。
        return Result<std::vector<Pid_t>, PlatformError>::success({});
    }
    size_t count = cbNeeded / sizeof(DWORD);
    processIds.resize(count);
    return Result<std::vector<Pid_t>, PlatformError>::success(std::move(processIds));
}

std::shared_ptr<PlatformProcess> WindowsOS::open(Pid_t pid)
{
    // 缺陷③(FR-008/FR-009,C-P1):读写权限优先;被拒则降级只读(读取零退化);
    // 都失败返回 nullptr(既有失败路径不变)。
    constexpr DWORD kReadWriteAccess = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
                                     | PROCESS_VM_WRITE | PROCESS_VM_OPERATION;
    constexpr DWORD kReadOnlyAccess  = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;

    ScopedHandle hProcess(OpenProcess(kReadWriteAccess, FALSE, pid));
    bool readOnly = false;
    if (!hProcess) {
        hProcess = ScopedHandle(OpenProcess(kReadOnlyAccess, FALSE, pid));
        readOnly = true;
    }
    if (!hProcess) {
        return nullptr;
    }

    auto process = std::make_shared<WindowsProcess>(pid, "Unknown", std::move(hProcess));
    if (readOnly) {
        process->markReadOnly();
    }
    return process;
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
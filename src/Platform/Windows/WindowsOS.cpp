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
    // 显式窄化转换：缓冲区字节数（Windows API 要求 DWORD）（FR-005/C-B3）
    if (!EnumProcesses(processIds.data(), static_cast<DWORD>(processIds.size() * sizeof(Pid_t)), &cbNeeded))
    {
        // 缺陷⑧(FR-023/C-P5):枚举失败上报错误,不得以空列表伪装成功。
        const PlatformError err = PlatformError::from_last_error("EnumProcesses", 0);
        m_enumerationError = err;
        return Result<std::vector<Pid_t>, PlatformError>::error(err);
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
    // std::vector<std::shared_ptr<PlatformProcess>> processes;
    for (auto pid : allPid) {
        this->ProcessList.push_back(std::make_shared<WindowsProcess>(pid));
    }
}

#endif // _WIN32
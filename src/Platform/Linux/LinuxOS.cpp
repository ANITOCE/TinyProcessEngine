#ifdef __linux__

#include "LinuxOS.h"
#include "LinuxProcess.h"

#include <dirent.h>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>

// ============================================================
// 辅助: 从路径提取文件名
// ============================================================
std::string LinuxOS::extractBasename(const std::string& path)
{
    if (path.empty()) return "";
    auto pos = path.find_last_of('/');
    if (pos != std::string::npos)
        return path.substr(pos + 1);
    return path;
}

// ============================================================
// 进程名获取: /proc/PID/comm → /proc/PID/cmdline → "<unknown>"
// ============================================================
std::string LinuxOS::readProcessName(Pid_t pid)
{
    // --- 主路径: /proc/PID/comm ---
    {
        std::ifstream commFile("/proc/" + std::to_string(pid) + "/comm");
        if (commFile.good()) {
            std::string name;
            std::getline(commFile, name);
            // 去除尾部不可打印字符
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == '\0'))
                name.pop_back();
            if (!name.empty())
                return name;
        }
    }

    // --- 回退路径: /proc/PID/cmdline (提取首个 Null 段) ---
    {
        std::ifstream cmdlineFile("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
        if (cmdlineFile.good()) {
            std::string firstArg;
            std::getline(cmdlineFile, firstArg, '\0');
            if (!firstArg.empty()) {
                return extractBasename(firstArg);
            }
        }
    }

    // --- 最终回退 ---
    return "<unknown>";
}

// ============================================================
// 进程枚举
// ============================================================
// ============================================================
// 构造:枚举进程列表(与 WindowsOS 对齐;缺陷⑧/FR-024)
// ============================================================
LinuxOS::LinuxOS()
{
    auto pids = getAllProcessesPid();
    if (pids) {
        getAllProcesses(pids.value());
    }
}

// ============================================================
// 进程枚举
// ============================================================
Result<std::vector<Pid_t>, PlatformError> LinuxOS::getAllProcessesPid()
{
    std::vector<Pid_t> pids;
    DIR *dir = opendir("/proc");
    if (!dir)
    {
        // 缺陷⑧(FR-023/C-P5):枚举失败上报错误,不得以空列表伪装成功。
        const PlatformError err = PlatformError::from_last_error("opendir /proc", 0);
        m_enumerationError = err;
        return Result<std::vector<Pid_t>, PlatformError>::error(err);
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_type == DT_DIR)
        {
            int pid = atoi(entry->d_name);
            if (pid > 0)
                pids.push_back(pid);
        }
    }
    closedir(dir);
    return Result<std::vector<Pid_t>, PlatformError>::success(std::move(pids));
}

void LinuxOS::getAllProcesses(std::vector<Pid_t> allPid)
{
    if (allPid.empty()) {
        std::cerr << "[WARN] PidList is empty!" << std::endl;
        return;
    }
    for (auto pid : allPid) {
        this->ProcessList.push_back(
            std::make_shared<LinuxProcess>(pid, readProcessName(pid)));
    }
}

std::shared_ptr<PlatformProcess> LinuxOS::open(Pid_t pid)
{
    return std::make_shared<LinuxProcess>(pid, readProcessName(pid));
}

#endif // __linux__
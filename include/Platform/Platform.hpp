#pragma once

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "MemoryPage.hpp"
#include "TinyProcessEngine/Result.hpp"

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#elif defined(__linux__)
#include <unistd.h>
#endif // __linux__ or win32

namespace tpe::platform {

#ifdef _WIN32
typedef DWORD Pid_t;
typedef HANDLE pHandle_t;
#elif defined(__linux__)
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

    /// 目标身份与权限检查（Phase 02）：
    /// - `bindIdentity()` 在打开时把后续读写绑定到当前目标实例（Windows 句柄 /
    ///   Linux `/proc/<pid>/mem` 描述符）；失败表示无法取得内存读权限。
    /// - `hasMemoryReadAccess()` 用于远程严格打开；本地兼容入口不强制调用。
    /// - `isAlive()` 供提交前身份校验；默认实现兼容既有测试替身。
    virtual bool bindIdentity() { return true; }
    virtual bool hasMemoryReadAccess() const { return true; }
    virtual bool isAlive() const { return true; }
    virtual bool isReadOnly() const { return false; }
};

class PlatformOS
{
public:
    // std::vector<Pid_t> allPid = std::vector<Pid_t>(1024);
    std::vector<std::shared_ptr<PlatformProcess>> ProcessList;

    virtual std::shared_ptr<PlatformProcess> open(Pid_t pid) = 0;
    // 缺陷⑧(FR-023;C-P5):枚举失败通过返回值上报,不得以空列表伪装成功。
    virtual Result<std::vector<Pid_t>, PlatformError> getAllProcessesPid() = 0;
    virtual void getAllProcesses(std::vector<Pid_t> allPid)= 0;

    /// 最近一次枚举失败的原因(缺陷⑧):枚举成功(含空列表)时为空。
    const std::optional<PlatformError>& enumerationError() const { return m_enumerationError; }

    virtual ~PlatformOS() = default;

protected:
    // 由平台实现在枚举失败时填充(缺陷⑧;C-P5)。
    std::optional<PlatformError> m_enumerationError;
};

std::shared_ptr<PlatformProcess> createPlatformProcess(Pid_t pid, std::string p_name);
std::unique_ptr<PlatformOS> createPlatformOS();
Pid_t str_to_pid(std::string str);

} // namespace tpe::platform
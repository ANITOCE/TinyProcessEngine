#ifndef _PROCESS_ENGINE_H_
#define _PROCESS_ENGINE_H_

#include "Platform.h"
#include "ValueType.h"
#include "Matches.h"
#include "ScanTypes.h"
#include "ScanSession.h"
#include "MemoryScanner.h"

#include <vector>
#include <memory>
#include <optional>

class ProcessEngine
{
private:
    inline static std::unique_ptr<PlatformOS> m_os = createPlatformOS();
    std::shared_ptr<PlatformProcess> m_currentProcess;
    MemoryScanner m_scanner;
    std::unique_ptr<ScanSession> m_session;

public:
    ProcessEngine();
    ~ProcessEngine();

    void getProcessList() const;
    std::shared_ptr<PlatformProcess> openProcess(Pid_t pid);

    /// 按 PID 查找进程名(单行);未命中时返回 std::nullopt(契约 C-T2)。
    std::optional<std::string> searchProcess(Pid_t pid) const;

    /// Execute first scan on the currently opened process.
    /// @param type    值类型（决定浮点容差与展示）
    /// @param pattern 搜索模式（由调用方显式提供；不再交互追问）
    /// Returns the number of matches found (0 if scan failed or no process).
    uint64_t searchMemory(const ValueType& type, const tpe::Memory& pattern);

    /// Execute next (incremental) scan on existing results.
    uint64_t nextScan(ScanCondition condition, const ValueType& type,
                      const std::optional<tpe::Memory>& newValue = std::nullopt);

    /// Access the current scan session (for CLI to display results).
    ScanSession* session() { return m_session.get(); }
    const ScanSession* session() const { return m_session.get(); }

    /// Check if a process is currently opened.
    bool hasProcess() const { return m_currentProcess != nullptr; }

    void modifyMemory();
};

#endif // _PROCESS_ENGINE_H_
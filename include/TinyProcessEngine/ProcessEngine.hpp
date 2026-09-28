#pragma once

#include "Platform.hpp"
#include "ValueType.hpp"
#include "Matches.hpp"
#include "ScanTypes.hpp"
#include "ScanSession.hpp"
#include "MemoryScanner.hpp"

#include <vector>
#include <memory>
#include <optional>

namespace tpe {

class ProcessEngine
{
private:
    // 缺陷⑧(R9):由静态改为实例成员,支持测试注入平台实现。
    std::unique_ptr<tpe::platform::PlatformOS> m_os;
    std::shared_ptr<tpe::platform::PlatformProcess> m_currentProcess;
    MemoryScanner m_scanner;
    std::unique_ptr<ScanSession> m_session;

public:
    ProcessEngine();
    /// 注入构造(缺陷⑧;R9 测试接缝):使用调用方提供的平台实现;
    /// 默认构造仍使用 createPlatformOS()。
    explicit ProcessEngine(std::unique_ptr<tpe::platform::PlatformOS> os);
    ~ProcessEngine();

    /// 打印进程列表(格式不变);平台枚举失败时返回该错误(FR-023/FR-024)。
    Result<void, PlatformError> getProcessList() const;
    std::shared_ptr<tpe::platform::PlatformProcess> openProcess(tpe::platform::Pid_t pid);

    /// 按 PID 查找进程名(单行);未命中时返回 std::nullopt(契约 C-T2)。
    std::optional<std::string> searchProcess(tpe::platform::Pid_t pid) const;

    /// Execute first scan on the currently opened process.
    /// @param type    值类型（决定浮点容差与展示）
    /// @param pattern 搜索模式（由调用方显式提供；不再交互追问）
    /// Returns the number of matches found (0 if scan failed or no process).
    uint64_t searchMemory(const ValueType& type, const tpe::Memory& pattern);

    /// Execute first scan with unknown initial value (`new-scan --unknown`).
    /// 记录按类型宽度对齐步进的候选地址（不做值过滤；C-D1/FR-002），
    /// 每条携带当轮实读快照；会话条件记为 `ScanCondition::Unknown`。
    /// @param type 值类型（决定对齐步进宽度）
    /// Returns the number of candidates recorded (0 if scan failed or no process).
    uint64_t searchUnknown(const ValueType& type);

    /// Execute first-round comparison scan (`new-scan --greater` / `--less`).
    /// 保留“当前值严格大于/小于 target”的地址（严格 GT/LT；C-D2/C-D3/FR-007–011），
    /// 采集步进同为类型宽度对齐，每条携带当轮实读快照；会话条件记为 `condition`。
    /// @param type      值类型（决定步进宽度与数值分派）
    /// @param condition GreaterThan（严格大于）或 LessThan（严格小于）
    /// @param target    外部目标值（小端内存表示）
    /// Returns the number of matches kept (0 if scan failed or no process).
    uint64_t searchComparison(const ValueType& type, ScanCondition condition,
                              const tpe::Memory& target);

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

} // namespace tpe
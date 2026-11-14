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
#include <string>

namespace tpe {

/// 打开进程的结果（Phase 02）：进程对象、实时名称与只读标记。
struct OpenProcessInfo {
    std::shared_ptr<tpe::platform::PlatformProcess> process;
    std::string name;
    bool readOnly = false;
};

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

    /// 返回与本地 `getProcessList()` 相同数据、相同格式的整份文本:
    /// 每个进程一行 `PID: <pid> ProcessName: <name>`,行分隔为 LF,不含提示符;
    /// 平台枚举失败时返回该错误(不打印)。供远程执行器使用(实现见 T016)。
    Result<std::string, PlatformError> processListText() const;

    std::shared_ptr<tpe::platform::PlatformProcess> openProcess(tpe::platform::Pid_t pid);

    /// Phase 02：按严格/兼容模式打开并返回实时名称、只读标记。
    /// `strict=true` 用于远程入口：要求 PID 存在、名称非空且不是
    /// `Unknown`/`<unknown>`、取得内存读权限并绑定身份；本地兼容模式只新增
    /// “PID 不存在必须失败”，不施加远程名称/权限限制。
    Result<OpenProcessInfo, PlatformError> openProcessResult(tpe::platform::Pid_t pid,
                                                             bool strict);

    /// 释放当前目标与扫描状态（扫描层 exit / 会话清理）。
    void closeProcess();

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

    /// Phase 02：当前目标是否仍对应打开时的进程实例。
    bool targetAlive() const { return m_currentProcess && m_currentProcess->isAlive(); }

    void modifyMemory();
};

} // namespace tpe
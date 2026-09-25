#pragma once

#include "Platform.hpp"
#include "ScanTypes.hpp"
#include "ValueType.hpp"
#include "ResultStorage.hpp"

#include <memory>
#include <vector>
#include <filesystem>
#include <optional>

namespace tpe {

// ============================================================
// SessionState — 扫描会话状态
// ============================================================
enum class SessionState {
    Idle,       // 无活动扫描
    Scanning,   // 扫描执行中
    Ready,      // 有可用结果集
};

// ============================================================
// SessionError — 会话操作失败信息(FR-012–014;C-E1)
// 库层失败经返回值报告,不依赖异常。
// ============================================================
struct SessionError {
    std::string message;
};

// ============================================================
// ScanSession — 管理一次完整搜索会话的生命周期
// ============================================================
class ScanSession {
public:
    explicit ScanSession(std::shared_ptr<tpe::platform::PlatformProcess> process);

    // ── 会话生命周期 ──
    void beginScan(const ValueType& type, ScanOptions options = {});
    void commitFirstScan(std::vector<ScanRecord> results);
    [[nodiscard]] Result<void, SessionError> commitNextScan(ScanCondition condition,
                                                            std::vector<ScanRecord> results);
    [[nodiscard]] Result<void, SessionError> undo();
    void close();

    // ── 查询 ──
    SessionState state() const { return m_state; }
    uint32_t round() const { return m_round; }
    uint64_t resultCount() const;
    const ValueType* valueType() const { return m_valueType; }
    ScanCondition lastCondition() const { return m_condition; }
    bool canUndo() const;
    bool isDiskBacked() const;

    // ── 结果访问 ──
    std::optional<ScanRecord> resultAt(uint64_t index) const;
    [[nodiscard]] Result<void, SessionError> exportTo(const std::filesystem::path& path,
                                                      std::string_view format) const;

    // ── 直接内存操作 ──
    Result<tpe::Memory, PlatformError> readMemory(tpe::Address addr, tpe::Size size) const;
    Result<void, PlatformError> writeMemory(tpe::Address addr, const tpe::Memory& data) const;

private:
    std::shared_ptr<tpe::platform::PlatformProcess> m_process;
    SessionState m_state = SessionState::Idle;
    uint32_t m_round = 0;
    ScanCondition m_condition = ScanCondition::ExactValue;
    const ValueType* m_valueType = nullptr;

    // 结果集存储:超过阈值(1,000,000 条)自动切换磁盘后端(缺陷⑤/FR-013)
    ResultStorage m_storage;                      // 当前轮结果集(内存块 / 磁盘临时文件)
    std::optional<ResultStorage> m_prevStorage;   // 上一轮结果集(单级 undo)
};

} // namespace tpe

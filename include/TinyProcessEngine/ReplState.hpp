#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "CliValueType.hpp"

namespace tpe::cli {

/// 扫描类型(提示符 `<scan-type>` 段;契约 C-R1/C-R2,勘误后的旗标集合)。
enum class ReplScanType { Equal, Unknown, Greater, Less, Changed, Unchanged };

/// 扫描类型的提示符文本(小写;如 "equal")。
std::string_view scanTypeName(ReplScanType type);

/// list 分页大小(契约 C-R3:每页 20 条)。
inline constexpr unsigned kReplPageSize = 20;

/// REPL 会话展示状态(data-model §1;纯逻辑,无 I/O)。
///
/// 提示符:`<process-name>-<scan-type>-<value-type>[-<value>]> `(数据模型 §1)。
/// 状态迁移方法按 data-model §4 覆盖 T2–T6、T9。
struct ReplState {
    std::string processName;                     // 会话内不变
    ReplScanType scanType = ReplScanType::Equal; // 默认 `equal`
    const CliValueType* valueType = nullptr;     // 默认 `i32`(构造时设置)
    std::optional<std::string> lastValue;        // 最近一次带值扫描的输入值
    uint64_t matchesTotal = 0;                   // 与 ScanSession::resultCount() 一致

    ReplState();
    explicit ReplState(std::string name);

    /// 提示符渲染(含尾随空格)。
    std::string prompt() const;

    /// 切换数值类型(旗标;跨命令保持)。
    void setValueType(const CliValueType& type);

    /// T2/T3:带值扫描成功 → scanType/valueType 更新,lastValue = 输入值,matchesTotal 更新。
    void onValueScan(ReplScanType type, const CliValueType& vt, std::string value, uint64_t total);

    /// T5:无值扫描成功 → scanType/valueType 更新;`--changed`/`--unchanged`/`--unknown`
    /// 清空 lastValue(`--unknown` 属本次无值扫描 → 值段隐藏;FR-004/C-D1),
    /// `--greater`/`--less` 保持;matchesTotal 更新。
    void onValuelessScan(ReplScanType type, const CliValueType& vt, uint64_t total);

    /// T6:undo 后同步总量(matchesTotal 回退;提示符其余不变)。
    void onUndo(uint64_t total);

    /// list 总页数(total = 0 时为 0)。
    unsigned pageCount() const;

    /// list 页码是否有效(契约 C-R3:正整数且不超总页数;total = 0 时任何页都无效)。
    bool isPageInRange(unsigned page) const;
};

} // namespace tpe::cli

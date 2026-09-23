#ifndef TPE_MEMORY_SCANNER_H_
#define TPE_MEMORY_SCANNER_H_

#include "MemoryPage.h"
#include "ValueType.h"
#include "ScanTypes.h"
#include "Platform.h"

#include <vector>
#include <cstdint>
#include <optional>

// ============================================================
// MemoryScanner — 纯算法扫描器（无状态）
//
// 通过 IProcess 抽象接口操作目标进程内存，执行首轮扫描、
// 增量过滤和 AOB 模式搜索。
// ============================================================
class MemoryScanner {
public:
    MemoryScanner() = default;

    /// 首轮全量扫描：遍历目标进程所有可读页，搜索匹配值（非交互）。
    /// @param process 目标进程
    /// @param type    值类型（用于浮点容差与变长判断）
    /// @param pattern 搜索模式（显式传入；宽度即搜索步进，不再交互追问）
    /// @return 匹配的 ScanRecord 列表；每条记录携带命中处当轮实读值的快照
    ///         （宽度 = 搜索宽度，>8 字节由 ScanRecord 构造器截断）
    std::vector<ScanRecord> firstScan(
        PlatformProcess& process,
        const ValueType& type,
        const tpe::Memory& pattern,
        const ScanOptions& options = {}
    );

    /// 增量过滤扫描：在上一轮结果基础上按条件筛选（非交互）。
    /// 读取宽度取自 `type.byteWidth()`（变长类型由 `newValue` 决定）。
    /// 比较条件语义（Phase 05 缺陷 ①/FR-002/FR-003）：
    /// - Changed/Unchanged：当前读值 vs 快照字节比较；无快照记录一律排除；
    /// - Increased/Decreased：按 `type.numericKind()` 分派（有符号/无符号/浮点），
    ///   比较宽度 = 类型宽度；Other（如 string）不保留。
    /// 所有保留记录均写入当轮实读值快照，作为下一轮比较基准。
    /// @param previousResults 上一轮匹配记录
    /// @param condition      过滤条件
    /// @param type           值类型（用于数值分派与读取宽度）
    /// @param newValue       条件为 ExactValue 时的目标值
    /// @return              过滤后的 ScanRecord 列表
    std::vector<ScanRecord> nextScan(
        PlatformProcess& process,
        const std::vector<ScanRecord>& previousResults,
        ScanCondition condition,
        const ValueType& type,
        const std::optional<tpe::Memory>& newValue = std::nullopt
    );

    /// AOB 模式搜索。
    /// @param pattern  AOB 模式（至少含 1 个确定性字节）
    /// @param options  扫描选项
    /// @return        匹配地址列表
    std::vector<tpe::Address> scanAOB(
        PlatformProcess& process,
        const struct AobPattern& pattern,
        const ScanOptions& options = {}
    );

public:
    // Boyer-Moore-Horspool 字节搜索 (public for testing)
    static std::vector<size_t> boyerMooreSearch(
        const tpe::Memory& haystack,
        const tpe::Memory& pattern,
        size_t startOffset = 0
    );

private:
};

#endif // TPE_MEMORY_SCANNER_H_

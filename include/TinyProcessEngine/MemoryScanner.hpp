#pragma once

#include "MemoryPage.hpp"
#include "ValueType.hpp"
#include "ScanTypes.hpp"
#include "Platform.hpp"

#include <vector>
#include <cstdint>
#include <optional>

namespace tpe {

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
        tpe::platform::PlatformProcess& process,
        const ValueType& type,
        const tpe::Memory& pattern,
        const ScanOptions& options = {}
    );

    /// 首轮未知初值扫描（`new-scan --unknown`；C-D1/FR-002）：
    /// 记录可扫描区域内按类型宽度对齐步进的**全部候选地址**，不做值过滤。
    /// - 步进宽度 = `type.byteWidth()`（u8→1 / i16→2 / i32,float→4 / i64,double→8）；
    ///   宽度 0（变长，如 string）无对齐口径 → 返回空结果（防御；CLI 层另有显式拒绝）
    /// - 起点 = `align_up(page.start, width)`；不跨页（候选的 width 字节完整位于页内）
    /// - 分块读取，不可读块跳过；块间按 width 网格连续推进（chunkSize 任意值无漂移）
    /// - 每条记录携带当轮实读 width 字节快照（>8 由 ScanRecord 构造器截断；INV-R）
    /// @param process 目标进程
    /// @param type    值类型（决定对齐步进宽度）
    /// @param options 扫描选项
    /// @return 候选 ScanRecord 列表（地址升序，与页序一致）
    std::vector<ScanRecord> firstScanUnknown(
        tpe::platform::PlatformProcess& process,
        const ValueType& type,
        const ScanOptions& options = {}
    );

    /// 首轮大小比较扫描（`new-scan --greater` / `--less`；C-D2/C-D3/FR-007–011）：
    /// 保留“当前值**严格大于/小于**外部目标值”的地址（相等不保留），按 NumericKind
    /// 分派数值语义（有符号/无符号/浮点；NaN 参与比较一律不匹配）；采集步进与
    /// `firstScanUnknown` 同为类型宽度对齐；每条记录携带当轮实读 width 字节快照。
    /// - `condition` 仅接受 `GreaterThan` / `LessThan`（仅首轮语义；其它条件防御性返回空）
    /// - 宽度 0（变长）/ 超 8 字节 / 目标值不足以解码 → 无比较口径 → 返回空结果
    ///   （CLI 层另有显式拒绝）
    /// @param process   目标进程
    /// @param type      值类型（决定步进宽度与数值分派）
    /// @param condition GreaterThan（严格大于）或 LessThan（严格小于）
    /// @param target    外部目标值（小端内存表示；取前 width 字节）
    /// @param options   扫描选项
    /// @return 命中 ScanRecord 列表（地址升序，与页序一致）
    std::vector<ScanRecord> firstScanComparison(
        tpe::platform::PlatformProcess& process,
        const ValueType& type,
        ScanCondition condition,
        const tpe::Memory& target,
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
        tpe::platform::PlatformProcess& process,
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
        tpe::platform::PlatformProcess& process,
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

} // namespace tpe

#ifndef TPE_SCAN_TYPES_H_
#define TPE_SCAN_TYPES_H_

#include "MemoryPage.h"

#include <cstdint>
#include <optional>

// ============================================================
// ScanCondition — 扫描条件枚举
//   首轮: ExactValue, Unknown
//   增量轮: ExactValue, Changed, Unchanged, Increased, Decreased
// ============================================================
enum class ScanCondition {
    ExactValue,   // 精确值匹配（首轮和增量轮均可用）
    Unknown,      // 未知初始值（仅首轮）— 记录所有地址不做值过滤
    Changed,      // 值已变化（仅增量轮）
    Unchanged,    // 值未变化（仅增量轮）
    Increased,    // 值变大（仅增量轮）
    Decreased,    // 值变小（仅增量轮）
};

// ============================================================
// ScanOptions — 扫描选项
// ============================================================
struct ScanOptions {
    bool writableOnly = true;                       // 仅扫描可写内存页
    bool executableOnly = false;                    // 仅扫描可执行内存页
    std::optional<tpe::Address> rangeStart;         // 自定义起始地址
    std::optional<tpe::Address> rangeEnd;           // 自定义结束地址
    tpe::Size chunkSize = 4 * 1024 * 1024;          // 内存页单次分块读取大小 (默认 4MB)
};

// ============================================================
// ScanRecord — 单条匹配记录 (fixed 24 bytes, packed)
//
//  Binary Layout:
//    Byte  0..7 : address       (uint64_t, LE)
//    Byte  8    : snapshot_size (uint8_t, 0..8)
//    Byte  9..16: snapshot_data (uint8_t[8], 仅前 snapshot_size 字节有效)
//    Byte 17..23: [padding reserved]
// ============================================================
#pragma pack(push, 1)
struct ScanRecord {
    tpe::Address address = 0;               // 8 bytes
    uint8_t      snapshot_size = 0;         // 1 byte — 快照有效字节数 [0, 8]
    uint8_t      snapshot_data[8] = {};     // 8 bytes — 小端序快照值

    ScanRecord() = default;

    ScanRecord(tpe::Address addr)
        : address(addr), snapshot_size(0) {}

    ScanRecord(tpe::Address addr, const tpe::Memory& snapshot)
        : address(addr)
    {
        size_t snapLen = snapshot.size();
        if (snapLen > sizeof(snapshot_data))
            snapLen = sizeof(snapshot_data);
        snapshot_size = static_cast<uint8_t>(snapLen);
        for (size_t i = 0; i < snapshot_size; ++i) {
            snapshot_data[i] = snapshot[i];
        }
    }

    /// Check if this record has a valid snapshot
    bool hasSnapshot() const { return snapshot_size > 0; }
};
#pragma pack(pop)

static_assert(sizeof(ScanRecord) == 17, "ScanRecord must be exactly 17 bytes (packed)");

#endif // TPE_SCAN_TYPES_H_

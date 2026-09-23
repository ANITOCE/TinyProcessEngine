#include "MemoryScanner.h"
#include "AobPattern.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <optional>

// ============================================================
// BMH: Build bad-character skip table
// ============================================================
static std::array<ptrdiff_t, 256> buildBadCharTable(const tpe::Memory& pattern) {
    std::array<ptrdiff_t, 256> table;
    ptrdiff_t m = static_cast<ptrdiff_t>(pattern.size());
    for (size_t i = 0; i < 256; ++i) { table[i] = m; }
    for (ptrdiff_t i = 0; i < m - 1; ++i) { table[pattern[i]] = m - 1 - i; }
    return table;
}

// ============================================================
// BMH search
// ============================================================
std::vector<size_t> MemoryScanner::boyerMooreSearch(
    const tpe::Memory& haystack, const tpe::Memory& pattern, size_t startOffset)
{
    std::vector<size_t> matches;
    if (pattern.empty() || haystack.empty() || pattern.size() > haystack.size()) return matches;
    auto badChar = buildBadCharTable(pattern);
    ptrdiff_t n = static_cast<ptrdiff_t>(haystack.size());
    ptrdiff_t m = static_cast<ptrdiff_t>(pattern.size());
    ptrdiff_t i = static_cast<ptrdiff_t>(startOffset);
    while (i <= n - m) {
        ptrdiff_t j = m - 1;
        while (j >= 0 && pattern[j] == haystack[i + j]) --j;
        if (j < 0) { matches.push_back(static_cast<size_t>(i)); i += 1; }
        else { uint8_t badByte = haystack[i + j]; i += badChar[badByte]; }
    }
    return matches;
}

// ============================================================
// Float/double tolerant comparison helpers
// ============================================================
template<typename T>
static bool floatTolerantMatch(const uint8_t* memBytes, const uint8_t* searchBytes, T tolerance) {
    T memVal, searchVal;
    std::memcpy(&memVal, memBytes, sizeof(T));
    std::memcpy(&searchVal, searchBytes, sizeof(T));
    if (std::isnan(memVal) || std::isnan(searchVal)) return false;
    if (std::isinf(memVal) || std::isinf(searchVal)) return memVal == searchVal;
    return std::abs(memVal - searchVal) < tolerance;
}

// ============================================================
// firstScan — Full linear scan of all readable pages
// ============================================================
std::vector<ScanRecord> MemoryScanner::firstScan(
    PlatformProcess& process, const ValueType& type, const tpe::Memory& pattern,
    const ScanOptions& options)
{
    std::vector<ScanRecord> results;
    if (pattern.empty()) return results;

    size_t typeWidth = pattern.size();
    bool isFloat  = (type.name.find("float")  != std::string::npos && typeWidth == 4);
    bool isDouble = (type.name.find("double") != std::string::npos && typeWidth == 8);
    bool isFloatingPoint = isFloat || isDouble;

    auto pages = process.getCheatablePages();
    // Filter by address range
    std::vector<MemoryPage> filteredPages;
    for (auto& page : pages) {
        if (options.rangeStart && page.start + page.size <= *options.rangeStart) continue;
        if (options.rangeEnd   && page.start >= *options.rangeEnd) continue;
        filteredPages.push_back(page);
    }

    tpe::Memory overlapBuf;

    for (auto& page : filteredPages) {
        size_t remaining = page.size;
        tpe::Address currAddr = page.start;

        while (remaining > 0) {
            size_t chunkSz = (std::min)(remaining, options.chunkSize);
            MemoryPage chunkPage(currAddr, chunkSz);
            auto rd = process.read(chunkPage);
            if (!rd) { currAddr += chunkSz; remaining -= chunkSz; overlapBuf.clear(); continue; }

            tpe::Memory& chunk = rd.value();
            tpe::Memory searchBuf;
            if (!overlapBuf.empty()) {
                searchBuf.reserve(overlapBuf.size() + chunk.size());
                searchBuf.insert(searchBuf.end(), overlapBuf.begin(), overlapBuf.end());
                searchBuf.insert(searchBuf.end(), chunk.begin(), chunk.end());
            } else {
                searchBuf = chunk;
            }

            size_t overlapSkip = overlapBuf.empty() ? 0
                : (overlapBuf.size() >= pattern.size() ? overlapBuf.size() - pattern.size() + 1 : 0);

            if (isFloatingPoint) {
                for (size_t off = overlapSkip; off + typeWidth <= searchBuf.size(); ++off) {
                    bool ok = isFloat
                        ? floatTolerantMatch<float>(searchBuf.data()+off, pattern.data(), 1e-6f)
                        : floatTolerantMatch<double>(searchBuf.data()+off, pattern.data(), 1e-9);
                    if (ok) {
                        tpe::Address addr = overlapBuf.empty() ? currAddr + off
                            : currAddr + (off - overlapBuf.size());
                        // 快照 = 命中处当轮实读字节(浮点容差匹配下为内存实值;>8 字节由构造器截断)
                        results.emplace_back(addr, tpe::Memory(searchBuf.begin() + off,
                                                               searchBuf.begin() + off + typeWidth));
                    }
                }
            } else {
                auto offsets = boyerMooreSearch(searchBuf, pattern, overlapSkip);
                for (auto off : offsets) {
                    tpe::Address addr = overlapBuf.empty() ? currAddr + off
                        : currAddr + (off - overlapBuf.size());
                    // 快照 = 命中处当轮实读字节(>8 字节由构造器截断)
                    results.emplace_back(addr, tpe::Memory(searchBuf.begin() + off,
                                                           searchBuf.begin() + off + typeWidth));
                }
            }

            // Save tail for cross-chunk boundary
            if (chunk.size() >= pattern.size() - 1 && pattern.size() > 1) {
                size_t start = chunk.size() - (pattern.size() - 1);
                overlapBuf.assign(chunk.begin() + start, chunk.end());
            } else { overlapBuf.clear(); }

            currAddr += chunkSz;
            remaining -= chunkSz;
        }
    }
    return results;
}

// ============================================================
// Numeric comparison helpers — 快照与当前读值的数值比较
//   (Phase 05 缺陷 ①;C-S2/S3:比较宽度=类型宽度,按 NumericKind 分派)
// ============================================================

namespace {

/// 数值比较前置条件:Other(如 string)、无快照、宽度为 0/超 8 字节、
/// 快照或当前读值不足类型宽度时均不可比较 ⇒ 排除(不保留)。
bool canCompareNumeric(NumericKind kind, const ScanRecord& prev,
                       const tpe::Memory& current, size_t width)
{
    return kind != NumericKind::Other && width > 0 && width <= sizeof(prev.snapshot_data) &&
           prev.snapshot_size >= width && current.size() >= width;
}

/// 小端读取 width(1/2/4/8)字节并零扩展至 64 位。
uint64_t readUnsignedLittleEndian(const uint8_t* bytes, size_t width)
{
    uint64_t raw = 0;
    std::memcpy(&raw, bytes, width);
    return raw;
}

/// 小端读取 width(1/2/4/8)字节并按最高有效位符号扩展至 64 位。
int64_t readSignedLittleEndian(const uint8_t* bytes, size_t width)
{
    uint64_t raw = readUnsignedLittleEndian(bytes, width);
    const unsigned bits = static_cast<unsigned>(width) * 8u;
    if (bits < 64u) {
        const uint64_t signMask = uint64_t{1} << (bits - 1u);
        if (raw & signMask) {
            raw |= ~((uint64_t{1} << bits) - 1u);
        }
    }
    return static_cast<int64_t>(raw);
}

/// 按浮点宽度解码两个小端值:cur > prev(Increased)或 cur < prev(Decreased)。
template <class T>
bool compareFloatingAs(ScanCondition condition, const uint8_t* prevBytes, const uint8_t* curBytes,
                       size_t width)
{
    if (width != sizeof(T)) return false;
    T prevVal{}, curVal{};
    std::memcpy(&prevVal, prevBytes, width);
    std::memcpy(&curVal, curBytes, width);
    return (condition == ScanCondition::Increased) ? (curVal > prevVal) : (curVal < prevVal);
}

/// Increased/Decreased 的统一判定:按 NumericKind 分派;不可比较时一律不保留。
bool compareNumeric(ScanCondition condition, NumericKind kind, const ScanRecord& prev,
                    const tpe::Memory& current, size_t width)
{
    if (!canCompareNumeric(kind, prev, current, width)) return false;

    switch (kind) {
    case NumericKind::SignedInteger: {
        const int64_t prevVal = readSignedLittleEndian(prev.snapshot_data, width);
        const int64_t curVal = readSignedLittleEndian(current.data(), width);
        return (condition == ScanCondition::Increased) ? (curVal > prevVal) : (curVal < prevVal);
    }
    case NumericKind::UnsignedInteger: {
        const uint64_t prevVal = readUnsignedLittleEndian(prev.snapshot_data, width);
        const uint64_t curVal = readUnsignedLittleEndian(current.data(), width);
        return (condition == ScanCondition::Increased) ? (curVal > prevVal) : (curVal < prevVal);
    }
    case NumericKind::FloatingPoint:
        if (width == sizeof(float)) {
            return compareFloatingAs<float>(condition, prev.snapshot_data, current.data(), width);
        }
        if (width == sizeof(double)) {
            return compareFloatingAs<double>(condition, prev.snapshot_data, current.data(), width);
        }
        return false; // 其他浮点宽度不可比较
    default:
        return false;
    }
}

} // namespace

// ============================================================
// nextScan — Incremental filtering
// ============================================================
std::vector<ScanRecord> MemoryScanner::nextScan(
    PlatformProcess& process,
    const std::vector<ScanRecord>& previousResults,
    ScanCondition condition,
    const ValueType& type,
    const std::optional<tpe::Memory>& newValue)
{
    std::vector<ScanRecord> filtered;
    if (previousResults.empty()) return filtered;

    // 读取宽度来自类型显式声明(取代交互式 askValue().size());
    // 0 = 变长类型(如 string):由待比较值决定;两者皆无时无宽度信息,无法执行比较扫描。
    size_t typeWidth = type.byteWidth();
    if (typeWidth == 0) {
        if (!newValue.has_value() || newValue->empty()) return filtered;
        typeWidth = newValue->size();
    }
    const NumericKind kind = type.numericKind();

    for (const auto& prev : previousResults) {
        // Read current value at this address
        MemoryPage page(prev.address, typeWidth);
        auto rd = process.read(page);
        if (!rd) continue;  // skip unreadable addresses

        tpe::Memory& currentBytes = rd.value();
        if (currentBytes.size() < typeWidth) continue;

        bool keep = false;

        switch (condition) {
        case ScanCondition::ExactValue: {
            if (!newValue.has_value()) continue;
            keep = (currentBytes == *newValue);
            break;
        }
        case ScanCondition::Changed: {
            // 字节语义:无快照(或快照宽于当前读值)⇒ 排除,不得默认保留(FR-002 / C-S2)
            if (prev.snapshot_size == 0 || prev.snapshot_size > currentBytes.size()) break;
            keep = (std::memcmp(currentBytes.data(), prev.snapshot_data,
                                prev.snapshot_size) != 0);
            break;
        }
        case ScanCondition::Unchanged: {
            if (prev.snapshot_size == 0 || prev.snapshot_size > currentBytes.size()) break;
            keep = (std::memcmp(currentBytes.data(), prev.snapshot_data,
                                prev.snapshot_size) == 0);
            break;
        }
        case ScanCondition::Increased:
        case ScanCondition::Decreased:
            // 数值语义(FR-003 / C-S3):比较宽度 = 类型宽度,按 NumericKind 分派
            keep = compareNumeric(condition, kind, prev, currentBytes, typeWidth);
            break;
        default:
            break;
        }

        if (keep) {
            // 所有轮次写当轮实值快照:下一轮比较基准 = 本轮实读字节(FR-001/FR-004 / C-S1)
            filtered.emplace_back(prev.address, currentBytes);
        }
    }

    return filtered;
}

// ============================================================
// scanAOB — Array of Bytes pattern search
// ============================================================
std::vector<tpe::Address> MemoryScanner::scanAOB(
    PlatformProcess& process,
    const AobPattern& pattern,
    const ScanOptions& options)
{
    std::vector<tpe::Address> results;
    if (pattern.isAllWildcards()) return results;

    auto pages = process.getCheatablePages();

    // Filter by region options
    std::vector<MemoryPage> filteredPages;
    for (auto& page : pages) {
        if (options.rangeStart && page.start + page.size <= *options.rangeStart) continue;
        if (options.rangeEnd   && page.start >= *options.rangeEnd) continue;
        filteredPages.push_back(page);
    }

    const size_t patLen = pattern.totalLength();
    if (patLen == 0) return results;

    for (auto& page : filteredPages) {
        size_t remaining = page.size;
        tpe::Address currAddr = page.start;

        while (remaining > 0) {
            size_t chunkSz = (std::min)(remaining, options.chunkSize);
            MemoryPage chunkPage(currAddr, chunkSz);
            auto rd = process.read(chunkPage);
            if (!rd) { currAddr += chunkSz; remaining -= chunkSz; continue; }

            tpe::Memory& chunk = rd.value();

            // Scan using anchor segment + full verification
            const auto& anchor = pattern.anchor;

            // Convert anchor bytes to pattern for BMH
            tpe::Memory anchorPattern(anchor.bytes.begin(), anchor.bytes.end());
            auto anchorHits = boyerMooreSearch(chunk, anchorPattern, 0);

            for (auto hit : anchorHits) {
                // Calculate where the full pattern would start
                ptrdiff_t patternStart = static_cast<ptrdiff_t>(hit) -
                                         static_cast<ptrdiff_t>(anchor.offset);

                if (patternStart < 0 || patternStart + static_cast<ptrdiff_t>(patLen) > static_cast<ptrdiff_t>(chunk.size())) {
                    continue; // pattern extends outside chunk
                }

                // Verify all segments
                bool allMatch = true;
                for (const auto& seg : pattern.segments) {
                    for (size_t bi = 0; bi < seg.bytes.size(); ++bi) {
                        size_t pos = patternStart + seg.offset + bi;
                        if (pos >= chunk.size() || chunk[pos] != seg.bytes[bi]) {
                            allMatch = false;
                            break;
                        }
                    }
                    if (!allMatch) break;
                }

                if (allMatch) {
                    results.push_back(currAddr + static_cast<size_t>(patternStart));
                }
            }

            currAddr += chunkSz;
            remaining -= chunkSz;
        }
    }

    // Sort and deduplicate
    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());

    return results;
}

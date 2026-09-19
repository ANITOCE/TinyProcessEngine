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
                        results.emplace_back(addr);
                    }
                }
            } else {
                auto offsets = boyerMooreSearch(searchBuf, pattern, overlapSkip);
                for (auto off : offsets) {
                    tpe::Address addr = overlapBuf.empty() ? currAddr + off
                        : currAddr + (off - overlapBuf.size());
                    results.emplace_back(addr);
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
    bool isFloat  = (type.name.find("float")  != std::string::npos && typeWidth == 4);
    bool isDouble = (type.name.find("double") != std::string::npos && typeWidth == 8);

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
            // Compare with previous snapshot
            if (prev.snapshot_size > 0 && prev.snapshot_size <= currentBytes.size()) {
                keep = (std::memcmp(currentBytes.data(), prev.snapshot_data,
                                    prev.snapshot_size) != 0);
            } else {
                keep = true; // No snapshot to compare against, keep by default
            }
            break;
        }
        case ScanCondition::Unchanged: {
            if (prev.snapshot_size > 0 && prev.snapshot_size <= currentBytes.size()) {
                keep = (std::memcmp(currentBytes.data(), prev.snapshot_data,
                                    prev.snapshot_size) == 0);
            } else {
                keep = true;
            }
            break;
        }
        case ScanCondition::Increased:
        case ScanCondition::Decreased: {
            if (prev.snapshot_size == 0 || prev.snapshot_size > currentBytes.size()) {
                keep = false; // No snapshot = can't compare
                break;
            }
            // Interpret as numeric values
            if (isFloat) {
                float prevVal, curVal;
                std::memcpy(&prevVal, prev.snapshot_data, sizeof(float));
                std::memcpy(&curVal, currentBytes.data(), sizeof(float));
                if (condition == ScanCondition::Increased)
                    keep = (curVal > prevVal);
                else
                    keep = (curVal < prevVal);
            } else if (isDouble) {
                double prevVal, curVal;
                std::memcpy(&prevVal, prev.snapshot_data, sizeof(double));
                std::memcpy(&curVal, currentBytes.data(), sizeof(double));
                if (condition == ScanCondition::Increased)
                    keep = (curVal > prevVal);
                else
                    keep = (curVal < prevVal);
            } else {
                // Integer comparison — convert both byte arrays to uint64_t for comparison
                uint64_t prevVal = 0, curVal = 0;
                std::memcpy(&prevVal, prev.snapshot_data,
                            (std::min)(sizeof(prevVal), static_cast<size_t>(prev.snapshot_size)));
                std::memcpy(&curVal, currentBytes.data(),
                            (std::min)(sizeof(curVal), currentBytes.size()));
                if (condition == ScanCondition::Increased)
                    keep = (curVal > prevVal);
                else
                    keep = (curVal < prevVal);
            }
            break;
        }
        default:
            break;
        }

        if (keep) {
            // Create new record with updated snapshot (FR-028: only for comparison conditions)
            bool storeSnapshot = (condition == ScanCondition::Changed ||
                                  condition == ScanCondition::Unchanged ||
                                  condition == ScanCondition::Increased ||
                                  condition == ScanCondition::Decreased);
            if (storeSnapshot) {
                filtered.emplace_back(prev.address, currentBytes);
            } else {
                filtered.emplace_back(prev.address);
            }
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

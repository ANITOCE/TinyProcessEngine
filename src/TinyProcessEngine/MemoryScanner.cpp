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
    // 红阶段 stub(T022a):绿阶段迁移原实现，使用显式 pattern（不再 askValue）
    (void)process;
    (void)type;
    (void)pattern;
    (void)options;
    return {};
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
    // 红阶段 stub(T022a):绿阶段迁移原实现，读取宽度改用 type.byteWidth()
    (void)process;
    (void)previousResults;
    (void)condition;
    (void)type;
    (void)newValue;
    return {};
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

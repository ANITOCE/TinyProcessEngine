#include <gtest/gtest.h>
#include "MemoryScanner.hpp"
#include "ScanTypes.hpp"
#include "ValueType.hpp"
#include "AobPattern.hpp"
#include "ScanSession.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using tpe::AobPattern;
using tpe::Double;
using tpe::Float;
using tpe::Int16;
using tpe::Int32;
using tpe::Int64;
using tpe::MemoryPage;
using tpe::MemoryScanner;
using tpe::NumericKind;
using tpe::PlatformError;
using tpe::Result;
using tpe::ScanCondition;
using tpe::ScanOptions;
using tpe::ScanRecord;
using tpe::ScanSession;
using tpe::String;
using tpe::UnsignedByte;
using tpe::ValueType;
using tpe::platform::Pid_t;
using tpe::platform::PlatformProcess;

// ============================================================
// MemoryScanner::boyerMooreSearch — Algorithm tests
// ============================================================
TEST(BoyerMooreTest, FindsSingleMatch) {
    tpe::Memory haystack = {0x00, 0x42, 0x00, 0x42, 0x00};
    tpe::Memory pattern  = {0x42};
    auto matches = MemoryScanner::boyerMooreSearch(haystack, pattern);
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0], 1u);
    EXPECT_EQ(matches[1], 3u);
}

TEST(BoyerMooreTest, NoMatch) {
    tpe::Memory haystack = {0x00, 0x01, 0x02};
    tpe::Memory pattern  = {0xFF};
    auto matches = MemoryScanner::boyerMooreSearch(haystack, pattern);
    EXPECT_TRUE(matches.empty());
}

TEST(BoyerMooreTest, PatternLargerThanHaystack) {
    tpe::Memory haystack = {0x00};
    tpe::Memory pattern  = {0x00, 0x01};
    auto matches = MemoryScanner::boyerMooreSearch(haystack, pattern);
    EXPECT_TRUE(matches.empty());
}

TEST(BoyerMooreTest, MultiBytePattern) {
    tpe::Memory haystack = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF};
    tpe::Memory pattern  = {0xDE, 0xAD, 0xBE, 0xEF};
    auto matches = MemoryScanner::boyerMooreSearch(haystack, pattern);
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0], 0u);
    EXPECT_EQ(matches[1], 4u);
}

TEST(BoyerMooreTest, StartOffset) {
    tpe::Memory haystack = {0x01, 0x02, 0x01, 0x02, 0x01};
    tpe::Memory pattern  = {0x01};
    auto matches = MemoryScanner::boyerMooreSearch(haystack, pattern, 1);
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0], 2u);
    EXPECT_EQ(matches[1], 4u);
}

// ============================================================
// AobPattern::parse tests
// ============================================================
TEST(AobPatternTest, ParseSimplePattern) {
    auto result = AobPattern::parse("48 8B 05 00 10");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->isAllWildcards());
    EXPECT_GE(result->segments.size(), 1u);
}

TEST(AobPatternTest, ParseWithWildcards) {
    auto result = AobPattern::parse("48 8B ?? ?? 00 10");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->segments.size(), 2u);
}

TEST(AobPatternTest, AllWildcardsRejected) {
    std::string err;
    auto result = AobPattern::parse("?? ?? ??", &err);
    EXPECT_FALSE(result.has_value());
}

TEST(AobPatternTest, InvalidHexRejected) {
    std::string err;
    auto result = AobPattern::parse("48 ZX", &err);
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(err.empty());
}

TEST(AobPatternTest, EmptyInputRejected) {
    std::string err;
    auto result = AobPattern::parse("", &err);
    EXPECT_FALSE(result.has_value());
}

TEST(AobPatternTest, TotalLength) {
    auto result = AobPattern::parse("48 8B ?? 00");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->totalLength(), 4u);
}

// ============================================================
// ScanTypes tests
// ============================================================
TEST(ScanRecordTest, DefaultConstruction) {
    ScanRecord rec;
    EXPECT_EQ(rec.address, 0u);
    EXPECT_EQ(rec.snapshot_size, 0u);
    EXPECT_FALSE(rec.hasSnapshot());
}

TEST(ScanRecordTest, WithSnapshot) {
    tpe::Memory snap = {0xAA, 0xBB, 0xCC, 0xDD};
    ScanRecord rec(0x12345, snap);
    EXPECT_EQ(rec.address, 0x12345u);
    EXPECT_EQ(rec.snapshot_size, 4u);
    EXPECT_TRUE(rec.hasSnapshot());
    EXPECT_EQ(rec.snapshot_data[0], 0xAA);
    EXPECT_EQ(rec.snapshot_data[3], 0xDD);
}

TEST(ScanOptionsTest, DefaultValues) {
    ScanOptions opts;
    EXPECT_TRUE(opts.writableOnly);
    EXPECT_FALSE(opts.executableOnly);
    EXPECT_FALSE(opts.rangeStart.has_value());
    EXPECT_EQ(opts.chunkSize, 4u * 1024 * 1024);
}

TEST(ScanConditionTest, EnumValues) {
    // Verify all expected enum values exist
    auto ev = ScanCondition::ExactValue;
    auto uk = ScanCondition::Unknown;
    auto ch = ScanCondition::Changed;
    auto uc = ScanCondition::Unchanged;
    auto inc = ScanCondition::Increased;
    auto dec = ScanCondition::Decreased;
    (void)ev; (void)uk; (void)ch; (void)uc; (void)inc; (void)dec;
}

// ============================================================
// T022a 非交互扫描:显式 pattern / 类型宽度,不再调用 askValue()
// ============================================================

namespace {

/// 测试替身:askValue() 每次调用计数(引擎解耦后不应再被调用)。
class NoPromptValueType : public ValueType {
public:
    NoPromptValueType() : ValueType("no-prompt-value") {}

    tpe::Memory askValue() const override
    {
        ++m_askCalls;
        return {};
    }

    std::size_t byteWidth() const override { return 4; } // 模拟 4 字节类型

    /// 数值分派对齐真实整数类型(Phase 05 T013):替身按无符号整数语义参与
    /// --greater/--less;修复前实现按零扩展无符号比较,故两者语义等价。
    NumericKind numericKind() const override { return NumericKind::UnsignedInteger; }

    mutable int m_askCalls = 0;
};

/// 测试替身:单页内存缓冲进程(实现 Platform.hpp 的进程接口)。
/// T007 扩展:可追加独立区域(多页/顺序断言)与标记不可读区域(读取失败路径)。
class FakeProcess : public PlatformProcess {
public:
    FakeProcess(tpe::Address base, tpe::Size size)
        : PlatformProcess(static_cast<Pid_t>(4242)),
          m_base(base),
          m_size(size),
          m_bytes(static_cast<std::size_t>(size), 0) {}

    tpe::Memory& bytes() { return m_bytes; }

    /// 追加独立区域(可扫描页;字节初值 0);返回其字节缓冲供布置。
    tpe::Memory& addRegion(tpe::Address base, tpe::Size size)
    {
        m_regions.push_back(Region{base, size, tpe::Memory(static_cast<std::size_t>(size), 0)});
        return m_regions.back().bytes;
    }

    /// 标记区域(按起点,须先 addRegion)不可读:与之相交的读取一律失败。
    void setRegionUnreadable(tpe::Address base)
    {
        for (const Region& region : m_regions) {
            if (region.base == base) {
                m_unreadable.push_back(MemoryPage(region.base, region.size));
            }
        }
    }

    std::vector<MemoryPage> getCheatablePages() const override
    {
        std::vector<MemoryPage> pages{MemoryPage(m_base, m_size)};
        for (const Region& region : m_regions) {
            pages.push_back(MemoryPage(region.base, region.size));
        }
        return pages;
    }

    Result<tpe::Memory, PlatformError> read(MemoryPage page) const override
    {
        if (intersectsUnreadable(page.start, page.size)) {
            return Result<tpe::Memory, PlatformError>::error(makeError());
        }
        if (contains(page.start, page.size)) {
            const std::size_t offset = static_cast<std::size_t>(page.start - m_base);
            return Result<tpe::Memory, PlatformError>::success(
                tpe::Memory(m_bytes.begin() + offset, m_bytes.begin() + offset + page.size));
        }
        for (const Region& region : m_regions) {
            if (page.start >= region.base && page.start - region.base + page.size <= region.size) {
                const std::size_t offset = static_cast<std::size_t>(page.start - region.base);
                return Result<tpe::Memory, PlatformError>::success(
                    tpe::Memory(region.bytes.begin() + offset,
                                region.bytes.begin() + offset + page.size));
            }
        }
        return Result<tpe::Memory, PlatformError>::error(makeError());
    }

    Result<void, PlatformError> write(tpe::Address address, const tpe::Memory& value) override
    {
        if (contains(address, value.size())) {
            std::copy(value.begin(), value.end(),
                      m_bytes.begin() + static_cast<std::size_t>(address - m_base));
            return Result<void, PlatformError>::success();
        }
        for (Region& region : m_regions) {
            if (address >= region.base && address - region.base + value.size() <= region.size) {
                std::copy(value.begin(), value.end(),
                          region.bytes.begin() + static_cast<std::size_t>(address - region.base));
                return Result<void, PlatformError>::success();
            }
        }
        return Result<void, PlatformError>::error(makeError());
    }

private:
    struct Region {
        tpe::Address base;
        tpe::Size size;
        tpe::Memory bytes;
    };

    bool contains(tpe::Address address, tpe::Size length) const
    {
        if (address < m_base) return false;
        const tpe::Size offset = address - m_base;
        return offset <= m_size && length <= m_size - offset;
    }

    bool intersectsUnreadable(tpe::Address start, tpe::Size size) const
    {
        for (const MemoryPage& page : m_unreadable) {
            if (start < page.start + page.size && page.start < start + size) return true;
        }
        return false;
    }

    PlatformError makeError() const
    {
        return PlatformError{"FakeProcess", static_cast<unsigned long>(getPid()), 0, "out of range"};
    }

    tpe::Address m_base;
    tpe::Size m_size;
    tpe::Memory m_bytes;
    std::vector<Region> m_regions;
    std::vector<MemoryPage> m_unreadable;
};

} // namespace

TEST(NonInteractiveScan, FirstScanUsesExplicitPattern)
{
    FakeProcess process(0x1000, 0x100);
    NoPromptValueType type;
    MemoryScanner scanner;
    process.bytes()[0x04] = 0x2A;
    process.bytes()[0x10] = 0x2A;

    const tpe::Memory pattern = {0x2A, 0x00, 0x00, 0x00};
    const std::vector<ScanRecord> results = scanner.firstScan(process, type, pattern);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].address, 0x1004u);
    EXPECT_EQ(results[1].address, 0x1010u);
}

TEST(NonInteractiveScan, FirstScanDoesNotCallAskValue)
{
    FakeProcess process(0x1000, 0x100);
    NoPromptValueType type;
    MemoryScanner scanner;

    (void)scanner.firstScan(process, type, tpe::Memory{0x01});
    EXPECT_EQ(type.m_askCalls, 0);
}

TEST(NonInteractiveScan, NextScanExactValueFiltersByProvidedValue)
{
    FakeProcess process(0x1000, 0x100);
    NoPromptValueType type;
    MemoryScanner scanner;
    process.bytes()[0x08] = 0x2A; // 0x1008 → 42
    process.bytes()[0x0C] = 0x2B; // 0x100C → 43

    std::vector<ScanRecord> previous{ScanRecord(0x1008), ScanRecord(0x100C)};
    const tpe::Memory wanted = {0x2A, 0x00, 0x00, 0x00};
    const std::vector<ScanRecord> results =
        scanner.nextScan(process, previous, ScanCondition::ExactValue, type, wanted);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].address, 0x1008u);
}

TEST(NonInteractiveScan, NextScanChangedUsesSnapshotComparison)
{
    FakeProcess process(0x1000, 0x100);
    NoPromptValueType type;
    MemoryScanner scanner;
    const tpe::Memory oldValue = {100, 0, 0, 0};
    process.bytes()[0x08] = 100; // 未变
    process.bytes()[0x0C] = 101; // 已变

    std::vector<ScanRecord> previous{ScanRecord(0x1008, oldValue), ScanRecord(0x100C, oldValue)};
    const std::vector<ScanRecord> results =
        scanner.nextScan(process, previous, ScanCondition::Changed, type);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].address, 0x100Cu);
    EXPECT_EQ(results[0].snapshot_size, 4u); // 比较条件轮次写入新快照
}

TEST(NonInteractiveScan, NextScanIncreasedAndDecreasedCompareNumerically)
{
    FakeProcess process(0x1000, 0x100);
    NoPromptValueType type;
    MemoryScanner scanner;
    const tpe::Memory oldValue = {100, 0, 0, 0};
    process.bytes()[0x08] = 99;  // 变小
    process.bytes()[0x0C] = 101; // 变大

    std::vector<ScanRecord> previous{ScanRecord(0x1008, oldValue), ScanRecord(0x100C, oldValue)};
    const std::vector<ScanRecord> increased =
        scanner.nextScan(process, previous, ScanCondition::Increased, type);
    ASSERT_EQ(increased.size(), 1u);
    EXPECT_EQ(increased[0].address, 0x100Cu);

    const std::vector<ScanRecord> decreased =
        scanner.nextScan(process, previous, ScanCondition::Decreased, type);
    ASSERT_EQ(decreased.size(), 1u);
    EXPECT_EQ(decreased[0].address, 0x1008u);
}

TEST(NonInteractiveScan, NextScanDoesNotCallAskValue)
{
    FakeProcess process(0x1000, 0x100);
    NoPromptValueType type;
    MemoryScanner scanner;
    std::vector<ScanRecord> previous{ScanRecord(0x1008, tpe::Memory{100, 0, 0, 0})};

    (void)scanner.nextScan(process, previous, ScanCondition::Changed, type);
    EXPECT_EQ(type.m_askCalls, 0);
}

// ============================================================
// US1(Phase 05,缺陷 ①)— 值快照写入
//   C-S1:所有轮次写当轮实值快照;FR-001 / FR-004 / INV-S1/S2/S3
// ============================================================

TEST(NonInteractiveScan, FirstScanStoresValueSnapshot)
{
    // 精确命中:快照 == 命中地址处的当轮实读字节,宽度 == 类型宽度(4)
    FakeProcess process(0x1000, 0x100);
    Int32 type;
    MemoryScanner scanner;
    process.bytes()[0x04] = 0x2A;
    process.bytes()[0x10] = 0x2A;

    const tpe::Memory pattern = {0x2A, 0x00, 0x00, 0x00};
    const std::vector<ScanRecord> results = scanner.firstScan(process, type, pattern);

    ASSERT_EQ(results.size(), 2u);
    for (const ScanRecord& rec : results) {
        ASSERT_EQ(rec.snapshot_size, 4u) << "address " << rec.address;
        const std::size_t offset = static_cast<std::size_t>(rec.address - 0x1000);
        for (std::size_t i = 0; i < 4; ++i) {
            EXPECT_EQ(rec.snapshot_data[i], process.bytes()[offset + i])
                << "address " << rec.address << " byte " << i;
        }
    }
    EXPECT_EQ(results[0].snapshot_data[0], 0x2A);

    // 浮点容差命中:快照必须是内存实值而非搜索 pattern(INV-S3)
    FakeProcess tolerant(0x2000, 0x40);
    Float floatType;
    const tpe::Memory searchValue = {0x00, 0x00, 0xC0, 0x3F}; // 1.5f
    const tpe::Memory actualValue = {0x01, 0x00, 0xC0, 0x3F}; // 1.5000001f(差 ~1.2e-7 < 1e-6 容差)
    std::copy(actualValue.begin(), actualValue.end(), tolerant.bytes().begin() + 0x08);

    const std::vector<ScanRecord> floatHits = scanner.firstScan(tolerant, floatType, searchValue);
    ASSERT_EQ(floatHits.size(), 1u);
    EXPECT_EQ(floatHits[0].address, 0x2008u);
    ASSERT_EQ(floatHits[0].snapshot_size, 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(floatHits[0].snapshot_data[i], actualValue[i]) << "byte " << i;
    }
}

TEST(NonInteractiveScan, NextScanExactValueStoresFreshSnapshot)
{
    // --equal 轮同样写当轮实值快照(取代 Phase 02 FR-028 豁免;FR-001)
    FakeProcess process(0x1000, 0x100);
    Int32 type;
    MemoryScanner scanner;
    process.bytes()[0x08] = 42;

    std::string error;
    const auto wanted = type.parse("42", error);
    ASSERT_TRUE(wanted.has_value()) << error;

    std::vector<ScanRecord> previous{ScanRecord(0x1008)};
    const std::vector<ScanRecord> kept =
        scanner.nextScan(process, previous, ScanCondition::ExactValue, type, wanted);

    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0].address, 0x1008u);
    ASSERT_EQ(kept[0].snapshot_size, 4u);
    EXPECT_EQ(kept[0].snapshot_data[0], 42);
    EXPECT_EQ(kept[0].snapshot_data[1], 0);
    EXPECT_EQ(kept[0].snapshot_data[2], 0);
    EXPECT_EQ(kept[0].snapshot_data[3], 0);
}

// ============================================================
// US1 — 无快照记录排除与变化/未变化字节语义
//   C-S2:Changed/Unchanged 以快照 memcmp 判定;无快照记录一律排除
//   (FR-002 / INV-S4;修复前 keep=true 兜底为缺陷本源)
// ============================================================

TEST(NonInteractiveScan, ChangedExcludesRecordsWithoutSnapshot)
{
    // 无快照记录:无论地址处值是否变化,均不得默认保留
    FakeProcess process(0x1000, 0x100);
    Int32 type;
    MemoryScanner scanner;
    process.bytes()[0x08] = 100; // 值未变
    process.bytes()[0x0C] = 101; // 值已变

    std::vector<ScanRecord> previous{ScanRecord(0x1008), ScanRecord(0x100C)};
    EXPECT_TRUE(scanner.nextScan(process, previous, ScanCondition::Changed, type).empty())
        << "无快照记录不得默认保留(C-S2/FR-002)";
    EXPECT_TRUE(scanner.nextScan(process, previous, ScanCondition::Unchanged, type).empty())
        << "无快照记录不得默认保留(Unchanged 同样适用)";
}

TEST(NonInteractiveScan, UnchangedKeepsAllWhenValuesIdentical)
{
    // 目标未变:--unchanged 全保留、--changed 为空;保留记录携带当轮实值快照
    FakeProcess process(0x1000, 0x100);
    Int32 type;
    MemoryScanner scanner;
    process.bytes()[0x08] = 100;
    process.bytes()[0x0C] = 100;

    std::string error;
    const auto v100 = type.parse("100", error);
    ASSERT_TRUE(v100.has_value()) << error;
    const std::vector<ScanRecord> first = scanner.firstScan(process, type, *v100);
    ASSERT_EQ(first.size(), 2u);

    const std::vector<ScanRecord> unchanged =
        scanner.nextScan(process, first, ScanCondition::Unchanged, type);
    ASSERT_EQ(unchanged.size(), 2u);
    EXPECT_EQ(unchanged[0].address, 0x1008u);
    EXPECT_EQ(unchanged[1].address, 0x100Cu);
    for (const ScanRecord& rec : unchanged) {
        ASSERT_EQ(rec.snapshot_size, 4u);
        EXPECT_EQ(rec.snapshot_data[0], 100);
    }
    EXPECT_TRUE(scanner.nextScan(process, first, ScanCondition::Changed, type).empty());
}

// ============================================================
// US1 — 链式过滤的比较基准(C-S4 / FR-004):
//   每轮以上一轮实值为基准;快照逐轮更新
// ============================================================

TEST(NonInteractiveScan, ChainedFiltersUseLatestValueAsBaseline)
{
    FakeProcess process(0x1000, 0x100);
    Int32 type;
    MemoryScanner scanner;
    std::string error;
    const auto v100 = type.parse("100", error);
    ASSERT_TRUE(v100.has_value()) << error;
    std::copy(v100->begin(), v100->end(), process.bytes().begin() + 0x08);

    const std::vector<ScanRecord> first = scanner.firstScan(process, type, *v100);
    ASSERT_EQ(first.size(), 1u);

    // 轮 2:100 → 102 ⇒ --greater 命中(基准 = 首轮实值 100)
    const auto v102 = type.parse("102", error);
    ASSERT_TRUE(v102.has_value()) << error;
    std::copy(v102->begin(), v102->end(), process.bytes().begin() + 0x08);
    const std::vector<ScanRecord> second =
        scanner.nextScan(process, first, ScanCondition::Increased, type);
    ASSERT_EQ(second.size(), 1u);
    ASSERT_EQ(second[0].snapshot_size, 4u);
    EXPECT_EQ(second[0].snapshot_data[0], 102); // 快照更新为上一轮实值

    // 轮 3:102 → 105 ⇒ --greater 命中、--less 不命中
    const auto v105 = type.parse("105", error);
    ASSERT_TRUE(v105.has_value()) << error;
    std::copy(v105->begin(), v105->end(), process.bytes().begin() + 0x08);
    ASSERT_EQ(scanner.nextScan(process, second, ScanCondition::Increased, type).size(), 1u);
    EXPECT_TRUE(scanner.nextScan(process, second, ScanCondition::Decreased, type).empty());

    // 轮 3':102 → 101 ⇒ --less 命中、--greater 不命中
    //(若比较基准停留在首轮值 100,101 会被判为递增 ⇒ 本双断言钉住"上一轮实值"基准)
    const auto v101 = type.parse("101", error);
    ASSERT_TRUE(v101.has_value()) << error;
    std::copy(v101->begin(), v101->end(), process.bytes().begin() + 0x08);
    ASSERT_EQ(scanner.nextScan(process, second, ScanCondition::Decreased, type).size(), 1u);
    EXPECT_TRUE(scanner.nextScan(process, second, ScanCondition::Increased, type).empty());
}

// ============================================================
// US1 — 数值比较按类型语义(C-S3 / FR-003)
//   有符号按符号扩展、无符号按零扩展、浮点按浮点(string 不参与)
// ============================================================

TEST(NonInteractiveScan, IncreasedComparesSignedIntegers)
{
    FakeProcess process(0x1000, 0x100);
    Int32 type;
    MemoryScanner scanner;
    std::string error;

    // 场景 1:-5 → -3(数值变大)⇒ --greater 命中、--less 不命中
    const auto minus5 = type.parse("-5", error);
    ASSERT_TRUE(minus5.has_value()) << error;
    std::copy(minus5->begin(), minus5->end(), process.bytes().begin() + 0x08);
    std::vector<ScanRecord> first{ScanRecord(0x1008, *minus5)};

    const auto minus3 = type.parse("-3", error);
    ASSERT_TRUE(minus3.has_value()) << error;
    std::copy(minus3->begin(), minus3->end(), process.bytes().begin() + 0x08);

    ASSERT_EQ(scanner.nextScan(process, first, ScanCondition::Increased, type).size(), 1u);
    EXPECT_TRUE(scanner.nextScan(process, first, ScanCondition::Decreased, type).empty());

    // 场景 2:跨符号边界 5 → -3(数值变小)⇒ --less 命中、--greater 不命中
    //(零扩展无符号比较会把 0xFFFFFFFD 判为最大值 ⇒ 方向反转,即缺陷本源)
    const auto plus5 = type.parse("5", error);
    ASSERT_TRUE(plus5.has_value()) << error;
    std::copy(plus5->begin(), plus5->end(), process.bytes().begin() + 0x10);
    std::vector<ScanRecord> second{ScanRecord(0x1010, *plus5)};
    std::copy(minus3->begin(), minus3->end(), process.bytes().begin() + 0x10);

    const std::vector<ScanRecord> less =
        scanner.nextScan(process, second, ScanCondition::Decreased, type);
    ASSERT_EQ(less.size(), 1u);
    EXPECT_EQ(less[0].address, 0x1010u);
    EXPECT_TRUE(scanner.nextScan(process, second, ScanCondition::Increased, type).empty());
}

TEST(NonInteractiveScan, IncreasedComparesUnsignedBytes)
{
    // u8:250 → 5(无符号回绕,5 < 250)⇒ --less 命中、--greater 不命中
    FakeProcess process(0x1000, 0x100);
    UnsignedByte type;
    MemoryScanner scanner;
    std::string error;

    const auto v250 = type.parse("250", error);
    ASSERT_TRUE(v250.has_value()) << error;
    std::copy(v250->begin(), v250->end(), process.bytes().begin() + 0x08);
    const std::vector<ScanRecord> first = scanner.firstScan(process, type, *v250);
    ASSERT_EQ(first.size(), 1u);

    const auto v5 = type.parse("5", error);
    ASSERT_TRUE(v5.has_value()) << error;
    std::copy(v5->begin(), v5->end(), process.bytes().begin() + 0x08);

    const std::vector<ScanRecord> less =
        scanner.nextScan(process, first, ScanCondition::Decreased, type);
    ASSERT_EQ(less.size(), 1u);
    EXPECT_EQ(less[0].address, 0x1008u);
    EXPECT_TRUE(scanner.nextScan(process, first, ScanCondition::Increased, type).empty());
}

TEST(NonInteractiveScan, StringNeverMatchesIncreasedOrDecreased)
{
    // 变长类型(string → NumericKind::Other)不参与 --greater/--less,一律不保留(C-S3)
    FakeProcess process(0x1000, 0x100);
    String type;
    MemoryScanner scanner;
    const tpe::Memory text = {'a', 'b', 'c'};
    std::copy(text.begin(), text.end(), process.bytes().begin() + 0x08);
    std::vector<ScanRecord> previous{ScanRecord(0x1008, text)};

    EXPECT_TRUE(scanner.nextScan(process, previous, ScanCondition::Increased, type).empty());
    EXPECT_TRUE(scanner.nextScan(process, previous, ScanCondition::Decreased, type).empty());
}

TEST(NonInteractiveScan, IncreasedDecreasedCompareFloats)
{
    // float:1.5 → 1.4 ⇒ --less 命中、--greater 不命中
    FakeProcess process(0x1000, 0x100);
    Float type;
    MemoryScanner scanner;
    std::string error;

    const auto v1_5 = type.parse("1.5", error);
    ASSERT_TRUE(v1_5.has_value()) << error;
    std::copy(v1_5->begin(), v1_5->end(), process.bytes().begin() + 0x08);
    const std::vector<ScanRecord> first = scanner.firstScan(process, type, *v1_5);
    ASSERT_EQ(first.size(), 1u);

    const auto v1_4 = type.parse("1.4", error);
    ASSERT_TRUE(v1_4.has_value()) << error;
    std::copy(v1_4->begin(), v1_4->end(), process.bytes().begin() + 0x08);

    const std::vector<ScanRecord> less =
        scanner.nextScan(process, first, ScanCondition::Decreased, type);
    ASSERT_EQ(less.size(), 1u);
    EXPECT_EQ(less[0].address, 0x1008u);
    EXPECT_TRUE(scanner.nextScan(process, first, ScanCondition::Increased, type).empty());

    // double 同型:2.5 → 2.25 ⇒ --less 命中、--greater 不命中
    FakeProcess doubleProcess(0x3000, 0x100);
    Double doubleType;
    const auto v2_5 = doubleType.parse("2.5", error);
    ASSERT_TRUE(v2_5.has_value()) << error;
    std::copy(v2_5->begin(), v2_5->end(), doubleProcess.bytes().begin() + 0x18);
    const std::vector<ScanRecord> doubleFirst =
        scanner.firstScan(doubleProcess, doubleType, *v2_5);
    ASSERT_EQ(doubleFirst.size(), 1u);

    const auto v2_25 = doubleType.parse("2.25", error);
    ASSERT_TRUE(v2_25.has_value()) << error;
    std::copy(v2_25->begin(), v2_25->end(), doubleProcess.bytes().begin() + 0x18);

    const std::vector<ScanRecord> doubleLess =
        scanner.nextScan(doubleProcess, doubleFirst, ScanCondition::Decreased, doubleType);
    ASSERT_EQ(doubleLess.size(), 1u);
    EXPECT_TRUE(
        scanner.nextScan(doubleProcess, doubleFirst, ScanCondition::Increased, doubleType).empty());
}

// ============================================================
// US3(Phase 05,缺陷 ②)— u8 单字节写入(C-S5 / FR-006)
//   经 ScanSession::writeMemory 写入 u8 解析产物:仅目标 1 字节被修改,
//   相邻 3 字节保持原值(修复前:4 字节写入覆盖相邻字节)。
// ============================================================

TEST(ScanSessionWrite, WriteU8WritesSingleByte)
{
    auto process = std::make_shared<FakeProcess>(0x1000, 0x40);
    std::fill(process->bytes().begin(), process->bytes().end(), static_cast<tpe::Byte>(0xAA));

    UnsignedByte type;
    std::string error;
    const auto parsed = type.parse("200", error);
    ASSERT_TRUE(parsed.has_value()) << error;

    ScanSession session(process);
    const Result<void, PlatformError> written = session.writeMemory(0x1008, *parsed);
    ASSERT_TRUE(written) << written.error().message;

    EXPECT_EQ(process->bytes()[0x08], 200); // 0xC8
    EXPECT_EQ(process->bytes()[0x09], 0xAA) << "相邻字节不得被写入覆盖(FR-006)";
    EXPECT_EQ(process->bytes()[0x0A], 0xAA) << "相邻字节不得被写入覆盖(FR-006)";
    EXPECT_EQ(process->bytes()[0x0B], 0xAA) << "相邻字节不得被写入覆盖(FR-006)";
}

// ============================================================
// US1(Phase 07 · T007)— firstScanUnknown 边界锁
//   C-D1/C-D3/R2:对齐步进矩阵、快照正确性、不可读页跳过、升序、
//   跨分块边界无漂移/无重复/无漏采(含小 chunkSize 与非宽度整数倍)。
// ============================================================

TEST(FirstScanUnknown, EnumeratesCandidatesAlignedByTypeWidth)
{
    // 64 字节页:u8→64 / i16→32 / i32→16 / i64→8;地址按宽度整除、步进=宽度
    FakeProcess process(0x1000, 0x40);
    MemoryScanner scanner;

    UnsignedByte u8;
    Int16 i16;
    Int32 i32;
    Int64 i64;

    const struct {
        const ValueType* type;
        size_t width;
        size_t expected;
    } cases[] = {
        {&u8, 1, 64u},
        {&i16, 2, 32u},
        {&i32, 4, 16u},
        {&i64, 8, 8u},
    };

    for (const auto& item : cases) {
        const std::vector<ScanRecord> results = scanner.firstScanUnknown(process, *item.type);
        ASSERT_EQ(results.size(), item.expected) << item.type->name;
        for (size_t k = 0; k < results.size(); ++k) {
            EXPECT_EQ(results[k].address % item.width, 0u)
                << "candidate must be aligned to width, got " << results[k].address;
            if (k > 0) {
                EXPECT_EQ(results[k].address - results[k - 1].address, item.width)
                    << "candidates must step by width";
            }
        }
        EXPECT_EQ(results.front().address, 0x1000u);
        EXPECT_EQ(results.back().address, 0x1000u + 0x40 - item.width)
            << "不跨页:末候选的宽度字节须完整在页内";
    }
}

TEST(FirstScanUnknown, StartsAtAlignedAddressWhenPageBaseIsUnaligned)
{
    // 页起点非对齐(0x1001):首个候选 = align_up(base,4) = 0x1004;
    // 末候选 0x103C(0x1040 的宽度字节越过页末 0x1041 → 不采集)
    FakeProcess process(0x1001, 0x40);
    MemoryScanner scanner;
    Int32 i32;

    const std::vector<ScanRecord> results = scanner.firstScanUnknown(process, i32);
    ASSERT_EQ(results.size(), 15u);
    EXPECT_EQ(results.front().address, 0x1004u);
    EXPECT_EQ(results.back().address, 0x103Cu);
    for (const ScanRecord& rec : results) {
        EXPECT_EQ(rec.address % 4u, 0u);
    }
}

TEST(FirstScanUnknown, RecordsSnapshotFromCurrentMemoryBytes)
{
    // 快照 = 当轮实读 width 字节(INV-R);零值同样携带快照(snapshot_size>0)
    FakeProcess process(0x1000, 0x20);
    MemoryScanner scanner;
    Int32 i32;
    process.bytes()[0x04] = 0x78; // 0x1004 → 0x12345678(小端)
    process.bytes()[0x05] = 0x56;
    process.bytes()[0x06] = 0x34;
    process.bytes()[0x07] = 0x12;

    const std::vector<ScanRecord> results = scanner.firstScanUnknown(process, i32);
    ASSERT_EQ(results.size(), 8u);
    for (const ScanRecord& rec : results) {
        EXPECT_EQ(rec.snapshot_size, 4u) << "address " << rec.address;
        const std::size_t offset = static_cast<std::size_t>(rec.address - 0x1000);
        for (std::size_t i = 0; i < 4; ++i) {
            EXPECT_EQ(rec.snapshot_data[i], process.bytes()[offset + i])
                << "address " << rec.address << " byte " << i;
        }
    }
    EXPECT_EQ(results[1].address, 0x1004u);
    EXPECT_EQ(results[1].snapshot_data[0], 0x78);
    EXPECT_EQ(results[1].snapshot_data[1], 0x56);
    EXPECT_EQ(results[1].snapshot_data[2], 0x34);
    EXPECT_EQ(results[1].snapshot_data[3], 0x12);

    // u8:快照宽度 = 1
    FakeProcess bytes(0x2000, 0x10);
    UnsignedByte u8;
    const std::vector<ScanRecord> u8Results = scanner.firstScanUnknown(bytes, u8);
    ASSERT_EQ(u8Results.size(), 16u);
    EXPECT_EQ(u8Results[0].snapshot_size, 1u);
}

TEST(FirstScanUnknown, SkipsUnreadablePagesAndKeepsAscendingOrder)
{
    // 中间区域不可读:整页跳过,其余页候选保留且全局升序
    FakeProcess process(0x1000, 0x20); // 8 个 i32 候选
    process.addRegion(0x2000, 0x40);   // 16 个候选——但不可读
    process.setRegionUnreadable(0x2000);
    process.addRegion(0x3000, 0x20);   // 8 个候选
    MemoryScanner scanner;
    Int32 i32;

    const std::vector<ScanRecord> results = scanner.firstScanUnknown(process, i32);

    ASSERT_EQ(results.size(), 16u);
    EXPECT_EQ(results[7].address, 0x101Cu); // 第一页末候选
    EXPECT_EQ(results[8].address, 0x3000u); // 直接跳到第三页
    for (size_t k = 1; k < results.size(); ++k) {
        EXPECT_LT(results[k - 1].address, results[k].address) << "ascending order";
    }
    for (const ScanRecord& rec : results) {
        EXPECT_FALSE(rec.address >= 0x2000u && rec.address < 0x2040u)
            << "unreadable page must not produce candidates";
    }
}

TEST(FirstScanUnknown, ChunkBoundariesKeepAlignmentWithoutDrift)
{
    // 101 字节页 → i32 候选 25 个;小 chunkSize(含非宽度整数倍)与整页读取逐项一致:
    // 无漂移、无重复、无漏采
    FakeProcess process(0x1000, 0x65);
    MemoryScanner scanner;
    Int32 i32;

    const std::vector<ScanRecord> full = scanner.firstScanUnknown(process, i32);
    ASSERT_EQ(full.size(), 25u);
    EXPECT_EQ(full.back().address, 0x1060u);

    for (const tpe::Size chunkSize :
         {tpe::Size{16}, tpe::Size{8}, tpe::Size{7}, tpe::Size{4}, tpe::Size{1}}) {
        ScanOptions options;
        options.chunkSize = chunkSize;
        const std::vector<ScanRecord> chunked = scanner.firstScanUnknown(process, i32, options);
        ASSERT_EQ(chunked.size(), full.size()) << "chunkSize=" << chunkSize;
        for (size_t k = 0; k < full.size(); ++k) {
            EXPECT_EQ(chunked[k].address, full[k].address)
                << "chunkSize=" << chunkSize << " index " << k;
        }
    }
}

// ============================================================
// US2(Phase 07 · T011)— firstScanComparison 数值语义边界锁
//   C-D2/C-D3/R4:严格 GT/LT 矩阵(符号族边界/u8 无符号/浮点 IEEE·NaN)、
//   对齐步进计数(镜像 T007)、快照正确性、跨分块无漂移、防御输入。
// ============================================================

namespace {

/// 收集地址序列(保持记录顺序;升序由用例单独断言)。
std::vector<tpe::Address> comparisonAddresses(const std::vector<ScanRecord>& records)
{
    std::vector<tpe::Address> out;
    out.reserve(records.size());
    for (const ScanRecord& rec : records) {
        out.push_back(rec.address);
    }
    return out;
}

/// 指定类型值的小端内存表示(充当外部目标值;大小即类型宽度)。
template <class T>
tpe::Memory littleEndianBytes(T value)
{
    tpe::Memory bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

void putI16AtSlot(tpe::Memory& bytes, std::size_t slot, std::int16_t value)
{
    std::memcpy(bytes.data() + slot * 2, &value, sizeof(value));
}

void putI32AtSlot(tpe::Memory& bytes, std::size_t slot, std::int32_t value)
{
    std::memcpy(bytes.data() + slot * 4, &value, sizeof(value));
}

void putI64AtSlot(tpe::Memory& bytes, std::size_t slot, std::int64_t value)
{
    std::memcpy(bytes.data() + slot * 8, &value, sizeof(value));
}

void putFloatAtSlot(tpe::Memory& bytes, std::size_t slot, float value)
{
    std::memcpy(bytes.data() + slot * 4, &value, sizeof(value));
}

void putDoubleAtSlot(tpe::Memory& bytes, std::size_t slot, double value)
{
    std::memcpy(bytes.data() + slot * 8, &value, sizeof(value));
}

/// 测试替身:4 字节宽度但 NumericKind::Other(不可由 CLI 到达;R4 防御路径)。
class OtherKindValueType : public ValueType {
public:
    OtherKindValueType() : ValueType("other-4byte") {}

    tpe::Memory askValue() const override { return {}; }
    std::size_t byteWidth() const override { return 4; }
    NumericKind numericKind() const override { return NumericKind::Other; }
};

} // namespace

TEST(FirstScanComparison, SignedIntegerMatrixKeepsStrictOrderBoundaries)
{
    // i32 全矩阵:正负对称、符号边界(INT32_MIN/MAX)、严格性(== 不保留)
    FakeProcess process(0x1000, 0x40);
    const std::int32_t values[16] = {5,          -5,          0,     100, 99,    101,
                                     INT32_MAX,  INT32_MIN,   1,     -1,  255,   -255,
                                     7,          -7,          42,    -42};
    for (std::size_t slot = 0; slot < 16; ++slot) {
        putI32AtSlot(process.bytes(), slot, values[slot]);
    }
    MemoryScanner scanner;
    Int32 i32;

    // 严格大于 0:全部正值(9 个;0 不保留)
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process, i32, ScanCondition::GreaterThan, littleEndianBytes<std::int32_t>(0))),
              (std::vector<tpe::Address>{0x1000, 0x100C, 0x1010, 0x1014, 0x1018, 0x1020,
                                         0x1028, 0x1030, 0x1038}));
    // 严格小于 0:全部负值(6 个;含 INT32_MIN)
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process, i32, ScanCondition::LessThan, littleEndianBytes<std::int32_t>(0))),
              (std::vector<tpe::Address>{0x1004, 0x101C, 0x1024, 0x102C, 0x1034, 0x103C}));
    // 严格大于 100:101 / 255 / INT32_MAX;==100(0x100C)不保留
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process, i32, ScanCondition::GreaterThan, littleEndianBytes<std::int32_t>(100))),
              (std::vector<tpe::Address>{0x1014, 0x1018, 0x1028}));
    // 严格大于 99:100/101/255/INT32_MAX;==99(0x1010)不保留(= 边界严格性)
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process, i32, ScanCondition::GreaterThan, littleEndianBytes<std::int32_t>(99))),
              (std::vector<tpe::Address>{0x100C, 0x1014, 0x1018, 0x1028}));
    // 严格小于 -100:INT32_MIN / -255
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process, i32, ScanCondition::LessThan, littleEndianBytes<std::int32_t>(-100))),
              (std::vector<tpe::Address>{0x101C, 0x102C}));
    // 极端边界:无可比较者 → 空(不误保留等值端)
    EXPECT_TRUE(scanner
                    .firstScanComparison(process, i32, ScanCondition::GreaterThan,
                                         littleEndianBytes<std::int32_t>(INT32_MAX))
                    .empty());
    EXPECT_TRUE(scanner
                    .firstScanComparison(process, i32, ScanCondition::LessThan,
                                         littleEndianBytes<std::int32_t>(INT32_MIN))
                    .empty());
}

TEST(FirstScanComparison, Signed16And64BitFamiliesKeepBoundaries)
{
    // i16:-1/0/1/INT16_MIN/MAX 边界
    FakeProcess process16(0x1000, 0x10);
    const std::int16_t values16[8] = {-1, 0, 1, INT16_MAX, INT16_MIN, 2, -2, 5};
    for (std::size_t slot = 0; slot < 8; ++slot) {
        putI16AtSlot(process16.bytes(), slot, values16[slot]);
    }
    MemoryScanner scanner;
    Int16 i16;
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process16, i16, ScanCondition::GreaterThan, littleEndianBytes<std::int16_t>(0))),
              (std::vector<tpe::Address>{0x1004, 0x1006, 0x100A, 0x100E}));
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process16, i16, ScanCondition::LessThan, littleEndianBytes<std::int16_t>(0))),
              (std::vector<tpe::Address>{0x1000, 0x1008, 0x100C}));
    EXPECT_TRUE(scanner
                    .firstScanComparison(process16, i16, ScanCondition::GreaterThan,
                                         littleEndianBytes<std::int16_t>(INT16_MAX))
                    .empty());

    // i64:-1/0/1/INT64_MAX 边界
    FakeProcess process64(0x2000, 0x20);
    const std::int64_t values64[4] = {-1, 0, 1, INT64_MAX};
    for (std::size_t slot = 0; slot < 4; ++slot) {
        putI64AtSlot(process64.bytes(), slot, values64[slot]);
    }
    Int64 i64;
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process64, i64, ScanCondition::GreaterThan, littleEndianBytes<std::int64_t>(0))),
              (std::vector<tpe::Address>{0x2010, 0x2018}));
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process64, i64, ScanCondition::LessThan, littleEndianBytes<std::int64_t>(0))),
              (std::vector<tpe::Address>{0x2000}));
    EXPECT_TRUE(scanner
                    .firstScanComparison(process64, i64, ScanCondition::LessThan,
                                         littleEndianBytes<std::int64_t>(INT64_MIN))
                    .empty());
}

TEST(FirstScanComparison, UnsignedByteUsesUnsignedSemantics)
{
    // u8:0/255 无符号边界;步进宽度 1;每条快照 = 1 字节
    FakeProcess process(0x1000, 0x08);
    const tpe::Byte bytes[8] = {0, 1, 127, 128, 200, 254, 255, 100};
    for (std::size_t slot = 0; slot < 8; ++slot) {
        process.bytes()[slot] = bytes[slot];
    }
    MemoryScanner scanner;
    UnsignedByte u8;

    const tpe::Memory target200{static_cast<tpe::Byte>(200)};
    const std::vector<ScanRecord> greater200 =
        scanner.firstScanComparison(process, u8, ScanCondition::GreaterThan, target200);
    EXPECT_EQ(comparisonAddresses(greater200),
              (std::vector<tpe::Address>{0x1005, 0x1006})); // 254/255;==200 不保留

    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process, u8, ScanCondition::LessThan,
                  tpe::Memory{static_cast<tpe::Byte>(1)})),
              (std::vector<tpe::Address>{0x1000})); // 仅 0;< 1 严格

    const std::vector<ScanRecord> greater100 =
        scanner.firstScanComparison(process, u8, ScanCondition::GreaterThan,
                                    tpe::Memory{static_cast<tpe::Byte>(100)});
    EXPECT_EQ(comparisonAddresses(greater100),
              (std::vector<tpe::Address>{0x1002, 0x1003, 0x1004, 0x1005, 0x1006}));
    for (const ScanRecord& rec : greater100) {
        EXPECT_EQ(rec.snapshot_size, 1u) << "u8 snapshot width must be 1";
        const std::size_t offset = static_cast<std::size_t>(rec.address - 0x1000);
        EXPECT_EQ(rec.snapshot_data[0], process.bytes()[offset]);
    }

    EXPECT_TRUE(scanner
                    .firstScanComparison(process, u8, ScanCondition::GreaterThan,
                                         tpe::Memory{static_cast<tpe::Byte>(255)})
                    .empty());
    EXPECT_TRUE(scanner
                    .firstScanComparison(process, u8, ScanCondition::LessThan,
                                         tpe::Memory{static_cast<tpe::Byte>(0)})
                    .empty());
}

TEST(FirstScanComparison, FloatingPointFollowsIeeeAndNanIsNeverKept)
{
    // float:±Inf 按 IEEE;NaN 内存/Nan 目标一律不匹配
    FakeProcess process(0x1000, 0x40);
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float values[16] = {1.5f, -1.5f, 0.0f, inf,  -inf, nan,  100.0f, 100.5f,
                              99.5f, -100.0f, 1e30f, -1e30f, 0.25f, -0.25f, 42.0f, -42.0f};
    for (std::size_t slot = 0; slot < 16; ++slot) {
        putFloatAtSlot(process.bytes(), slot, values[slot]);
    }
    MemoryScanner scanner;
    Float f;

    // 严格大于 100.0:+Inf / 100.5 / 1e30(NaN 槽 0x1014 不保留)
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process, f, ScanCondition::GreaterThan, littleEndianBytes<float>(100.0f))),
              (std::vector<tpe::Address>{0x100C, 0x101C, 0x1028}));
    // 严格小于 -100.0:-Inf / -1e30
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process, f, ScanCondition::LessThan, littleEndianBytes<float>(-100.0f))),
              (std::vector<tpe::Address>{0x1010, 0x102C}));
    // 目标 NaN:一律不匹配
    EXPECT_TRUE(scanner
                    .firstScanComparison(process, f, ScanCondition::GreaterThan,
                                         littleEndianBytes<float>(nan))
                    .empty());
    EXPECT_TRUE(scanner
                    .firstScanComparison(process, f, ScanCondition::LessThan,
                                         littleEndianBytes<float>(nan))
                    .empty());
    // ±Inf 端:无严格更极端者
    EXPECT_TRUE(scanner
                    .firstScanComparison(process, f, ScanCondition::GreaterThan,
                                         littleEndianBytes<float>(inf))
                    .empty());
    EXPECT_TRUE(scanner
                    .firstScanComparison(process, f, ScanCondition::LessThan,
                                         littleEndianBytes<float>(-inf))
                    .empty());
    // 严格小于 +Inf:全部有限值与 -Inf;+Inf 自身与 NaN 不保留(14 个)
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process, f, ScanCondition::LessThan, littleEndianBytes<float>(inf))),
              (std::vector<tpe::Address>{0x1000, 0x1004, 0x1008, 0x1010, 0x1018, 0x101C, 0x1020,
                                         0x1024, 0x1028, 0x102C, 0x1030, 0x1034, 0x1038, 0x103C}));
}

TEST(FirstScanComparison, DoubleFollowsIeeeAndNanIsNeverKept)
{
    // double:步进宽度 8;NaN 不保留
    FakeProcess process(0x1000, 0x40);
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double values[8] = {1.5, -1.5, inf, -inf, nan, 100.5, -100.5, 0.0};
    for (std::size_t slot = 0; slot < 8; ++slot) {
        putDoubleAtSlot(process.bytes(), slot, values[slot]);
    }
    MemoryScanner scanner;
    Double d;

    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process, d, ScanCondition::GreaterThan, littleEndianBytes<double>(100.0))),
              (std::vector<tpe::Address>{0x1010, 0x1028})); // +Inf / 100.5
    EXPECT_EQ(comparisonAddresses(scanner.firstScanComparison(
                  process, d, ScanCondition::LessThan, littleEndianBytes<double>(100.0))),
              (std::vector<tpe::Address>{0x1000, 0x1008, 0x1018, 0x1030, 0x1038})); // 1.5/-1.5/-Inf/-100.5/0.0;NaN 槽 0x1020 不保留
    EXPECT_TRUE(scanner
                    .firstScanComparison(process, d, ScanCondition::GreaterThan,
                                         littleEndianBytes<double>(nan))
                    .empty());
    EXPECT_TRUE(scanner
                    .firstScanComparison(process, d, ScanCondition::LessThan,
                                         littleEndianBytes<double>(nan))
                    .empty());
    EXPECT_TRUE(scanner
                    .firstScanComparison(process, d, ScanCondition::GreaterThan,
                                         littleEndianBytes<double>(inf))
                    .empty());
}

TEST(FirstScanComparison, SteppingMatchesTypeWidthOnAllOnesPage)
{
    // 64 字节页全 0x01:目标 0 的严格大于 = 全部候选,计数即步进面(镜像 T007)
    FakeProcess process(0x1000, 0x40);
    std::fill(process.bytes().begin(), process.bytes().end(), static_cast<tpe::Byte>(0x01));
    MemoryScanner scanner;

    UnsignedByte u8;
    Int16 i16;
    Int32 i32;
    Int64 i64;
    const struct {
        const ValueType* type;
        size_t width;
        size_t expected;
    } cases[] = {
        {&u8, 1, 64u},
        {&i16, 2, 32u},
        {&i32, 4, 16u},
        {&i64, 8, 8u},
    };

    for (const auto& item : cases) {
        const std::vector<ScanRecord> results = scanner.firstScanComparison(
            process, *item.type, ScanCondition::GreaterThan, tpe::Memory(item.width, 0));
        ASSERT_EQ(results.size(), item.expected) << item.type->name;
        for (size_t k = 0; k < results.size(); ++k) {
            EXPECT_EQ(results[k].address % item.width, 0u)
                << "match must be aligned to width, got " << results[k].address;
            if (k > 0) {
                EXPECT_EQ(results[k].address - results[k - 1].address, item.width)
                    << "matches must step by width";
                EXPECT_LT(results[k - 1].address, results[k].address) << "ascending order";
            }
        }
        EXPECT_EQ(results.front().address, 0x1000u);
        EXPECT_EQ(results.back().address, 0x1000u + 0x40 - item.width)
            << "不跨页:末候选的宽度字节须完整在页内";
    }
}

TEST(FirstScanComparison, RecordsSnapshotFromCurrentMemoryBytes)
{
    // 快照 = 命中处当轮实读 width 字节(INV-R;供 next-scan 过滤链)
    FakeProcess process(0x1000, 0x20);
    putI32AtSlot(process.bytes(), 1, 100);
    putI32AtSlot(process.bytes(), 2, 101);
    putI32AtSlot(process.bytes(), 3, 200);
    MemoryScanner scanner;
    Int32 i32;

    const std::vector<ScanRecord> results = scanner.firstScanComparison(
        process, i32, ScanCondition::GreaterThan, littleEndianBytes<std::int32_t>(100));
    ASSERT_EQ(results.size(), 2u); // 101 @0x1008、200 @0x100C;==100 不保留
    EXPECT_EQ(results[0].address, 0x1008u);
    EXPECT_EQ(results[1].address, 0x100Cu);
    for (const ScanRecord& rec : results) {
        EXPECT_EQ(rec.snapshot_size, 4u) << "address " << rec.address;
        const std::size_t offset = static_cast<std::size_t>(rec.address - 0x1000);
        for (std::size_t i = 0; i < 4; ++i) {
            EXPECT_EQ(rec.snapshot_data[i], process.bytes()[offset + i])
                << "address " << rec.address << " byte " << i;
        }
    }
    EXPECT_EQ(results[0].snapshot_data[0], 0x65); // 101 小端
    EXPECT_EQ(results[1].snapshot_data[0], 0xC8); // 200 小端
}

TEST(FirstScanComparison, ChunkBoundariesKeepComparisonWithoutDrift)
{
    // 62 字节页 → i16 槽位 31 个(值 = 槽号);小 chunkSize(含非宽度整数倍)与整页读取
    // 的地址与快照逐项一致:无漂移、无重复、无漏采,跨块比较正确
    FakeProcess process(0x1000, 0x3E);
    for (std::size_t slot = 0; slot < 31; ++slot) {
        putI16AtSlot(process.bytes(), slot, static_cast<std::int16_t>(slot));
    }
    MemoryScanner scanner;
    Int16 i16;
    const tpe::Memory target15 = littleEndianBytes<std::int16_t>(15);

    const std::vector<ScanRecord> fullGreater = scanner.firstScanComparison(
        process, i16, ScanCondition::GreaterThan, target15);
    ASSERT_EQ(fullGreater.size(), 15u); // 槽 16..30
    EXPECT_EQ(fullGreater.front().address, 0x1020u);
    EXPECT_EQ(fullGreater.back().address, 0x103Cu);

    const std::vector<ScanRecord> fullLess =
        scanner.firstScanComparison(process, i16, ScanCondition::LessThan, target15);
    ASSERT_EQ(fullLess.size(), 15u); // 槽 0..14
    EXPECT_EQ(fullLess.front().address, 0x1000u);
    EXPECT_EQ(fullLess.back().address, 0x101Cu);

    for (const tpe::Size chunkSize :
         {tpe::Size{16}, tpe::Size{8}, tpe::Size{7}, tpe::Size{3}, tpe::Size{1}}) {
        ScanOptions options;
        options.chunkSize = chunkSize;
        const std::vector<ScanRecord> chunkedGreater = scanner.firstScanComparison(
            process, i16, ScanCondition::GreaterThan, target15, options);
        const std::vector<ScanRecord> chunkedLess = scanner.firstScanComparison(
            process, i16, ScanCondition::LessThan, target15, options);
        ASSERT_EQ(chunkedGreater.size(), fullGreater.size()) << "chunkSize=" << chunkSize;
        ASSERT_EQ(chunkedLess.size(), fullLess.size()) << "chunkSize=" << chunkSize;
        for (size_t k = 0; k < fullGreater.size(); ++k) {
            EXPECT_EQ(chunkedGreater[k].address, fullGreater[k].address)
                << "chunkSize=" << chunkSize << " index " << k;
            ASSERT_EQ(chunkedGreater[k].snapshot_size, fullGreater[k].snapshot_size)
                << "chunkSize=" << chunkSize << " index " << k;
            EXPECT_EQ(std::memcmp(chunkedGreater[k].snapshot_data, fullGreater[k].snapshot_data,
                                  fullGreater[k].snapshot_size),
                      0)
                << "chunkSize=" << chunkSize << " index " << k;
        }
        for (size_t k = 0; k < fullLess.size(); ++k) {
            EXPECT_EQ(chunkedLess[k].address, fullLess[k].address)
                << "chunkSize=" << chunkSize << " index " << k;
        }
    }
}

TEST(FirstScanComparison, UnsupportedConditionsAndTypesReturnNoMatches)
{
    // R4/C-D3 防御:条件非 GT/LT、NumericKind::Other、无对齐口径(变长)或
    // 目标值不足以解码 → 一律不产生记录
    FakeProcess process(0x1000, 0x40);
    std::fill(process.bytes().begin(), process.bytes().end(), static_cast<tpe::Byte>(0x01));
    MemoryScanner scanner;
    Int32 i32;
    String string;
    OtherKindValueType other4;
    const tpe::Memory target = littleEndianBytes<std::int32_t>(0);

    for (const ScanCondition condition :
         {ScanCondition::ExactValue, ScanCondition::Unknown, ScanCondition::Changed,
          ScanCondition::Unchanged, ScanCondition::Increased, ScanCondition::Decreased}) {
        EXPECT_TRUE(scanner.firstScanComparison(process, i32, condition, target).empty());
    }
    EXPECT_TRUE(scanner.firstScanComparison(process, other4, ScanCondition::GreaterThan, target)
                    .empty());
    EXPECT_TRUE(scanner.firstScanComparison(process, string, ScanCondition::GreaterThan,
                                            tpe::Memory{0x01})
                    .empty());
    // 目标值不足 width(2 < 4)→ 无比较口径
    EXPECT_TRUE(scanner
                    .firstScanComparison(process, i32, ScanCondition::GreaterThan,
                                         tpe::Memory{0x01, 0x02})
                    .empty());
}

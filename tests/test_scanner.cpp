#include <gtest/gtest.h>
#include "MemoryScanner.h"
#include "ScanTypes.h"
#include "ValueType.h"
#include "AobPattern.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

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

    mutable int m_askCalls = 0;
};

/// 测试替身:单页内存缓冲进程(实现 Platform.h 的进程接口)。
class FakeProcess : public PlatformProcess {
public:
    FakeProcess(tpe::Address base, tpe::Size size)
        : PlatformProcess(static_cast<Pid_t>(4242)),
          m_base(base),
          m_size(size),
          m_bytes(static_cast<std::size_t>(size), 0) {}

    tpe::Memory& bytes() { return m_bytes; }

    std::vector<MemoryPage> getCheatablePages() const override
    {
        return {MemoryPage(m_base, m_size)};
    }

    Result<tpe::Memory, PlatformError> read(MemoryPage page) const override
    {
        if (!contains(page.start, page.size)) {
            return Result<tpe::Memory, PlatformError>::error(makeError());
        }
        const std::size_t offset = static_cast<std::size_t>(page.start - m_base);
        return Result<tpe::Memory, PlatformError>::success(
            tpe::Memory(m_bytes.begin() + offset, m_bytes.begin() + offset + page.size));
    }

    Result<void, PlatformError> write(tpe::Address address, const tpe::Memory& value) override
    {
        if (!contains(address, value.size())) {
            return Result<void, PlatformError>::error(makeError());
        }
        std::copy(value.begin(), value.end(),
                  m_bytes.begin() + static_cast<std::size_t>(address - m_base));
        return Result<void, PlatformError>::success();
    }

private:
    bool contains(tpe::Address address, tpe::Size length) const
    {
        if (address < m_base) return false;
        const tpe::Size offset = address - m_base;
        return offset <= m_size && length <= m_size - offset;
    }

    PlatformError makeError() const
    {
        return PlatformError{"FakeProcess", static_cast<unsigned long>(getPid()), 0, "out of range"};
    }

    tpe::Address m_base;
    tpe::Size m_size;
    tpe::Memory m_bytes;
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

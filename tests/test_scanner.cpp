#include <gtest/gtest.h>
#include "MemoryScanner.h"
#include "ScanTypes.h"
#include "ValueType.h"
#include "AobPattern.h"

#include <cstring>

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

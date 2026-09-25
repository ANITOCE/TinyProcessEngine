#include <gtest/gtest.h>
#include "Matches.h"

using tpe::Matches;
using tpe::MemoryPage;
using tpe::PageMatches;
using tpe::no_matches;

// ============================================================
// PageMatches tests
// ============================================================
TEST(PageMatchesTest, ConstructWithValidOffsets) {
    MemoryPage page(0x1000, 4096);
    std::vector<tpe::Offset> offsets = {0, 16, 32};
    PageMatches pm(page, offsets);

    EXPECT_EQ(pm.getPage().start, 0x1000u);
    EXPECT_EQ(pm.getPage().size, 4096u);
    EXPECT_EQ(pm.getOffsets().size(), 3u);
    EXPECT_EQ(pm.getOffsets()[0], 0u);
    EXPECT_EQ(pm.getOffsets()[2], 32u);
}

TEST(PageMatchesTest, EmptyOffsetsThrows) {
    MemoryPage page(0x2000, 1024);
    std::vector<tpe::Offset> empty;
    EXPECT_THROW(PageMatches(page, empty), no_matches);
}

// ============================================================
// Matches tests
// ============================================================
TEST(MatchesTest, AddPageOffsets) {
    Matches m;
    MemoryPage page(0x1000, 4096);
    std::vector<tpe::Offset> offsets = {8, 16, 24};
    m.add(page, offsets);

    EXPECT_TRUE(m.any());
    EXPECT_EQ(m.totalMatches(), 3u);

    const auto& pages = m.getPageMatches();
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0].getOffsets().size(), 3u);
}

TEST(MatchesTest, AddEmptyOffsetsIsNoOp) {
    Matches m;
    MemoryPage page(0x1000, 4096);
    std::vector<tpe::Offset> empty;
    m.add(page, empty); // Should not throw, just ignore

    EXPECT_FALSE(m.any());
    EXPECT_EQ(m.totalMatches(), 0u);
    EXPECT_TRUE(m.getPageMatches().empty());
}

TEST(MatchesTest, MultiplePagesAccumulate) {
    Matches m;
    MemoryPage page1(0x1000, 4096);
    MemoryPage page2(0x2000, 2048);

    m.add(page1, {4, 8});
    m.add(page2, {16});

    EXPECT_TRUE(m.any());
    EXPECT_EQ(m.totalMatches(), 3u);
    EXPECT_EQ(m.getPageMatches().size(), 2u);
}

TEST(MatchesTest, AnyReturnsFalseWhenEmpty) {
    Matches m;
    EXPECT_FALSE(m.any());
    EXPECT_EQ(m.totalMatches(), 0u);
}

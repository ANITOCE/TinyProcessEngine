#include <gtest/gtest.h>
#include "ScanTypes.h"
#include "ResultStorage.h"

#include <filesystem>
#include <cstring>

// ============================================================
// ResultStorage — Append + Read Back
// ============================================================
TEST(ResultStorageTest, AppendAndReadBack) {
    ResultStorage storage;
    EXPECT_EQ(storage.totalCount(), 0u);
    EXPECT_EQ(storage.backend(), StorageBackend::InMemory);

    ScanRecord rec(0x7FFE1234ULL);
    storage.append(rec);

    EXPECT_EQ(storage.totalCount(), 1u);

    auto result = storage.readAt(0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->address, 0x7FFE1234ULL);
    EXPECT_EQ(result->snapshot_size, 0u);
}

TEST(ResultStorageTest, ReadAtOutOfRange) {
    ResultStorage storage;
    auto result = storage.readAt(0);
    EXPECT_FALSE(result.has_value());

    storage.append(ScanRecord(0x1000));
    result = storage.readAt(1);
    EXPECT_FALSE(result.has_value());
}

// ============================================================
// ResultStorage — Chunk Boundary Test
// ============================================================
TEST(ResultStorageTest, ChunkBoundary) {
    ResultStorage storage;

    // Fill exactly one chunk (4096 records)
    for (size_t i = 0; i < CHUNK_RECORDS; ++i) {
        storage.append(ScanRecord(static_cast<tpe::Address>(0x1000 + i)));
    }
    EXPECT_EQ(storage.totalCount(), static_cast<uint64_t>(CHUNK_RECORDS));
    EXPECT_EQ(storage.chunkCount(), 1u);

    // Append one more — should trigger new chunk
    storage.append(ScanRecord(0x2000));
    EXPECT_EQ(storage.totalCount(), static_cast<uint64_t>(CHUNK_RECORDS + 1));
    EXPECT_EQ(storage.chunkCount(), 2u);

    // Verify records at chunk boundary
    auto lastOfFirst = storage.readAt(CHUNK_RECORDS - 1);
    ASSERT_TRUE(lastOfFirst.has_value());
    EXPECT_EQ(lastOfFirst->address, 0x1000ULL + CHUNK_RECORDS - 1);

    auto firstOfSecond = storage.readAt(CHUNK_RECORDS);
    ASSERT_TRUE(firstOfSecond.has_value());
    EXPECT_EQ(firstOfSecond->address, 0x2000ULL);
}

// ============================================================
// ResultStorage — Disk Migration: Preserves Data
// ============================================================
TEST(ResultStorageTest, DiskMigration_PreservesData) {
    ResultStorage storage;

    // Add 100 records
    for (size_t i = 0; i < 100; ++i) {
        storage.append(ScanRecord(static_cast<tpe::Address>(0xAA00 + i)));
    }
    uint64_t totalBefore = storage.totalCount();
    EXPECT_EQ(totalBefore, 100u);

    // Migrate to disk
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    auto err = storage.migrateToDisk(tempDir);
    ASSERT_FALSE(err.has_value()) << "Migration error: " << (err.value_or(""));

    EXPECT_TRUE(storage.isDiskBacked());
    EXPECT_EQ(storage.totalCount(), totalBefore);

    // Verify spot checks
    auto r0 = storage.readAt(0);
    ASSERT_TRUE(r0.has_value());
    EXPECT_EQ(r0->address, 0xAA00ULL);

    auto r50 = storage.readAt(50);
    ASSERT_TRUE(r50.has_value());
    EXPECT_EQ(r50->address, 0xAA00ULL + 50);

    auto r99 = storage.readAt(99);
    ASSERT_TRUE(r99.has_value());
    EXPECT_EQ(r99->address, 0xAA00ULL + 99);
}

TEST(ResultStorageTest, DiskMigration_ReadAt) {
    ResultStorage storage;
    for (size_t i = 0; i < 50; ++i) {
        storage.append(ScanRecord(static_cast<tpe::Address>(i * 8)));
    }
    auto err = storage.migrateToDisk(std::filesystem::temp_directory_path());
    ASSERT_FALSE(err.has_value());

    for (uint64_t i = 0; i < 50; ++i) {
        auto rec = storage.readAt(i);
        ASSERT_TRUE(rec.has_value()) << "Missing record at index " << i;
        EXPECT_EQ(rec->address, static_cast<tpe::Address>(i * 8));
    }
}

// ============================================================
// ResultStorage — Disk Space Check
// ============================================================
TEST(ResultStorageTest, DiskSpaceCheck_RejectsWhenLow) {
    // Test with a path that has 0 available space (NUL on Windows, /dev/full on Linux)
    // This is a best-effort test — on Windows, "NUL" won't work for space query
    // We verify that the error path doesn't crash and returns a message
    ResultStorage storage;
    storage.append(ScanRecord(0x100));

    // Use a non-existent path with special chars — should fail gracefully
    auto err = storage.migrateToDisk("Z:\\nonexistent_drive_xyz\\path");
    EXPECT_TRUE(err.has_value()) << "Should fail for non-existent drive";
}

// ============================================================
// ResultStorage — Round Trip (1000 records)
// ============================================================
TEST(ResultStorageTest, ThousandRecords_RoundTrip) {
    ResultStorage storage;
    constexpr uint64_t N = 1000;

    for (uint64_t i = 0; i < N; ++i) {
        uint8_t snap[8];
        for (int j = 0; j < 8; ++j) snap[j] = static_cast<uint8_t>((i >> (j * 8)) & 0xFF);
        tpe::Memory snapVec(snap, snap + 8);
        storage.append(ScanRecord(static_cast<tpe::Address>(0x10000 + i), snapVec));
    }

    EXPECT_EQ(storage.totalCount(), N);

    for (uint64_t i = 0; i < N; i += 100) {
        auto rec = storage.readAt(i);
        ASSERT_TRUE(rec.has_value());
        EXPECT_EQ(rec->address, 0x10000ULL + i);
        EXPECT_EQ(rec->snapshot_size, 8u);
    }
}

// ============================================================
// ResultStorage — ReadChunk with partial last chunk
// ============================================================
TEST(ResultStorageTest, ReadChunk_PartialLastChunk) {
    ResultStorage storage;
    size_t N = CHUNK_RECORDS + 100; // 1 full chunk + 100 in second chunk

    for (size_t i = 0; i < N; ++i) {
        storage.append(ScanRecord(static_cast<tpe::Address>(i)));
    }

    auto chunk0 = storage.readChunk(0);
    EXPECT_EQ(chunk0.size(), CHUNK_RECORDS);

    auto chunk1 = storage.readChunk(1);
    EXPECT_EQ(chunk1.size(), 100u);
}

// ============================================================
// ResultStorage — Append With Snapshot
// ============================================================
TEST(ResultStorageTest, AppendWithSnapshot) {
    ResultStorage storage;
    tpe::Memory snap = {0xDE, 0xAD, 0xBE, 0xEF};
    storage.append(ScanRecord(0x4000, snap));

    auto rec = storage.readAt(0);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->snapshot_size, 4u);
    EXPECT_EQ(rec->snapshot_data[0], 0xDE);
    EXPECT_EQ(rec->snapshot_data[1], 0xAD);
    EXPECT_EQ(rec->snapshot_data[2], 0xBE);
    EXPECT_EQ(rec->snapshot_data[3], 0xEF);
    EXPECT_TRUE(rec->hasSnapshot());
}

TEST(ResultStorageTest, AppendWithoutSnapshot) {
    ResultStorage storage;
    storage.append(ScanRecord(0x5000));

    auto rec = storage.readAt(0);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->snapshot_size, 0u);
    EXPECT_FALSE(rec->hasSnapshot());
}

// ============================================================
// ScanSession — State machine and lifecycle tests
// Note: These tests use a null process since ScanSession
// doesn't call process methods until scan/read/write operations
// ============================================================

#include "ScanSession.h"

// Mock ValueType for testing
struct MockValueType : public ValueType {
    MockValueType() : ValueType("mock_int32") {}
    tpe::Memory askValue() const override {
        return {0x42, 0x00, 0x00, 0x00};  // value 66 as int32 LE
    }
};

static std::shared_ptr<PlatformProcess> makeNullProcess() {
    return nullptr;  // Used for state machine tests only
}

TEST(ScanSessionTest, InitialStateIsIdle) {
    ScanSession session(makeNullProcess());
    EXPECT_EQ(session.state(), SessionState::Idle);
    EXPECT_EQ(session.round(), 0u);
    EXPECT_EQ(session.resultCount(), 0u);
    EXPECT_FALSE(session.canUndo());
}

TEST(ScanSessionTest, BeginScanThenCommitFirstScan) {
    ScanSession session(makeNullProcess());
    MockValueType mockType;
    
    session.beginScan(mockType);
    EXPECT_EQ(session.state(), SessionState::Scanning);

    std::vector<ScanRecord> results;
    results.emplace_back(0x1000ULL);
    results.emplace_back(0x2000ULL);
    session.commitFirstScan(std::move(results));

    EXPECT_EQ(session.state(), SessionState::Ready);
    EXPECT_EQ(session.round(), 1u);
    EXPECT_EQ(session.resultCount(), 2u);
    EXPECT_FALSE(session.canUndo());

    auto r0 = session.resultAt(0);
    ASSERT_TRUE(r0.has_value());
    EXPECT_EQ(r0->address, 0x1000ULL);
}

TEST(ScanSessionTest, CommitNextScanEnablesUndo) {
    ScanSession session(makeNullProcess());
    MockValueType mockType;
    session.beginScan(mockType);

    std::vector<ScanRecord> r1;
    r1.emplace_back(0x1000ULL);
    session.commitFirstScan(std::move(r1));
    EXPECT_FALSE(session.canUndo());

    std::vector<ScanRecord> r2;
    r2.emplace_back(0x2000ULL);
    session.commitNextScan(ScanCondition::Changed, std::move(r2));

    EXPECT_EQ(session.round(), 2u);
    EXPECT_EQ(session.resultCount(), 1u);
    EXPECT_TRUE(session.canUndo());
}

TEST(ScanSessionTest, UndoRevertsToPreviousRound) {
    ScanSession session(makeNullProcess());
    MockValueType mockType;
    session.beginScan(mockType);

    // Round 1: 3 results
    std::vector<ScanRecord> r1;
    r1.emplace_back(0x1000ULL);
    r1.emplace_back(0x2000ULL);
    r1.emplace_back(0x3000ULL);
    session.commitFirstScan(std::move(r1));

    // Round 2: filtered to 1 result
    std::vector<ScanRecord> r2;
    r2.emplace_back(0x2000ULL);
    session.commitNextScan(ScanCondition::Changed, std::move(r2));
    EXPECT_EQ(session.resultCount(), 1u);
    EXPECT_EQ(session.round(), 2u);

    // Undo back to round 1
    session.undo();
    EXPECT_EQ(session.round(), 1u);
    EXPECT_EQ(session.resultCount(), 3u);
    EXPECT_FALSE(session.canUndo());
}

TEST(ScanSessionTest, UndoAtFirstRoundThrows) {
    ScanSession session(makeNullProcess());
    MockValueType mockType;
    session.beginScan(mockType);

    std::vector<ScanRecord> r1;
    r1.emplace_back(0x1000ULL);
    session.commitFirstScan(std::move(r1));

    EXPECT_THROW(session.undo(), std::runtime_error);
}

TEST(ScanSessionTest, CloseResetsState) {
    ScanSession session(makeNullProcess());
    MockValueType mockType;
    session.beginScan(mockType);

    std::vector<ScanRecord> r1;
    r1.emplace_back(0x1000ULL);
    session.commitFirstScan(std::move(r1));

    session.close();
    EXPECT_EQ(session.state(), SessionState::Idle);
    EXPECT_EQ(session.round(), 0u);
    EXPECT_EQ(session.resultCount(), 0u);
}

TEST(ScanSessionTest, ResultAtOutOfRange) {
    ScanSession session(makeNullProcess());
    MockValueType mockType;
    session.beginScan(mockType);

    std::vector<ScanRecord> r1;
    r1.emplace_back(0x1000ULL);
    session.commitFirstScan(std::move(r1));

    auto missing = session.resultAt(999);
    EXPECT_FALSE(missing.has_value());
}

TEST(ScanSessionTest, ExportThrowsOnEmptyResults) {
    ScanSession session(makeNullProcess());
    EXPECT_THROW(session.exportTo("test.txt", "txt"), std::runtime_error);
}

TEST(ScanSessionTest, ExportCreatesFile) {
    ScanSession session(makeNullProcess());
    MockValueType mockType;
    session.beginScan(mockType);

    std::vector<ScanRecord> r1;
    r1.emplace_back(0x1000ULL);
    r1.emplace_back(0x2000ULL);
    session.commitFirstScan(std::move(r1));

    auto tmpPath = std::filesystem::temp_directory_path() / "tpe_test_export1.csv";
    std::error_code ec;
    std::filesystem::remove(tmpPath, ec);

    EXPECT_NO_THROW(session.exportTo(tmpPath, "csv"));
    EXPECT_TRUE(std::filesystem::exists(tmpPath));
    std::filesystem::remove(tmpPath, ec);
}

TEST(ScanSessionTest, ExportTextFormat) {
    ScanSession session(makeNullProcess());
    MockValueType mockType;
    session.beginScan(mockType);

    std::vector<ScanRecord> r1;
    r1.emplace_back(0xABCD);
    session.commitFirstScan(std::move(r1));

    auto tmpPath = std::filesystem::temp_directory_path() / "tpe_test_export2.txt";
    // Remove if exists from previous run
    std::error_code ec;
    std::filesystem::remove(tmpPath, ec);

    EXPECT_NO_THROW(session.exportTo(tmpPath, "txt"));
    EXPECT_TRUE(std::filesystem::exists(tmpPath));

    std::ifstream file(tmpPath);
    EXPECT_TRUE(file.is_open());
    std::string line;
    std::getline(file, line);
    EXPECT_FALSE(line.empty());

    file.close();
    std::filesystem::remove(tmpPath, ec);
}

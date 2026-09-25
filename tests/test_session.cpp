#include <gtest/gtest.h>
#include "ScanTypes.hpp"
#include "ResultStorage.hpp"

#include <filesystem>
#include <cstring>
#include <string>
#include <system_error>

using tpe::CHUNK_RECORDS;
using tpe::ResultStorage;
using tpe::ScanRecord;
using tpe::StorageBackend;

// Count temp files created by ResultStorage disk migration (tpe_scan_*.tmp)
static size_t countTempScanFiles() {
    size_t count = 0;
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) return 0;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        const auto& entry = *it;
        std::error_code entryEc;
        if (entry.is_regular_file(entryEc) && !entryEc) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("tpe_scan_", 0) == 0 && entry.path().extension() == ".tmp") {
                ++count;
            }
        }
        it.increment(ec);
    }
    return count;
}

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

#include "ScanSession.hpp"

using tpe::ScanCondition;
using tpe::ScanSession;
using tpe::SessionState;
using tpe::ValueType;
using tpe::platform::PlatformProcess;

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

// ============================================================
// US4 (缺陷⑤) — ResultStorage move semantics
// ============================================================
TEST(ResultStorageTest, MoveAssignmentReleasesPreviousResources) {
    const auto tempDir = std::filesystem::temp_directory_path();

    ResultStorage a;
    for (size_t i = 0; i < 3; ++i) {
        a.append(ScanRecord(static_cast<tpe::Address>(0xA000 + i)));
    }
    auto errA = a.migrateToDisk(tempDir);
    ASSERT_FALSE(errA.has_value()) << errA.value_or("");
    const auto oldPathA = a.diskPath();
    ASSERT_FALSE(oldPathA.empty());
    ASSERT_TRUE(std::filesystem::exists(oldPathA));

    ResultStorage b;
    for (size_t i = 0; i < 2; ++i) {
        b.append(ScanRecord(static_cast<tpe::Address>(0xB000 + i)));
    }
    auto errB = b.migrateToDisk(tempDir);
    ASSERT_FALSE(errB.has_value()) << errB.value_or("");
    const auto pathB = b.diskPath();
    ASSERT_FALSE(pathB.empty());
    ASSERT_TRUE(std::filesystem::exists(pathB));

    a = std::move(b);

    // Move assignment must release the previous resources of the target
    EXPECT_FALSE(std::filesystem::exists(oldPathA))
        << "move assignment must delete the previous temp file of the target";

    // Target now owns the source resources
    EXPECT_EQ(a.totalCount(), 2u);
    EXPECT_TRUE(a.isDiskBacked());
    EXPECT_EQ(a.diskPath(), pathB);
    auto a0 = a.readAt(0);
    ASSERT_TRUE(a0.has_value());
    EXPECT_EQ(a0->address, 0xB000ULL);
    auto a1 = a.readAt(1);
    ASSERT_TRUE(a1.has_value());
    EXPECT_EQ(a1->address, 0xB001ULL);

    // Source is left empty
    EXPECT_EQ(b.totalCount(), 0u);
    EXPECT_EQ(b.backend(), StorageBackend::InMemory);
    EXPECT_FALSE(b.isDiskBacked());
    EXPECT_TRUE(b.diskPath().empty());

    // Red-run hygiene: remove leaked file if move assignment failed to do so
    std::error_code cleanupEc;
    std::filesystem::remove(oldPathA, cleanupEc);
}

// ============================================================
// US4 (缺陷⑤) — Disk backend threshold (FR-013)
//   exactly 1,000,000 records stay in memory;
//   the 1,000,001st record switches to disk before being stored
// ============================================================
TEST(ScanSessionTest, SessionMigratesToDiskBeyondThreshold) {
    constexpr uint64_t kThreshold = 1'000'000;

    // Boundary: exactly 1,000,000 records -> in-memory, no temp file created
    const size_t filesBefore = countTempScanFiles();
    {
        ScanSession session(makeNullProcess());
        MockValueType mockType;
        session.beginScan(mockType);

        std::vector<ScanRecord> results;
        results.reserve(static_cast<size_t>(kThreshold));
        for (uint64_t i = 0; i < kThreshold; ++i) {
            results.emplace_back(static_cast<tpe::Address>(0x10000 + i));
        }
        session.commitFirstScan(std::move(results));

        EXPECT_EQ(session.resultCount(), kThreshold);
        EXPECT_FALSE(session.isDiskBacked());
        EXPECT_EQ(countTempScanFiles(), filesBefore);

        auto first = session.resultAt(0);
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(first->address, static_cast<tpe::Address>(0x10000));
        auto last = session.resultAt(kThreshold - 1);
        ASSERT_TRUE(last.has_value());
        EXPECT_EQ(last->address, static_cast<tpe::Address>(0x10000 + kThreshold - 1));
    }
    EXPECT_EQ(countTempScanFiles(), filesBefore);

    // Beyond threshold: 1,000,001 records -> disk-backed, one temp file exists
    {
        ScanSession session(makeNullProcess());
        MockValueType mockType;
        session.beginScan(mockType);

        std::vector<ScanRecord> results;
        results.reserve(static_cast<size_t>(kThreshold) + 1);
        for (uint64_t i = 0; i <= kThreshold; ++i) {
            results.emplace_back(static_cast<tpe::Address>(0x20000 + i));
        }
        session.commitFirstScan(std::move(results));

        EXPECT_EQ(session.resultCount(), kThreshold + 1);
        EXPECT_TRUE(session.isDiskBacked());
        EXPECT_EQ(countTempScanFiles(), filesBefore + 1);

        auto first = session.resultAt(0);
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(first->address, static_cast<tpe::Address>(0x20000));
        auto last = session.resultAt(kThreshold);
        ASSERT_TRUE(last.has_value());
        EXPECT_EQ(last->address, static_cast<tpe::Address>(0x20000 + kThreshold));
    }
    // Session destructor must delete the temp file
    EXPECT_EQ(countTempScanFiles(), filesBefore);
}

// ============================================================
// US4 (缺陷⑤) — Undo across backends (FR-014/FR-015)
// ============================================================
TEST(ScanSessionTest, SessionUndoAcrossBackendsKeepsContentAndCleansFiles) {
    constexpr uint64_t kBig = 1'000'001;
    const size_t filesBefore = countTempScanFiles();

    ScanSession session(makeNullProcess());
    MockValueType mockType;
    session.beginScan(mockType);

    // Round 1: small in-memory result set
    std::vector<ScanRecord> r1;
    r1.emplace_back(ScanRecord(0x1000));
    r1.emplace_back(ScanRecord(0x2000));
    r1.emplace_back(ScanRecord(0x3000));
    session.commitFirstScan(std::move(r1));
    EXPECT_FALSE(session.isDiskBacked());

    // Round 2: beyond threshold -> disk backend
    std::vector<ScanRecord> r2;
    r2.reserve(static_cast<size_t>(kBig));
    for (uint64_t i = 0; i < kBig; ++i) {
        r2.emplace_back(static_cast<tpe::Address>(0x40000 + i));
    }
    session.commitNextScan(ScanCondition::Changed, std::move(r2));
    EXPECT_TRUE(session.isDiskBacked());
    EXPECT_EQ(session.resultCount(), kBig);
    EXPECT_EQ(countTempScanFiles(), filesBefore + 1);

    // Undo: content returns to round 1; round-2 disk file is deleted
    session.undo();
    EXPECT_FALSE(session.isDiskBacked());
    EXPECT_EQ(session.resultCount(), 3u);
    auto u0 = session.resultAt(0);
    ASSERT_TRUE(u0.has_value());
    EXPECT_EQ(u0->address, 0x1000ULL);
    auto u2 = session.resultAt(2);
    ASSERT_TRUE(u2.has_value());
    EXPECT_EQ(u2->address, 0x3000ULL);
    EXPECT_FALSE(session.resultAt(3).has_value());
    EXPECT_EQ(countTempScanFiles(), filesBefore);

    // close() releases everything with no residue
    session.close();
    EXPECT_EQ(session.resultCount(), 0u);
    EXPECT_EQ(countTempScanFiles(), filesBefore);

    // beginScan() after a disk-backed round also cleans up
    ScanSession session2(makeNullProcess());
    session2.beginScan(mockType);
    std::vector<ScanRecord> r3;
    r3.reserve(static_cast<size_t>(kBig));
    for (uint64_t i = 0; i < kBig; ++i) {
        r3.emplace_back(static_cast<tpe::Address>(0x80000 + i));
    }
    session2.commitFirstScan(std::move(r3));
    EXPECT_TRUE(session2.isDiskBacked());
    EXPECT_EQ(countTempScanFiles(), filesBefore + 1);

    session2.beginScan(mockType);
    EXPECT_EQ(session2.resultCount(), 0u);
    EXPECT_FALSE(session2.isDiskBacked());
    EXPECT_EQ(countTempScanFiles(), filesBefore);
}

// ============================================================
// US6(缺陷⑧)— 平台枚举替身与枚举失败信号(FR-023; C-P5)
// ============================================================
#include "Platform.hpp"
#include "ProcessEngine.hpp"

#include <memory>
#include <utility>
#include <vector>

using tpe::MemoryPage;
using tpe::PlatformError;
using tpe::ProcessEngine;
using tpe::Result;
using tpe::platform::Pid_t;
using tpe::platform::PlatformOS;

namespace {

/// 伪进程:仅承载 PID 与名称(引擎列表展示路径)。
class FakePlatformProcess : public PlatformProcess {
public:
    FakePlatformProcess(Pid_t pid, std::string name)
        : PlatformProcess(pid, std::move(name)) {}

    std::vector<MemoryPage> getCheatablePages() const override { return {}; }

    Result<tpe::Memory, PlatformError> read(MemoryPage) const override
    {
        return Result<tpe::Memory, PlatformError>::error(notSupported());
    }

    Result<void, PlatformError> write(tpe::Address, const tpe::Memory&) override
    {
        return Result<void, PlatformError>::error(notSupported());
    }

private:
    PlatformError notSupported() const
    {
        return PlatformError{"FakePlatformProcess",
                             static_cast<unsigned long>(getPid()), 0, "not supported"};
    }
};

/// 平台替身(US6/缺陷⑧):可配置进程列表 / 枚举错误 / open 结果。
class FakePlatformOS : public PlatformOS {
public:
    void setProcessList(std::vector<std::pair<Pid_t, std::string>> entries)
    {
        ProcessList.clear();
        for (const auto& entry : entries) {
            ProcessList.push_back(
                std::make_shared<FakePlatformProcess>(entry.first, entry.second));
        }
    }

    /// 模拟平台枚举失败:平台实现于失败时填充 enumerationError(缺陷⑧契约)。
    void setEnumerationError(PlatformError err) { m_enumerationError = std::move(err); }

    void setOpenResult(std::shared_ptr<PlatformProcess> process)
    {
        m_openResult = std::move(process);
    }

    std::shared_ptr<PlatformProcess> open(Pid_t) override { return m_openResult; }

    Result<std::vector<Pid_t>, PlatformError> getAllProcessesPid() override
    {
        if (m_enumerationError.has_value()) {
            return Result<std::vector<Pid_t>, PlatformError>::error(*m_enumerationError);
        }
        std::vector<Pid_t> pids;
        for (const auto& process : ProcessList) {
            pids.push_back(process->getPid());
        }
        return Result<std::vector<Pid_t>, PlatformError>::success(std::move(pids));
    }

    void getAllProcesses(std::vector<Pid_t>) override {}

private:
    std::shared_ptr<PlatformProcess> m_openResult;
};

} // namespace

/// FR-023/C-P5:平台枚举失败必须作为可判定信号上达引擎,不得以空列表伪装成功。
TEST(ProcessEngineTest, GetProcessListSurfacesEnumerationFailure)
{
    auto os = std::make_unique<FakePlatformOS>();
    os->setEnumerationError(
        PlatformError{"EnumProcesses", 0, 5, "EnumProcesses failed (simulated)"});
    ProcessEngine engine(std::move(os));

    const Result<void, PlatformError> listed = engine.getProcessList();

    EXPECT_FALSE(listed.has_value()) << "enumeration failure must surface as a Result error";
    if (!listed.has_value()) {
        EXPECT_EQ(listed.error().message, "EnumProcesses failed (simulated)");
    }
}

#ifndef TPE_RESULT_STORAGE_H_
#define TPE_RESULT_STORAGE_H_

#include "ScanTypes.h"

#include <vector>
#include <array>
#include <cstdint>
#include <string>
#include <filesystem>
#include <optional>
#include <fstream>

// ============================================================
// ResultStorage — 扫描结果的分块存储
//
// 内存模式: vector<array<ScanRecord, 4096>>
// 磁盘模式: 顺序文件 (record_size=17 bytes fixed)
//
// 切换阈值: totalCount > 1,000,000
// ============================================================

// Constants
constexpr size_t CHUNK_RECORDS   = 4096;
constexpr size_t CHUNK_SIZE_BYTES = CHUNK_RECORDS * sizeof(ScanRecord); // ~69 KB
constexpr uint64_t DISK_BACKEND_THRESHOLD = 1'000'000;

enum class StorageBackend {
    InMemory,
    OnDisk,
};

/// Index entry for a chunk stored on disk.
struct ChunkIndex {
    std::streamoff fileOffset;      // byte offset in data file
    uint32_t       recordCount;     // number of valid records in this chunk (≤ CHUNK_RECORDS)
};

class ResultStorage {
public:
    ResultStorage() = default;
    ~ResultStorage();

    // Non-copyable; movable
    ResultStorage(const ResultStorage&) = delete;
    ResultStorage& operator=(const ResultStorage&) = delete;
    ResultStorage(ResultStorage&&) noexcept = default;
    ResultStorage& operator=(ResultStorage&&) noexcept = default;

    // ── Write ──

    /// Append a single record. Automatically creates new chunk when current is full.
    void append(const ScanRecord& record);

    // ── Read ──

    /// Read record by global index (0-based). Returns std::nullopt if index out of range.
    std::optional<ScanRecord> readAt(uint64_t index) const;

    /// Read an entire chunk by chunk index. Returns empty vector if chunkIndex out of range.
    std::vector<ScanRecord> readChunk(size_t chunkIndex) const;

    // ── Metadata ──

    uint64_t totalCount() const { return m_totalCount; }
    size_t chunkCount() const { return m_chunkCount; }
    StorageBackend backend() const { return m_backend; }

    /// Whether the current backend is disk-backed.
    bool isDiskBacked() const { return m_backend == StorageBackend::OnDisk; }

    /// Path to data file, only valid when OnDisk.
    std::filesystem::path diskPath() const { return m_diskPath.value_or(""); }

    // ── Backend Migration ──

    /// Migrate from InMemory to OnDisk. No-op if already OnDisk.
    /// Creates temp file in given directory.
    /// Returns error string on failure, empty optional on success.
    std::optional<std::string> migrateToDisk(const std::filesystem::path& tempDir);

private:
    StorageBackend m_backend = StorageBackend::InMemory;

    // In-memory storage
    std::vector<std::array<ScanRecord, CHUNK_RECORDS>> m_memoryChunks;

    // On-disk storage
    std::optional<std::filesystem::path> m_diskPath;
    std::vector<ChunkIndex>              m_diskIndex;
    std::fstream                         m_file;  // open while OnDisk

    size_t   m_chunkCount = 0;
    uint64_t m_totalCount = 0;

    void writeRecordToDisk(const ScanRecord& record);
    ScanRecord readRecordFromDisk(std::streamoff offset) const;
};

#endif // TPE_RESULT_STORAGE_H_

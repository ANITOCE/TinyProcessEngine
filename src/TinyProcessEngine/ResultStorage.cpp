#include "ResultStorage.h"

#include <cstring>
#include <chrono>
#include <iostream>

// ============================================================
// releaseResources — close + delete temp file, reset to empty
// ============================================================
void ResultStorage::releaseResources() {
    if (m_file.is_open()) {
        m_file.close();
    }
    if (m_diskPath.has_value()) {
        std::error_code ec;
        std::filesystem::remove(*m_diskPath, ec);
    }
    m_memoryChunks.clear();
    m_diskIndex.clear();
    m_diskPath.reset();
    m_backend = StorageBackend::InMemory;
    m_chunkCount = 0;
    m_totalCount = 0;
}

// ============================================================
// Move operations — take over the source's resources; the target
// first releases its own resources (close + delete) so nothing
// leaks; the source is left in an empty InMemory state.
// ============================================================
ResultStorage::ResultStorage(ResultStorage&& other) noexcept
    : m_backend(other.m_backend),
      m_memoryChunks(std::move(other.m_memoryChunks)),
      m_diskPath(std::move(other.m_diskPath)),
      m_diskIndex(std::move(other.m_diskIndex)),
      m_file(std::move(other.m_file)),
      m_chunkCount(other.m_chunkCount),
      m_totalCount(other.m_totalCount) {
    other.m_memoryChunks.clear();
    other.m_diskIndex.clear();
    other.m_diskPath.reset();
    other.m_backend = StorageBackend::InMemory;
    other.m_chunkCount = 0;
    other.m_totalCount = 0;
    if (other.m_file.is_open()) {
        other.m_file.close();
    }
}

ResultStorage& ResultStorage::operator=(ResultStorage&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    // Release current resources first (the previous temp file must be deleted)
    releaseResources();

    // Take over the source's resources
    m_backend = other.m_backend;
    m_memoryChunks = std::move(other.m_memoryChunks);
    m_diskPath = std::move(other.m_diskPath);
    m_diskIndex = std::move(other.m_diskIndex);
    m_file = std::move(other.m_file);
    m_chunkCount = other.m_chunkCount;
    m_totalCount = other.m_totalCount;

    // Leave the source empty
    other.m_memoryChunks.clear();
    other.m_diskIndex.clear();
    other.m_diskPath.reset();
    other.m_backend = StorageBackend::InMemory;
    other.m_chunkCount = 0;
    other.m_totalCount = 0;
    if (other.m_file.is_open()) {
        other.m_file.close();
    }

    return *this;
}

// ============================================================
// Destructor — clean up temp file if disk-backed
// ============================================================
ResultStorage::~ResultStorage() {
    releaseResources();
}

// ============================================================
// append — add one record; auto-create chunks
// ============================================================
void ResultStorage::append(const ScanRecord& record) {
    if (m_backend == StorageBackend::InMemory) {
        // Auto-switch to disk if threshold exceeded
        if (m_totalCount >= DISK_BACKEND_THRESHOLD) {
            std::filesystem::path tempDir = std::filesystem::temp_directory_path();
            auto err = migrateToDisk(tempDir);
            if (err.has_value()) {
                std::cerr << "Warning: migration to disk failed: " << *err
                          << " — continuing in memory\n";
                // fall through to in-memory append
            }
        }
    }

    if (m_backend == StorageBackend::InMemory) {
        size_t chunkIdx = m_totalCount / CHUNK_RECORDS;
        size_t offsetInChunk = m_totalCount % CHUNK_RECORDS;

        if (chunkIdx >= m_memoryChunks.size()) {
            m_memoryChunks.emplace_back();
            ++m_chunkCount;
        }
        m_memoryChunks[chunkIdx][offsetInChunk] = record;
        ++m_totalCount;
    } else {
        writeRecordToDisk(record);
        ++m_totalCount;
        // update last chunk record count
        if (!m_diskIndex.empty()) {
            m_diskIndex.back().recordCount =
                static_cast<uint32_t>(m_totalCount % CHUNK_RECORDS);
            if (m_diskIndex.back().recordCount == 0) {
                m_diskIndex.back().recordCount = CHUNK_RECORDS;
            }
        }
    }
}

// ============================================================
// readAt — O(1) random access by global index
// ============================================================
std::optional<ScanRecord> ResultStorage::readAt(uint64_t index) const {
    if (index >= m_totalCount) return std::nullopt;

    size_t chunkIdx = static_cast<size_t>(index / CHUNK_RECORDS);
    size_t offsetInChunk = static_cast<size_t>(index % CHUNK_RECORDS);

    if (m_backend == StorageBackend::InMemory) {
        if (chunkIdx >= m_memoryChunks.size()) return std::nullopt;
        return m_memoryChunks[chunkIdx][offsetInChunk];
    } else {
        if (chunkIdx >= m_diskIndex.size()) return std::nullopt;
        std::streamoff fileOffset = m_diskIndex[chunkIdx].fileOffset +
                                     static_cast<std::streamoff>(offsetInChunk * sizeof(ScanRecord));
        return readRecordFromDisk(fileOffset);
    }
}

// ============================================================
// readChunk — read entire chunk (for streaming filter)
// ============================================================
std::vector<ScanRecord> ResultStorage::readChunk(size_t chunkIndex) const {
    std::vector<ScanRecord> result;
    if (chunkIndex >= m_chunkCount) return result;

    uint64_t startIndex = chunkIndex * CHUNK_RECORDS;
    uint64_t endIndex = (std::min)(startIndex + CHUNK_RECORDS, m_totalCount);

    result.reserve(static_cast<size_t>(endIndex - startIndex));
    for (uint64_t i = startIndex; i < endIndex; ++i) {
        auto rec = readAt(i);
        if (rec.has_value()) {
            result.push_back(*rec);
        }
    }
    return result;
}

// ============================================================
// migrateToDisk — flush all in-memory chunks to temp file
// ============================================================
std::optional<std::string> ResultStorage::migrateToDisk(const std::filesystem::path& tempDir) {
    if (m_backend == StorageBackend::OnDisk) return std::nullopt; // already on disk

    // Check disk space (FR-030)
    std::error_code ec;
    auto spaceInfo = std::filesystem::space(tempDir, ec);
    if (ec) {
        return "Cannot query disk space: " + ec.message();
    }
    // Estimate: each record = 17 bytes, plus overhead
    uint64_t estimatedBytes = m_totalCount * sizeof(ScanRecord) + (m_chunkCount * 64);
    if (spaceInfo.available < estimatedBytes + (100ULL * 1024 * 1024)) {
        return "Insufficient disk space: need at least " +
               std::to_string((estimatedBytes + 100 * 1024 * 1024) / (1024 * 1024)) +
               " MB, have " + std::to_string(spaceInfo.available / (1024 * 1024)) + " MB";
    }

    // Create temp file
    auto tempPath = tempDir / ("tpe_scan_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp");
    m_file.open(tempPath, std::ios::binary | std::ios::out | std::ios::in | std::ios::trunc);
    if (!m_file.is_open()) {
        return "Failed to create temp file: " + tempPath.string();
    }

    // Flush all in-memory records to disk
    m_diskIndex.clear();
    for (size_t ci = 0; ci < m_memoryChunks.size(); ++ci) {
        ChunkIndex idx;
        idx.fileOffset = static_cast<std::streamoff>(ci * CHUNK_SIZE_BYTES);
        idx.recordCount = (ci == m_memoryChunks.size() - 1)
            ? static_cast<uint32_t>(m_totalCount % CHUNK_RECORDS)
            : CHUNK_RECORDS;
        if (idx.recordCount == 0) idx.recordCount = CHUNK_RECORDS;
        m_diskIndex.push_back(idx);

        for (uint32_t ri = 0; ri < idx.recordCount; ++ri) {
            writeRecordToDisk(m_memoryChunks[ci][ri]);
        }
    }

    // Free memory and switch backend
    m_memoryChunks.clear();
    m_memoryChunks.shrink_to_fit();
    m_diskPath = tempPath;
    m_backend = StorageBackend::OnDisk;
    m_file.flush();

    return std::nullopt;
}

// ============================================================
// writeRecordToDisk — raw binary write at current file position
// ============================================================
void ResultStorage::writeRecordToDisk(const ScanRecord& record) {
    if (!m_file.is_open()) return;
    m_file.write(reinterpret_cast<const char*>(&record), sizeof(ScanRecord));

    // If this starts a new chunk, add index entry
    if ((m_totalCount % CHUNK_RECORDS) == 0) {
        ChunkIndex idx;
        idx.fileOffset = static_cast<std::streamoff>(m_chunkCount * CHUNK_SIZE_BYTES);
        idx.recordCount = 0; // will be updated on subsequent appends or at close
        m_diskIndex.push_back(idx);
        ++m_chunkCount;
    }
}

// ============================================================
// readRecordFromDisk — raw binary read at specific file offset
// ============================================================
ScanRecord ResultStorage::readRecordFromDisk(std::streamoff offset) const {
    ScanRecord rec;
    auto& file = const_cast<std::fstream&>(m_file);
    file.seekg(offset, std::ios::beg);
    file.read(reinterpret_cast<char*>(&rec), sizeof(ScanRecord));
    return rec;
}

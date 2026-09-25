#include "ScanSession.hpp"
#include "ResultStorage.hpp"

#include <fstream>

namespace tpe {

// ============================================================
// Construction
// ============================================================
ScanSession::ScanSession(std::shared_ptr<tpe::platform::PlatformProcess> process)
    : m_process(std::move(process))
{}

// ============================================================
// Session lifecycle
// ============================================================
void ScanSession::beginScan(const ValueType& type, ScanOptions /*options*/) {
    m_state = SessionState::Scanning;
    m_round = 0;
    m_valueType = &type;
    m_storage = ResultStorage{};
    m_prevStorage.reset();
}

void ScanSession::commitFirstScan(std::vector<ScanRecord> results) {
    m_storage = ResultStorage{};
    for (const auto& record : results) {
        m_storage.append(record);
    }
    m_round = 1;
    m_condition = ScanCondition::ExactValue;
    m_state = SessionState::Ready;
}

Result<void, SessionError> ScanSession::commitNextScan(ScanCondition condition,
                                                       std::vector<ScanRecord> results) {
    if (m_state != SessionState::Ready) {
        return Result<void, SessionError>::error(SessionError{"Session not in Ready state"});
    }
    m_prevStorage = std::move(m_storage);
    m_storage = ResultStorage{};
    for (const auto& record : results) {
        m_storage.append(record);
    }
    ++m_round;
    m_condition = condition;
    return Result<void, SessionError>::success();
}

Result<void, SessionError> ScanSession::undo() {
    if (!canUndo()) {
        return Result<void, SessionError>::error(SessionError{"Nothing to undo"});
    }
    m_storage = std::move(*m_prevStorage);
    m_prevStorage.reset();
    --m_round;
    return Result<void, SessionError>::success();
}

void ScanSession::close() {
    m_state = SessionState::Idle;
    m_round = 0;
    m_storage = ResultStorage{};
    m_prevStorage.reset();
    m_valueType = nullptr;
}

// ============================================================
// Queries
// ============================================================
uint64_t ScanSession::resultCount() const {
    return m_storage.totalCount();
}

bool ScanSession::canUndo() const {
    return m_state == SessionState::Ready && m_round >= 2 && m_prevStorage.has_value();
}

bool ScanSession::isDiskBacked() const {
    return m_storage.isDiskBacked();
}

// ============================================================
// Result access
// ============================================================
std::optional<ScanRecord> ScanSession::resultAt(uint64_t index) const {
    return m_storage.readAt(index);
}

// ============================================================
// Memory operations
// ============================================================
Result<tpe::Memory, PlatformError> ScanSession::readMemory(tpe::Address addr, tpe::Size size) const {
    if (!m_process) {
        return Result<tpe::Memory, PlatformError>::error(
            PlatformError{"readMemory", 0, 0, "No process opened"});
    }
    MemoryPage page(addr, size);
    return m_process->read(page);
}

Result<void, PlatformError> ScanSession::writeMemory(tpe::Address addr, const tpe::Memory& data) const {
    if (!m_process) {
        return Result<void, PlatformError>::error(
            PlatformError{"writeMemory", 0, 0, "No process opened"});
    }

    // Attempt write — PlatformProcess::write() already validates the address
    return m_process->write(addr, data);
}

// ============================================================
// Export
// ============================================================
Result<void, SessionError> ScanSession::exportTo(const std::filesystem::path& path,
                                                 std::string_view format) const {
    const uint64_t total = m_storage.totalCount();
    if (total == 0) {
        return Result<void, SessionError>::error(SessionError{"No results to export"});
    }

    // Validate parent directory
    auto parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        return Result<void, SessionError>::error(
            SessionError{"Directory does not exist: " + parent.string()});
    }

    std::string fmt(format);
    // Default to .txt if no extension
    std::filesystem::path outPath = path;
    if (!outPath.has_extension()) {
        outPath += ".txt";
        fmt = "txt";
    }

    std::ofstream file(outPath, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        return Result<void, SessionError>::error(
            SessionError{"Failed to open file: " + outPath.string()});
    }

    if (fmt == "csv" || outPath.extension() == ".csv") {
        file << "Address,Value,Type,PageBase,PageSize\n";
        for (uint64_t i = 0; i < total; ++i) {
            auto rec = m_storage.readAt(i);
            if (!rec.has_value()) break;
            file << "0x" << std::hex << rec->address << std::dec << ",";
            if (rec->hasSnapshot()) {
                file << static_cast<int>(rec->snapshot_data[0]);
            } else {
                file << "?";
            }
            file << ",,,\n";  // Type, PageBase, PageSize left empty for now
        }
    } else {
        // TXT format
        for (uint64_t i = 0; i < total; ++i) {
            auto rec = m_storage.readAt(i);
            if (!rec.has_value()) break;
            file << "0x" << std::hex << rec->address << std::dec << ": ";
            if (rec->hasSnapshot()) {
                file << static_cast<int>(rec->snapshot_data[0]);
            } else {
                file << "?";
            }
            file << "\n";
        }
    }
    file.close();
    return Result<void, SessionError>::success();
}

} // namespace tpe

#include "ScanSession.h"
#include "ResultStorage.h"

#include <fstream>
#include <stdexcept>
#include <algorithm>

// ============================================================
// Construction
// ============================================================
ScanSession::ScanSession(std::shared_ptr<PlatformProcess> process)
    : m_process(std::move(process))
{}

// ============================================================
// Session lifecycle
// ============================================================
void ScanSession::beginScan(const ValueType& type, ScanOptions /*options*/) {
    m_state = SessionState::Scanning;
    m_round = 0;
    m_valueType = &type;
    m_results.clear();
    m_prevResults.reset();
}

void ScanSession::commitFirstScan(std::vector<ScanRecord> results) {
    m_results = std::move(results);
    m_round = 1;
    m_condition = ScanCondition::ExactValue;
    m_state = SessionState::Ready;
}

void ScanSession::commitNextScan(ScanCondition condition, std::vector<ScanRecord> results) {
    if (m_state != SessionState::Ready) {
        throw std::runtime_error("Session not in Ready state");
    }
    m_prevResults = std::move(m_results);
    m_results = std::move(results);
    ++m_round;
    m_condition = condition;
}

void ScanSession::undo() {
    if (!canUndo()) {
        throw std::runtime_error("Nothing to undo");
    }
    m_results = std::move(*m_prevResults);
    m_prevResults.reset();
    --m_round;
}

void ScanSession::close() {
    m_state = SessionState::Idle;
    m_round = 0;
    m_results.clear();
    m_prevResults.reset();
    m_valueType = nullptr;
}

// ============================================================
// Queries
// ============================================================
uint64_t ScanSession::resultCount() const {
    return static_cast<uint64_t>(m_results.size());
}

bool ScanSession::canUndo() const {
    return m_state == SessionState::Ready && m_round >= 2 && m_prevResults.has_value();
}

bool ScanSession::isDiskBacked() const {
    return false; // ResultStorage integration deferred to Phase 6
}

// ============================================================
// Result access
// ============================================================
std::optional<ScanRecord> ScanSession::resultAt(uint64_t index) const {
    if (index >= m_results.size()) return std::nullopt;
    return m_results[static_cast<size_t>(index)];
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
void ScanSession::exportTo(const std::filesystem::path& path, std::string_view format) const {
    if (m_results.empty()) {
        throw std::runtime_error("No results to export");
    }

    // Validate parent directory
    auto parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        throw std::runtime_error("Directory does not exist: " + parent.string());
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
        throw std::runtime_error("Failed to open file: " + outPath.string());
    }

    if (fmt == "csv" || outPath.extension() == ".csv") {
        file << "Address,Value,Type,PageBase,PageSize\n";
        for (const auto& rec : m_results) {
            file << "0x" << std::hex << rec.address << std::dec << ",";
            if (rec.hasSnapshot()) {
                file << static_cast<int>(rec.snapshot_data[0]);
            } else {
                file << "?";
            }
            file << ",,,\n";  // Type, PageBase, PageSize left empty for now
        }
    } else {
        // TXT format
        for (const auto& rec : m_results) {
            file << "0x" << std::hex << rec.address << std::dec << ": ";
            if (rec.hasSnapshot()) {
                file << static_cast<int>(rec.snapshot_data[0]);
            } else {
                file << "?";
            }
            file << "\n";
        }
    }
    file.close();
}

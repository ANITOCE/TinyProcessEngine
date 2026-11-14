#include "ReplExecutor.hpp"

#include "CliParser.hpp"
#include "Platform.hpp"
#include "ProcessEngine.hpp"
#include "ReplState.hpp"
#include "ValueFormatter.hpp"

#include <algorithm>
#include <cassert>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace tpe::cli {
namespace {

constexpr const char* kNewScanUsage = "Usage: new-scan [--equal] [<value-type>] <value>";
constexpr const char* kNextScanUsage =
    "Usage: next-scan [--equal] [<value-type>] <value> | next-scan "
    "[--greater|--less|--changed|--unchanged] [<value-type>]";
constexpr const char* kListUsage = "Usage: list [<page>] | list --all";
constexpr const char* kWriteUsage = "Usage: write <address> <new-value>";
constexpr const char* kNoResultsHint = "No scan results available. Run 'new-scan' first.";
constexpr const char* kTargetUnavailable = "The target process is no longer available.";

std::string invalidValueMessage(const CliValueType& vt, const std::string& reason,
                                const std::string& usage)
{
    return "Invalid value for " + std::string(vt.shortName) + ": " + reason + "\n" + usage;
}

void appendResult(ReplExecutionResult& result, std::string text, bool error)
{
    if (!result.output.empty() && result.output.back() != '\n') {
        result.output.push_back('\n');
    }
    result.output += std::move(text);
    result.error = result.error || error;
}

void executeNewScan(const ReplCommand& command, ReplState& state, ProcessEngine& engine,
                    ReplExecutionResult& result)
{
    if (!engine.targetAlive()) {
        appendResult(result, kTargetUnavailable, true);
        return;
    }
    const CliValueType& vt = command.valueType != nullptr ? *command.valueType : *state.valueType;

    if (vt.kind == CliValueKind::String && command.scanType != ReplScanType::Equal) {
        appendResult(result,
                     "Scan type --" + std::string(scanTypeName(command.scanType)) +
                         " does not support --string.\n" + kNewScanUsage,
                     true);
        return;
    }

    if (command.scanType == ReplScanType::Unknown) {
        const uint64_t total = engine.searchUnknown(*vt.type);
        if (!engine.targetAlive()) {
            appendResult(result, kTargetUnavailable, true);
            return;
        }
        state.onValuelessScan(ReplScanType::Unknown, vt, total);
        return;
    }

    if (command.scanType == ReplScanType::Greater || command.scanType == ReplScanType::Less) {
        std::string reason;
        const std::optional<tpe::Memory> target = vt.type->parse(command.value, reason);
        if (!target.has_value()) {
            appendResult(result, invalidValueMessage(vt, reason, kNewScanUsage), true);
            return;
        }

        const std::optional<ScanCondition> condition =
            toFirstScanCondition(command.scanType);
        if (!condition.has_value()) {
            return;
        }

        const uint64_t total = engine.searchComparison(*vt.type, *condition, *target);
        if (!engine.targetAlive()) {
            appendResult(result, kTargetUnavailable, true);
            return;
        }
        state.onValueScan(command.scanType, vt, command.value, total);
        return;
    }

    std::string reason;
    const std::optional<tpe::Memory> pattern = vt.type->parse(command.value, reason);
    if (!pattern.has_value()) {
        appendResult(result, invalidValueMessage(vt, reason, kNewScanUsage), true);
        return;
    }

    const uint64_t total = engine.searchMemory(*vt.type, *pattern);
    if (!engine.targetAlive()) {
        appendResult(result, kTargetUnavailable, true);
        return;
    }
    state.onValueScan(ReplScanType::Equal, vt, command.value, total);
}

void executeNextScan(const ReplCommand& command, ReplState& state, ProcessEngine& engine,
                     ReplExecutionResult& result)
{
    if (!engine.targetAlive()) {
        appendResult(result, kTargetUnavailable, true);
        return;
    }
    if (command.valueType != nullptr &&
        command.valueType->shortName != state.valueType->shortName) {
        appendResult(result,
                     "next-scan cannot change the value type (current: " +
                         std::string(state.valueType->shortName) + ", requested: " +
                         std::string(command.valueType->shortName) + ").\n" + kNextScanUsage,
                     true);
        return;
    }

    if (state.valueType->kind == CliValueKind::String &&
        command.scanType != ReplScanType::Equal) {
        appendResult(result,
                     "Scan type --" + std::string(scanTypeName(command.scanType)) +
                         " does not support --string.\n" + kNextScanUsage,
                     true);
        return;
    }

    ScanSession* session = engine.session();
    if (session == nullptr || session->state() != SessionState::Ready) {
        appendResult(result, std::string(kNoResultsHint) + "\n" + kNextScanUsage, true);
        return;
    }

    const CliValueType& vt = command.valueType != nullptr ? *command.valueType : *state.valueType;

    std::optional<tpe::Memory> newValue;
    if (command.hasValue) {
        std::string reason;
        newValue = vt.type->parse(command.value, reason);
        if (!newValue.has_value()) {
            appendResult(result, invalidValueMessage(vt, reason, kNextScanUsage), true);
            return;
        }
    }

    const std::optional<ScanCondition> condition = toScanCondition(command.scanType);
    if (!condition.has_value()) {
        return;
    }

    const uint64_t total = engine.nextScan(*condition, *vt.type, newValue);
    if (!engine.targetAlive()) {
        appendResult(result, kTargetUnavailable, true);
        return;
    }
    if (command.scanType == ReplScanType::Equal) {
        state.onValueScan(command.scanType, vt, command.value, total);
    } else {
        state.onValuelessScan(command.scanType, vt, total);
    }
}

void executeList(const ReplCommand& command, ReplState& state, ProcessEngine& engine,
                 ReplExecutionResult& result)
{
    const ScanSession* session = engine.session();
    if (session == nullptr || session->state() != SessionState::Ready) {
        appendResult(result, std::string(kNoResultsHint) + "\n" + kListUsage, true);
        return;
    }
    if (!engine.targetAlive()) {
        appendResult(result, kTargetUnavailable, true);
        return;
    }

    const uint64_t total = session->resultCount();
    if (total == 0) {
        appendResult(result, "No matches to display.", false);
        return;
    }

    const auto readLiveBytes = [&](const ScanRecord& record) -> std::optional<tpe::Memory> {
        if (record.snapshot_size == 0) {
            return std::nullopt;
        }
        const tpe::Size width = state.valueType->kind == CliValueKind::String
                                    ? static_cast<tpe::Size>(record.snapshot_size)
                                    : state.valueType->type->byteWidth();
        const Result<tpe::Memory, PlatformError> bytes = session->readMemory(record.address, width);
        if (!bytes.has_value() || bytes.value().size() < width) {
            return std::nullopt;
        }
        return bytes.value();
    };

    std::ostringstream out;
    const auto appendRow = [&](uint64_t index) {
        const std::optional<ScanRecord> record = session->resultAt(index);
        if (record.has_value()) {
            out << formatListEntry(record->address, readLiveBytes(*record), *state.valueType)
                << "\n";
        }
    };

    if (command.listAll) {
        out << formatMatchesTotal(total) << "\n";
        const uint64_t shown = (std::min)(total, kListDisplayCap);
        for (uint64_t index = 0; index < shown; ++index) {
            appendRow(index);
        }
        if (total > shown) {
            out << formatTruncationNotice(total - shown) << "\n";
        }
        appendResult(result, out.str(), false);
        return;
    }

    if (!state.isPageInRange(command.page)) {
        appendResult(result,
                     "Page " + std::to_string(command.page) +
                         " is out of range (total pages: " +
                         std::to_string(state.pageCount()) + ").\n" + kListUsage,
                     true);
        return;
    }

    const uint64_t first = static_cast<uint64_t>(command.page - 1) * kReplPageSize;
    const uint64_t last = (std::min)(first + kReplPageSize, total);
    for (uint64_t index = first; index < last; ++index) {
        appendRow(index);
    }
    appendResult(result, out.str(), false);
}

void executeUndo(ReplState& state, ProcessEngine& engine, ReplExecutionResult& result)
{
    ScanSession* session = engine.session();
    if (session == nullptr || !session->canUndo()) {
        appendResult(result, "Nothing to undo (no previous scan round).\nUsage: undo", true);
        return;
    }
    const Result<void, SessionError> undone = session->undo();
    assert(undone.has_value());
    (void)undone;
    state.onUndo(session->resultCount());
}

void executeWrite(const ReplCommand& command, ReplState& state, ProcessEngine& engine,
                  ReplExecutionResult& result)
{
    ScanSession* session = engine.session();
    if (session == nullptr) {
        appendResult(result, std::string(kNoResultsHint) + "\n" + kWriteUsage, true);
        return;
    }

    const CliValueType& vt = *state.valueType;

    std::string reason;
    const std::optional<std::string> text = extractWriteValueText(command, vt, reason);
    if (!text.has_value()) {
        appendResult(result, reason + "\n" + kWriteUsage, true);
        return;
    }

    const std::optional<tpe::Memory> data = vt.type->parse(*text, reason);
    if (!data.has_value()) {
        appendResult(result, invalidValueMessage(vt, reason, kWriteUsage), true);
        return;
    }

    const Result<void, PlatformError> written = session->writeMemory(command.address, *data);
    if (!written.has_value()) {
        if (!engine.targetAlive()) {
            appendResult(result, kTargetUnavailable, true);
            return;
        }
        appendResult(result,
                     "Failed to write memory at " + formatAddress(command.address) + ": " +
                         written.error().message,
                     true);
        return;
    }
    appendResult(result,
                 "Wrote " + *text + " to " + formatAddress(command.address) + ".",
                 false);
}

} // namespace

ReplExecutor::ReplExecutor(tpe::ProcessEngine& engine)
    : m_engine(engine)
{
}

ReplExecutionResult ReplExecutor::execute(const ReplCommand& command, ReplState& state)
{
    ReplExecutionResult result;
    switch (command.kind) {
    case ReplCommandKind::NewScan:
        executeNewScan(command, state, m_engine, result);
        break;
    case ReplCommandKind::NextScan:
        executeNextScan(command, state, m_engine, result);
        break;
    case ReplCommandKind::List:
        executeList(command, state, m_engine, result);
        break;
    case ReplCommandKind::Write:
        executeWrite(command, state, m_engine, result);
        break;
    case ReplCommandKind::Undo:
        executeUndo(state, m_engine, result);
        break;
    case ReplCommandKind::Help:
    case ReplCommandKind::Exit:
    case ReplCommandKind::Empty:
    case ReplCommandKind::Unknown:
        break;
    }
    return result;
}

} // namespace tpe::cli

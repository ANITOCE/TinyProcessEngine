#include "startup_cli.hpp"

#include "CliParser.hpp"
#include "Platform.hpp"
#include "ProcessEngine.hpp"
#include "ReplState.hpp"
#include "ValueFormatter.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace tpe::app {

namespace {

/// 提示符展示用进程名:优先取进程列表中的真实名称。
/// (平台层 open() 以占位名构造进程对象,故不以它作为唯一来源。)
std::string promptProcessName(ProcessEngine& engine, tpe::platform::Pid_t pid,
                              const std::shared_ptr<tpe::platform::PlatformProcess>& process)
{
    const std::optional<std::string> listed = engine.searchProcess(pid);
    if (listed.has_value() && !listed->empty()) {
        return *listed;
    }
    return process ? process->getProcessName() : std::string();
}

/// 用法/失败提示(契约:含 Usage、不执行、不改变会话状态、不退出 REPL)。
void printMessage(const std::string& message)
{
    std::cout << message << std::endl;
}

std::string invalidValueMessage(const tpe::cli::CliValueType& vt, const std::string& reason,
                                const std::string& usage)
{
    return "Invalid value for " + std::string(vt.shortName) + ": " + reason + "\n" + usage;
}

constexpr const char* kNewScanUsage = "Usage: new-scan [--equal] [<value-type>] <value>";
constexpr const char* kNextScanUsage =
    "Usage: next-scan [--equal] [<value-type>] <value> | next-scan "
    "[--greater|--less|--changed|--unchanged] [<value-type>]";
constexpr const char* kListUsage = "Usage: list [<page>] | list --all";
constexpr const char* kWriteUsage = "Usage: write <address> <new-value>";
constexpr const char* kNoResultsHint = "No scan results available. Run 'new-scan' first.";

/// new-scan(契约 C-R1;US1 C-D1;US2 C-D2/C-D3;FR-012/FR-027 C-D5):默认 equal + 当前类型;
/// 成功不打印输出,仅更新提示符状态。三种首扫形态:
/// - Unknown:无值首扫(FR-001–004),不进行值解析;
/// - Greater/Less:值必传(解析层已保证);值解析口径与 equal 一致,失败 → 既有非法值提示;
/// - Equal:既有等值首扫。
/// --string 仅等值扫描:非等值形态与 string 组合(命令旗标或会话继承)→ 用法错误。
void executeNewScan(const tpe::cli::ReplCommand& command, tpe::cli::ReplState& state,
                    ProcessEngine& engine)
{
    const tpe::cli::CliValueType& vt =
        command.valueType != nullptr ? *command.valueType : *state.valueType;

    // FR-012/FR-027(C-D5,契约变更):string 仅等值扫描——--unknown/--greater/--less
    // 与 --string 组合(显式旗标或当前会话类型)一律用法错误显式拒绝
    // (含 Usage、不执行、不改变会话状态;不得以空结果替代拒绝)。
    // 先于值解析:组合非法性与值是否可解析无关,避免后者掩盖前者。
    if (vt.kind == tpe::cli::CliValueKind::String &&
        command.scanType != tpe::cli::ReplScanType::Equal) {
        printMessage("Scan type --" + std::string(tpe::cli::scanTypeName(command.scanType)) +
                     " does not support --string.\n" + kNewScanUsage);
        return; // 不执行、不改变会话状态
    }

    if (command.scanType == tpe::cli::ReplScanType::Unknown) {
        // C-D1/FR-004:成功后 scan-type=unknown、值段隐藏(lastValue 清空);
        // 带值已由解析层按用法错误拒绝(严格策略保持)。
        const uint64_t total = engine.searchUnknown(*vt.type);
        state.onValuelessScan(tpe::cli::ReplScanType::Unknown, vt, total);
        return;
    }

    if (command.scanType == tpe::cli::ReplScanType::Greater ||
        command.scanType == tpe::cli::ReplScanType::Less) {
        // C-D2/C-D3/FR-007–011:首轮大小比较(严格 GT/LT);
        // 值解析失败 → 既有非法值提示(不执行、不改变会话状态);
        // 成功后提示符显示本次比较值(§11.3 带值扫描)。
        std::string reason;
        const std::optional<tpe::Memory> target = vt.type->parse(command.value, reason);
        if (!target.has_value()) {
            printMessage(invalidValueMessage(vt, reason, kNewScanUsage));
            return; // 不执行、不改变会话状态
        }

        const std::optional<ScanCondition> condition =
            tpe::cli::toFirstScanCondition(command.scanType);
        if (!condition.has_value()) {
            return; // Greater/Less 映射恒有值;防御(不可达)
        }

        const uint64_t total = engine.searchComparison(*vt.type, *condition, *target);
        state.onValueScan(command.scanType, vt, command.value, total);
        return;
    }

    // Equal(默认):既有等值首扫
    std::string reason;
    const std::optional<tpe::Memory> pattern = vt.type->parse(command.value, reason);
    if (!pattern.has_value()) {
        printMessage(invalidValueMessage(vt, reason, kNewScanUsage));
        return; // 不执行、不改变会话状态
    }

    const uint64_t total = engine.searchMemory(*vt.type, *pattern);
    state.onValueScan(tpe::cli::ReplScanType::Equal, vt, command.value, total);
}

/// next-scan(契约 C-R2;FR-026/FR-027 C-D4/C-D5):条件 → ScanCondition;无结果时明确提示、
/// 不执行、不改变状态。类型一致性:会话类型仅由 new-scan 设定——同类旗标无操作、异类拒绝;
/// string 会话仅等值扫描。
void executeNextScan(const tpe::cli::ReplCommand& command, tpe::cli::ReplState& state,
                     ProcessEngine& engine)
{
    // FR-026(C-D4,契约变更):数值类型一致性——会话类型仅由 new-scan 设定;
    // 异类类型旗标 → 用法错误(含 Usage、不执行、不改变会话状态)。
    // 先于会话/结果检查,保证用法错误输出确定(不受当前结果集状态影响)。
    if (command.valueType != nullptr &&
        command.valueType->shortName != state.valueType->shortName) {
        printMessage("next-scan cannot change the value type (current: " +
                     std::string(state.valueType->shortName) + ", requested: " +
                     std::string(command.valueType->shortName) + ").\n" + kNextScanUsage);
        return; // 不执行、不改变会话状态
    }

    // FR-027(C-D5,契约变更):string 会话仅支持等值扫描——--changed/--unchanged/
    // --greater/--less 在 --string 会话中 → 用法错误(含 Usage、不执行、不改变会话状态;
    // 不得以空结果(0 条)替代拒绝)。
    if (state.valueType->kind == tpe::cli::CliValueKind::String &&
        command.scanType != tpe::cli::ReplScanType::Equal) {
        printMessage("Scan type --" + std::string(tpe::cli::scanTypeName(command.scanType)) +
                     " does not support --string.\n" + kNextScanUsage);
        return; // 不执行、不改变会话状态
    }

    ScanSession* session = engine.session();
    if (session == nullptr || session->state() != SessionState::Ready) {
        printMessage(std::string(kNoResultsHint) + "\n" + kNextScanUsage);
        return;
    }

    const tpe::cli::CliValueType& vt =
        command.valueType != nullptr ? *command.valueType : *state.valueType;

    std::optional<tpe::Memory> newValue;
    if (command.hasValue) {
        std::string reason;
        newValue = vt.type->parse(command.value, reason);
        if (!newValue.has_value()) {
            printMessage(invalidValueMessage(vt, reason, kNextScanUsage));
            return;
        }
    }

    const std::optional<ScanCondition> condition = tpe::cli::toScanCondition(command.scanType);
    if (!condition.has_value()) {
        return; // `--unknown` 已由解析层拒绝,此分支不可达(防御)
    }

    const uint64_t total = engine.nextScan(*condition, *vt.type, newValue);
    if (command.scanType == tpe::cli::ReplScanType::Equal) {
        state.onValueScan(command.scanType, vt, command.value, total);
    } else {
        state.onValuelessScan(command.scanType, vt, total);
    }
}

/// list(契约 C-R3;FR-013–018/C-D6):值列 = 展示时从目标进程实时重读的当前值——
/// 宽度:数值 = 类型宽度(`byteWidth()`),string = 记录快照宽度(≤8);
/// 零宽(snapshot_size == 0)/ 读取失败 / 短读 → 值列逐字 `??`(不回退快照)。
/// `--all` 上限 10000 与截断提示、分页 20/页、越界/空结果提示不变;
/// 实时读取为纯展示,不影响会话状态(Total / 分页 / undo 栈不变)。
void executeList(const tpe::cli::ReplCommand& command, tpe::cli::ReplState& state,
                 ProcessEngine& engine)
{
    const ScanSession* session = engine.session();
    if (session == nullptr || session->state() != SessionState::Ready) {
        printMessage(std::string(kNoResultsHint) + "\n" + kListUsage);
        return;
    }

    const uint64_t total = session->resultCount();
    if (total == 0) {
        printMessage("No matches to display."); // 空结果明确提示;不改变会话状态
        return;
    }

    // FR-013–016/C-D6(E4/INV-L):逐条读取当前内存值;失败 / 短读 / 零宽 → nullopt。
    const auto readLiveBytes = [&](const ScanRecord& record) -> std::optional<tpe::Memory> {
        if (record.snapshot_size == 0) {
            return std::nullopt; // 零宽:视为不可读(E4:`??`)
        }
        const tpe::Size width = state.valueType->kind == tpe::cli::CliValueKind::String
                                    ? static_cast<tpe::Size>(record.snapshot_size)
                                    : state.valueType->type->byteWidth();
        const Result<tpe::Memory, PlatformError> bytes =
            session->readMemory(record.address, width);
        if (!bytes.has_value() || bytes.value().size() < width) {
            return std::nullopt; // 读取失败 / 短读(FR-015)
        }
        return bytes.value();
    };

    std::ostringstream out;
    const auto appendRow = [&](uint64_t index) {
        const std::optional<ScanRecord> record = session->resultAt(index);
        if (record.has_value()) {
            out << tpe::cli::formatListEntry(record->address, readLiveBytes(*record),
                                             *state.valueType) << "\n";
        }
    };

    if (command.listAll) {
        out << tpe::cli::formatMatchesTotal(total) << "\n";
        const uint64_t shown = (std::min)(total, tpe::cli::kListDisplayCap);
        for (uint64_t index = 0; index < shown; ++index) {
            appendRow(index);
        }
        if (total > shown) {
            out << tpe::cli::formatTruncationNotice(total - shown) << "\n";
        }
        std::cout << out.str() << std::flush;
        return;
    }

    if (!state.isPageInRange(command.page)) {
        printMessage("Page " + std::to_string(command.page) +
                     " is out of range (total pages: " + std::to_string(state.pageCount()) +
                     ").\n" + kListUsage);
        return; // 用法提示;不改变会话状态
    }

    const uint64_t first = static_cast<uint64_t>(command.page - 1) * tpe::cli::kReplPageSize;
    const uint64_t last = (std::min)(first + tpe::cli::kReplPageSize, total);
    for (uint64_t index = first; index < last; ++index) {
        appendRow(index);
    }
    std::cout << out.str() << std::flush;
}

/// undo(契约 C-R5):无轮次 → 明确提示、状态不变。
void executeUndo(tpe::cli::ReplState& state, ProcessEngine& engine)
{
    ScanSession* session = engine.session();
    if (session == nullptr || !session->canUndo()) {
        printMessage(std::string("Nothing to undo (no previous scan round).") + "\n" +
                     "Usage: undo");
        return;
    }
    // 前置 canUndo() 已保证成功;失败为不可达路径(C-E3)
    const Result<void, SessionError> undone = session->undo();
    assert(undone.has_value());
    (void)undone;
    state.onUndo(session->resultCount());
}

/// write(契约 C-R4;FR-019 / P7 裁决):解析失败或运行期失败(地址越界 / 目标页不可写)
/// → 打印明确错误、不执行写入、不改变会话状态、不退出 REPL;
/// 成功 → 一行提示;不更新 `list` 快照值(实时重读属开发指南 Phase 07)。
void executeWrite(const tpe::cli::ReplCommand& command, tpe::cli::ReplState& state,
                  ProcessEngine& engine)
{
    ScanSession* session = engine.session();
    if (session == nullptr) {
        printMessage(std::string(kNoResultsHint) + "\n" + kWriteUsage);
        return;
    }

    const tpe::cli::CliValueType& vt = *state.valueType;

    std::string reason;
    const std::optional<std::string> text =
        tpe::cli::extractWriteValueText(command, vt, reason);
    if (!text.has_value()) {
        printMessage(reason + "\n" + kWriteUsage);
        return; // 不执行、不改变会话状态
    }

    const std::optional<tpe::Memory> data = vt.type->parse(*text, reason);
    if (!data.has_value()) {
        printMessage(invalidValueMessage(vt, reason, kWriteUsage));
        return; // 不执行、不改变会话状态
    }

    const Result<void, PlatformError> written = session->writeMemory(command.address, *data);
    if (!written.has_value()) {
        // P7 裁决:运行期失败 → 明确错误、不退出、状态保持在该命令之前
        printMessage("Failed to write memory at " + tpe::cli::formatAddress(command.address) +
                     ": " + written.error().message);
        return;
    }
    printMessage("Wrote " + *text + " to " + tpe::cli::formatAddress(command.address) + ".");
}

/// 执行已识别命令(契约 C-R1–C-R5)。
void executeReplCommand(const tpe::cli::ReplCommand& command, tpe::cli::ReplState& state,
                        ProcessEngine& engine)
{
    switch (command.kind) {
    case tpe::cli::ReplCommandKind::NewScan:
        executeNewScan(command, state, engine);
        break;
    case tpe::cli::ReplCommandKind::NextScan:
        executeNextScan(command, state, engine);
        break;
    case tpe::cli::ReplCommandKind::List:
        executeList(command, state, engine);
        break;
    case tpe::cli::ReplCommandKind::Write:
        executeWrite(command, state, engine);
        break;
    case tpe::cli::ReplCommandKind::Undo:
        executeUndo(state, engine);
        break;
    default:
        break; // planReplOutcome 不会把其它 kind 归为 Execute
    }
}

/// REPL 主循环(US2;契约 C-R1–C-R6):提示符 → 读行 → 处置。
/// 空输入无输出;未知命令显示帮助;错误不改变状态、不退出(FR-021);`exit`/EOF 退出码 0。
int runRepl(ProcessEngine& engine, const std::string& processName)
{
    tpe::cli::ReplState state(processName);

    for (;;) {
        std::cout << state.prompt() << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) {
            return tpe::cli::kExitOk; // stdin EOF:按正常退出处理(实现默认)
        }

        const tpe::cli::ReplCommand command = tpe::cli::parseReplCommand(line);
        switch (tpe::cli::planReplOutcome(command)) {
        case tpe::cli::ReplOutcome::Noop:
            break; // 空输入:不打印任何内容,仅重新显示提示符
        case tpe::cli::ReplOutcome::ShowHelp:
            std::cout << tpe::cli::replHelpText() << std::flush;
            break;
        case tpe::cli::ReplOutcome::UsageError:
            printMessage(command.error); // 含 Usage 提示;不执行、不改变状态
            break;
        case tpe::cli::ReplOutcome::Exit:
            return tpe::cli::kExitOk;
        case tpe::cli::ReplOutcome::Execute:
            executeReplCommand(command, state, engine);
            break;
        }
    }
}

} // namespace

int runCliWithEngine(const std::vector<std::string>& args, ProcessEngine& engine)
{
    const tpe::cli::TerminalCommand command = tpe::cli::parseTerminalCommand(args);

    switch (command.kind) {
    case tpe::cli::TerminalCommandKind::AllProcesses: {
        // 契约 C-T1/C-P5:成功 → 沿用 “PID: N ProcessName: X” 输出 + 退出码 0;
        // 枚举失败 → stderr 一行 + 运行期失败退出码。
        const Result<void, PlatformError> listed = engine.getProcessList();
        if (!listed.has_value()) {
            std::cerr << "Failed to enumerate processes: " << listed.error().message << std::endl;
            return tpe::cli::kExitRuntimeError;
        }
        return tpe::cli::kExitOk;
    }

    case tpe::cli::TerminalCommandKind::SearchProcess: {
        const std::optional<std::string> name = engine.searchProcess(command.pid);
        if (!name.has_value()) {
            std::cerr << "Process with PID " << command.pid << " not found." << std::endl;
            return tpe::cli::kExitRuntimeError;
        }
        if (name->empty()) {
            // 不得以空行充当成功(契约 C-T2)
            std::cerr << "Failed to read the process name for PID " << command.pid << "."
                      << std::endl;
            return tpe::cli::kExitRuntimeError;
        }
        std::cout << *name << std::endl; // 契约 C-T2:单行进程名
        return tpe::cli::kExitOk;
    }

    case tpe::cli::TerminalCommandKind::OpenProcess: {
        const std::shared_ptr<tpe::platform::PlatformProcess> process = engine.openProcess(command.pid);
        if (!process) {
            // ProcessEngine::openProcess 已向 stderr 输出失败原因(契约 C-T3);不进入 REPL
            return tpe::cli::kExitRuntimeError;
        }
        return runRepl(engine, promptProcessName(engine, command.pid, process));
    }

    default:
        // run() 仅转发三类执行命令到此处;其余分类(含 UsageError)在此不可达。
        return tpe::cli::kExitUsageError;
    }
}

int runCli(const std::vector<std::string>& args)
{
    ProcessEngine engine;
    return runCliWithEngine(args, engine);
}

} // namespace tpe::app

#include "RemoteCommandExecutor.hpp"

#include "CliParser.hpp"
#include "ProcessEngine.hpp"
#include "ReplExecutor.hpp"
#include "ReplState.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace tpe::remote {
namespace {

// ============================================================
// 固定文本
// ============================================================

/// 帮助文本：5 行，逐字符与契约一致；不含 `TPE> help` 回显行，不含尾随空行。
constexpr std::string_view kHelpText =
    "--all-processes        List processes on the server.\n"
    "--search-process <PID> Show the name of a process on the server.\n"
    "--open-process <PID>   Open a process on the server and enter its scan prompt.\n"
    "help                   Show this help.\n"
    "exit                   End this client session.";

/// 未知命令文本：未交付的命令（如 `--open-process`）与空输入共用。
constexpr std::string_view kUnknownCommandText =
    "Unknown command. Type 'help' for available commands.";

/// `--search-process` 的完整命令名，以及带参数时必须以空格分隔的前缀。
constexpr std::string_view kSearchProcessCommand = "--search-process";
constexpr std::string_view kSearchProcessPrefix = "--search-process ";

/// `--open-process` 的命令名与前缀（Phase 02）。
constexpr std::string_view kOpenProcessCommand = "--open-process";
constexpr std::string_view kOpenProcessPrefix = "--open-process ";

/// 参数错误以用法开头（“用法与原因”），保证输出非空且点明命令名。
constexpr std::string_view kSearchProcessUsage = "Usage: --search-process <PID>.";
constexpr std::string_view kOpenProcessUsage = "Usage: --open-process <PID>.";

/// 结果文本不可编码为 UTF-8 的固定失败文本。
constexpr std::string_view kUnencodableNameText =
    "Process name cannot be encoded as UTF-8.";
constexpr std::string_view kUnencodableListText =
    "Process list contains a name that cannot be encoded as UTF-8.";
constexpr std::string_view kPromptOversizeText =
    "Prompt exceeds the maximum message size.";
constexpr std::size_t kMaxRemotePromptBytes = 65535;

// ============================================================
// 结果构造
// ============================================================

RemoteCommandResult makeResult(ResponseStatus status, std::string text)
{
    RemoteCommandResult result;
    result.status = status;
    result.output = std::move(text);
    return result;
}

RemoteCommandResult makeCommandError(std::string text)
{
    return makeResult(ResponseStatus::CommandError, std::move(text));
}

RemoteCommandResult makeContinue(std::string text)
{
    return makeResult(ResponseStatus::Continue, std::move(text));
}

// ============================================================
// 结果文本的严格 UTF-8 校验
// ============================================================

bool isContinuationByte(std::uint8_t byte)
{
    return (byte & 0xC0u) == 0x80u;
}

/// 严格 UTF-8 校验（不依赖 locale）：拒绝过长编码、UTF-16 代理区码点与超出
/// U+10FFFF 的序列。语义与 `RemoteProtocol.cpp` 的载荷校验一致。
/// 本文件内私有副本：T045 收敛为共享工具时替换这里与协议侧的两处实现。
bool isValidUtf8(std::string_view text)
{
    const std::size_t size = text.size();
    std::size_t index = 0;
    while (index < size) {
        const std::uint8_t lead = static_cast<std::uint8_t>(text[index]);
        if (lead <= 0x7Fu) { // ASCII
            ++index;
            continue;
        }

        std::size_t trailing = 0;      // 后续字节数
        std::uint8_t secondMin = 0x80; // 第 2 字节允许范围
        std::uint8_t secondMax = 0xBF;
        if (lead >= 0xC2u && lead <= 0xDFu) {
            trailing = 1;
        } else if (lead == 0xE0u) {
            trailing = 2;
            secondMin = 0xA0; // 拒绝过长编码
        } else if (lead >= 0xE1u && lead <= 0xECu) {
            trailing = 2;
        } else if (lead == 0xEDu) {
            trailing = 2;
            secondMax = 0x9F; // 拒绝 UTF-16 代理区 U+D800..U+DFFF
        } else if (lead >= 0xEEu && lead <= 0xEFu) {
            trailing = 2;
        } else if (lead == 0xF0u) {
            trailing = 3;
            secondMin = 0x90; // 拒绝过长编码
        } else if (lead >= 0xF1u && lead <= 0xF3u) {
            trailing = 3;
        } else if (lead == 0xF4u) {
            trailing = 3;
            secondMax = 0x8F; // 拒绝超过 U+10FFFF
        } else {
            return false; // 非法首字节（含 0x80..0xC1、0xF5..0xFF）
        }

        if (trailing > size - index - 1) {
            return false; // 序列被截断
        }
        const std::uint8_t second = static_cast<std::uint8_t>(text[index + 1]);
        if (second < secondMin || second > secondMax) {
            return false;
        }
        for (std::size_t offset = 2; offset <= trailing; ++offset) {
            if (!isContinuationByte(static_cast<std::uint8_t>(text[index + offset]))) {
                return false;
            }
        }
        index += trailing + 1;
    }
    return true;
}

// ============================================================
// --search-process 的 PID 解析（无异常路径）
// ============================================================

/// 参数解析状态；除 Ok 外都会转成带用法与原因的 CommandError。
enum class PidParseStatus {
    Ok,
    Missing,
    NotDecimal,
    OutOfRange,
};

struct PidParseResult {
    PidParseStatus status = PidParseStatus::Missing;
    tpe::platform::Pid_t value = 0;
};

bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// 十进制、无符号、可完整落入平台 PID 宽度；`std::from_chars` 不抛异常。
PidParseResult parsePidArgument(std::string_view argument)
{
    if (argument.empty()) {
        return {PidParseStatus::Missing, 0};
    }

    unsigned long long raw = 0;
    const char* const first = argument.data();
    const char* const last = first + argument.size();
    const std::from_chars_result converted = std::from_chars(first, last, raw, 10);

    if (converted.ec == std::errc::result_out_of_range) {
        return {PidParseStatus::OutOfRange, 0};
    }
    if (converted.ec != std::errc{} || converted.ptr != last) {
        // 前导 '+'/'-'、空白、十六进制或尾随多余字符一律视为非十进制。
        return {PidParseStatus::NotDecimal, 0};
    }

    // Windows Pid_t = DWORD、Linux Pid_t = int，上限不同；
    // 先以 unsigned long long 比较，再收窄，避免实现定义的有符号回绕。
    // 括号包裹 `max`：Windows.h 的 max 宏会截获不带括号的调用。
    constexpr unsigned long long kMaxPid =
        static_cast<unsigned long long>((std::numeric_limits<tpe::platform::Pid_t>::max)());
    if (raw > kMaxPid) {
        return {PidParseStatus::OutOfRange, 0};
    }

    return {PidParseStatus::Ok, static_cast<tpe::platform::Pid_t>(raw)};
}

/// 参数错误的原因文本，紧随用法之后。
std::string_view pidParseReason(PidParseStatus status)
{
    switch (status) {
    case PidParseStatus::Missing:
        return "Missing PID.";
    case PidParseStatus::NotDecimal:
        return "PID must be a decimal integer.";
    case PidParseStatus::OutOfRange:
        return "PID is out of range.";
    case PidParseStatus::Ok:
        break;
    }
    return "PID is invalid.";
}

std::string searchProcessUsageError(PidParseStatus status)
{
    std::string text(kSearchProcessUsage);
    text += ' ';
    text += pidParseReason(status);
    return text;
}

std::string openProcessUsageError(PidParseStatus status)
{
    std::string text(kOpenProcessUsage);
    text += ' ';
    text += pidParseReason(status);
    return text;
}

// ============================================================
// 单条命令的执行
// ============================================================

RemoteCommandResult executeAllProcesses(tpe::ProcessEngine& engine)
{
    // 文本与本地 `getProcessList()` 同源（T016）；枚举失败时附带平台原因。
    const tpe::Result<std::string, tpe::PlatformError> text = engine.processListText();
    if (!text.has_value()) {
        return makeCommandError("Failed to enumerate processes: " + text.error().message);
    }
    // 先编码、后长度（末段）：这里只做编码校验，长度由 Server 编码响应时判定，
    // 因而同时不可编码且超限的结果必定先报告不可编码（T030）。
    if (!isValidUtf8(text.value())) {
        return makeCommandError(std::string(kUnencodableListText));
    }
    return makeContinue(text.value());
}

RemoteCommandResult executeSearchProcess(std::string_view line, tpe::ProcessEngine& engine)
{
    // 分类阶段已保证该行是命令名本身或“命令名 + 空格 + 参数”。
    const std::string_view argument = line.size() > kSearchProcessPrefix.size()
                                          ? line.substr(kSearchProcessPrefix.size())
                                          : std::string_view{};

    // 参数校验必须先于任何引擎/平台调用（参数错误不得触碰进程）。
    const PidParseResult parsed = parsePidArgument(argument);
    if (parsed.status != PidParseStatus::Ok) {
        return makeCommandError(searchProcessUsageError(parsed.status));
    }

    const std::optional<std::string> name = engine.searchProcess(parsed.value);
    const std::string pidText = std::to_string(parsed.value);

    if (!name.has_value()) {
        return makeCommandError("Process with PID " + pidText + " not found.");
    }
    if (name->empty()) {
        return makeCommandError("Failed to read the process name for PID " + pidText + ".");
    }
    // 名称原样保留、不替换不丢弃；无法编码时按报告固定文本（先编码后长度）。
    if (!isValidUtf8(*name)) {
        return makeCommandError(std::string(kUnencodableNameText));
    }
    return makeContinue(*name);
}

RemoteCommandResult executeHelp()
{
    return makeContinue(std::string(kHelpText));
}

} // namespace

RemoteCommandKind classifyRemoteCommand(std::string_view line)
{
    // 整行精确匹配：前后带空格的已知命令不属于已交付命令。
    if (line == "--all-processes") {
        return RemoteCommandKind::AllProcesses;
    }
    if (line == kSearchProcessCommand || startsWith(line, kSearchProcessPrefix)) {
        // 参数是否合法交由执行层判定，分类只认命令名。
        return RemoteCommandKind::SearchProcess;
    }
    if (line == kOpenProcessCommand || startsWith(line, kOpenProcessPrefix)) {
        return RemoteCommandKind::OpenProcess;
    }
    if (line == "help" || line == "--help") {
        return RemoteCommandKind::Help;
    }
    if (line == "exit") {
        return RemoteCommandKind::Exit;
    }
    return RemoteCommandKind::Unknown;
}

RemoteCommandResult executeRemoteCommand(std::string_view line, tpe::ProcessEngine& engine)
{
    switch (classifyRemoteCommand(line)) {
    case RemoteCommandKind::AllProcesses:
        return executeAllProcesses(engine);
    case RemoteCommandKind::SearchProcess:
        return executeSearchProcess(line, engine);
    case RemoteCommandKind::OpenProcess:
        return makeCommandError("--open-process requires a stateful remote session.");
    case RemoteCommandKind::Help:
        return executeHelp();
    case RemoteCommandKind::Exit:
        // Server 侧不会收到 exit：Client 正常结束走 BYE，
        // Phase 01 也不使用 ResponseStatus::Exit，故防御性地按未知命令返回
        // 命令层错误——会话保留，不会被误当成结束信号。
        return makeCommandError(std::string(kUnknownCommandText));
    default: // RemoteCommandKind::Unknown：未交付命令与空输入
        return makeCommandError(std::string(kUnknownCommandText));
    }
}

// ============================================================
// 有状态执行器
// ============================================================

namespace {

std::string stripTrailingSpaces(std::string text)
{
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

std::string logCommandName(RemoteCommandKind kind)
{
    switch (kind) {
    case RemoteCommandKind::AllProcesses:
        return "--all-processes";
    case RemoteCommandKind::SearchProcess:
        return "--search-process";
    case RemoteCommandKind::OpenProcess:
        return "--open-process";
    case RemoteCommandKind::Help:
        return "help";
    case RemoteCommandKind::Exit:
        return "exit";
    case RemoteCommandKind::Unknown:
        break;
    }
    return {};
}

} // namespace

RemoteCommandExecutor::RemoteCommandExecutor(tpe::ProcessEngine& engine)
    : m_engine(engine)
    , m_executor(engine)
{
}

RemoteCommandExecutor::~RemoteCommandExecutor()
{
    release();
}

std::string RemoteCommandExecutor::prompt() const
{
    if (m_layer == RemoteLayer::Connection) {
        return "TPE>";
    }
    return stripTrailingSpaces(m_state.prompt());
}

RemoteCommandResult RemoteCommandExecutor::execute(std::string_view line)
{
    if (m_layer == RemoteLayer::Connection) {
        return executeConnectionCommand(line);
    }
    return executeScanCommand(line);
}

RemoteCommandResult RemoteCommandExecutor::executeConnectionCommand(std::string_view line)
{
    const RemoteCommandKind kind = classifyRemoteCommand(line);
    RemoteCommandResult result;
    result.prompt = "TPE>";

    switch (kind) {
    case RemoteCommandKind::AllProcesses:
    case RemoteCommandKind::SearchProcess: {
        result = executeRemoteCommand(line, m_engine);
        result.prompt = "TPE>";
        result.loggable = true;
        result.logCommand = logCommandName(kind);
        if (kind == RemoteCommandKind::SearchProcess) {
            const PidParseResult parsed = parsePidArgument(
                line.size() > kSearchProcessPrefix.size()
                    ? line.substr(kSearchProcessPrefix.size())
                    : std::string_view{});
            if (parsed.status == PidParseStatus::Ok) {
                result.logPid = static_cast<std::uint32_t>(parsed.value);
            }
        }
        result.logResult = result.status == ResponseStatus::Continue ? LogResult::Ok
                                                                    : LogResult::Error;
        if (kind == RemoteCommandKind::SearchProcess && result.logPid.has_value() &&
            result.status == ResponseStatus::CommandError) {
            const std::string notFound = "Process with PID " +
                                          std::to_string(*result.logPid) + " not found.";
            if (result.output == notFound) {
                result.logResult = LogResult::NotFound;
            }
        }
        return result;
    }
    case RemoteCommandKind::OpenProcess:
        return executeOpenProcess(line);
    case RemoteCommandKind::Help:
        result.output = std::string(kHelpText);
        return result;
    case RemoteCommandKind::Exit:
    case RemoteCommandKind::Unknown:
        result.status = ResponseStatus::CommandError;
        result.output = std::string(kUnknownCommandText);
        return result;
    }
    return result;
}

RemoteCommandResult RemoteCommandExecutor::executeOpenProcess(std::string_view line)
{
    RemoteCommandResult result;
    result.prompt = "TPE>";
    result.loggable = true;
    result.logCommand = std::string(kOpenProcessCommand);

    const std::string_view argument = line.size() > kOpenProcessPrefix.size()
                                          ? line.substr(kOpenProcessPrefix.size())
                                          : std::string_view{};
    const PidParseResult parsed = parsePidArgument(argument);
    if (parsed.status != PidParseStatus::Ok) {
        result.status = ResponseStatus::CommandError;
        result.output = openProcessUsageError(parsed.status);
        result.logResult = LogResult::Error;
        return result;
    }
    result.logPid = static_cast<std::uint32_t>(parsed.value);

    const tpe::Result<tpe::OpenProcessInfo, tpe::PlatformError> opened =
        m_engine.openProcessResult(parsed.value, true);
    if (!opened.has_value()) {
        result.status = ResponseStatus::CommandError;
        result.output = "Failed to open process with PID " + std::to_string(parsed.value) +
                        ": " + opened.error().message;
        result.logResult = LogResult::Error;
        return result;
    }
    if (!isValidUtf8(opened.value().name)) {
        m_engine.closeProcess();
        result.status = ResponseStatus::CommandError;
        result.output = std::string(kUnencodableNameText);
        result.logResult = LogResult::Error;
        return result;
    }

    m_pid = static_cast<std::uint32_t>(parsed.value);
    m_processName = opened.value().name;
    m_state = tpe::cli::ReplState(m_processName);
    m_layer = RemoteLayer::Scan;
    if (prompt().size() > kMaxRemotePromptBytes) {
        release();
        result.status = ResponseStatus::CommandError;
        result.output = std::string(kPromptOversizeText);
        result.logResult = LogResult::Error;
        return result;
    }
    result.prompt = prompt();
    result.logResult = LogResult::Ok;
    return result;
}

RemoteCommandResult RemoteCommandExecutor::executeScanCommand(std::string_view line)
{
    RemoteCommandResult result;
    const tpe::cli::ReplCommand command = tpe::cli::parseReplCommand(std::string(line));
    const tpe::cli::ReplOutcome outcome = tpe::cli::planReplOutcome(command);

    if (command.kind == tpe::cli::ReplCommandKind::Help || outcome == tpe::cli::ReplOutcome::ShowHelp) {
        result.output = std::string(tpe::cli::replHelpText());
        result.prompt = prompt();
        return result;
    }
    if (outcome == tpe::cli::ReplOutcome::Exit ||
        command.kind == tpe::cli::ReplCommandKind::Exit) {
        result.loggable = true;
        result.logCommand = "exit";
        result.logPid = m_pid;
        result.logResult = LogResult::Ok;
        release();
        result.prompt = "TPE>";
        return result;
    }
    if (outcome == tpe::cli::ReplOutcome::Noop) {
        result.prompt = prompt();
        return result;
    }
    if (outcome == tpe::cli::ReplOutcome::UsageError) {
        result.status = ResponseStatus::CommandError;
        result.output = command.error;
        result.prompt = prompt();
        return result;
    }

    // 执行前预测新提示符；超长时保留原扫描状态与提示符。
    if (command.kind == tpe::cli::ReplCommandKind::NewScan ||
        command.kind == tpe::cli::ReplCommandKind::NextScan) {
        const tpe::cli::CliValueType& predictedType =
            command.valueType != nullptr ? *command.valueType : *m_state.valueType;
        std::string predicted = m_processName + "-" +
                                std::string(tpe::cli::scanTypeName(command.scanType)) + "-" +
                                std::string(predictedType.shortName);
        if (command.hasValue) {
            predicted += "-" + command.value;
        }
        predicted += ">";
        if (predicted.size() > kMaxRemotePromptBytes) {
            result.status = ResponseStatus::CommandError;
            result.output = std::string(kPromptOversizeText);
            result.prompt = prompt();
            return result;
        }
    }

    const tpe::cli::ReplExecutionResult executed = m_executor.execute(command, m_state);
    result.output = executed.output;
    result.status = executed.error ? ResponseStatus::CommandError : ResponseStatus::Continue;
    if (!isValidUtf8(result.output)) {
        // UTF-8 校验先于响应容量。
        result.output = "Scan result cannot be encoded as UTF-8.";
        result.status = ResponseStatus::CommandError;
    }
    result.prompt = prompt();

    switch (command.kind) {
    case tpe::cli::ReplCommandKind::NewScan:
        result.loggable = true;
        result.logCommand = "new-scan";
        break;
    case tpe::cli::ReplCommandKind::NextScan:
        result.loggable = true;
        result.logCommand = "next-scan";
        break;
    case tpe::cli::ReplCommandKind::List:
        result.loggable = true;
        result.logCommand = "list";
        break;
    case tpe::cli::ReplCommandKind::Write:
        result.loggable = true;
        result.logCommand = "write";
        break;
    case tpe::cli::ReplCommandKind::Undo:
        result.loggable = true;
        result.logCommand = "undo";
        break;
    default:
        break;
    }
    if (result.loggable) {
        result.logPid = m_pid;
        result.logResult = result.status == ResponseStatus::Continue ? LogResult::Ok
                                                                    : LogResult::Error;
    }
    return result;
}

void RemoteCommandExecutor::release()
{
    m_engine.closeProcess();
    m_pid.reset();
    m_processName.clear();
    m_state = tpe::cli::ReplState();
    m_layer = RemoteLayer::Connection;
}

} // namespace tpe::remote

#include "RemoteCommandExecutor.hpp"

#include "ProcessEngine.hpp"

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
// 固定文本（contracts/cli.md §3/§4/§6）
// ============================================================

/// 帮助文本：4 行，逐字符与契约一致；不含 `TPE> help` 回显行，不含尾随空行。
constexpr std::string_view kHelpText =
    "--all-processes        List processes on the server.\n"
    "--search-process <PID> Show the name of a process on the server.\n"
    "help                   Show this help.\n"
    "exit                   End this client session.";

/// 未知命令文本：未交付的命令（如 `--open-process`）与空输入共用（契约 §3）。
constexpr std::string_view kUnknownCommandText =
    "Unknown command. Type 'help' for available commands.";

/// `--search-process` 的完整命令名，以及带参数时必须以空格分隔的前缀。
constexpr std::string_view kSearchProcessCommand = "--search-process";
constexpr std::string_view kSearchProcessPrefix = "--search-process ";

/// 参数错误以用法开头（契约 §6“用法与原因”），保证输出非空且点明命令名。
constexpr std::string_view kSearchProcessUsage = "Usage: --search-process <PID>.";

/// 结果文本不可编码为 UTF-8 的固定失败文本（契约 §6；protocol.md §5）。
constexpr std::string_view kUnencodableNameText =
    "Process name cannot be encoded as UTF-8.";
constexpr std::string_view kUnencodableListText =
    "Process list contains a name that cannot be encoded as UTF-8.";

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
// 结果文本的严格 UTF-8 校验（契约 §6；protocol.md §5）
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

// ============================================================
// 单条命令的执行
// ============================================================

RemoteCommandResult executeAllProcesses(tpe::ProcessEngine& engine)
{
    // 文本与本地 `getProcessList()` 同源（T016）；枚举失败时附带平台原因（契约 §6）。
    const tpe::Result<std::string, tpe::PlatformError> text = engine.processListText();
    if (!text.has_value()) {
        return makeCommandError("Failed to enumerate processes: " + text.error().message);
    }
    // 先编码、后长度（契约 §6 末段）：这里只做编码校验，长度由 Server 编码响应时判定，
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
    // 名称原样保留、不替换不丢弃；无法编码时按契约 §6 报告固定文本（先编码后长度）。
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
    // 整行精确匹配：前后带空格的已知命令不属于已交付命令（契约 §3）。
    if (line == "--all-processes") {
        return RemoteCommandKind::AllProcesses;
    }
    if (line == kSearchProcessCommand || startsWith(line, kSearchProcessPrefix)) {
        // 参数是否合法交由执行层判定，分类只认命令名。
        return RemoteCommandKind::SearchProcess;
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
    case RemoteCommandKind::Help:
        return executeHelp();
    case RemoteCommandKind::Exit:
        // Server 侧不会收到 exit：Client 正常结束走 BYE（契约 §7），
        // Phase 01 也不使用 ResponseStatus::Exit，故防御性地按未知命令返回
        // 命令层错误——会话保留，不会被误当成结束信号。
        return makeCommandError(std::string(kUnknownCommandText));
    default: // RemoteCommandKind::Unknown：未交付命令与空输入
        return makeCommandError(std::string(kUnknownCommandText));
    }
}

} // namespace tpe::remote

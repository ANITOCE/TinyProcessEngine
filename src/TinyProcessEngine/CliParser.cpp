#include "CliParser.h"

#include <limits>

namespace tpe::cli {

namespace {

bool isWhitespace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f';
}

bool isFlagText(std::string_view text)
{
    return text.size() > 2 && text[0] == '-' && text[1] == '-';
}

std::string_view stripFlagPrefix(std::string_view name)
{
    if (isFlagText(name)) {
        return name.substr(2);
    }
    return name;
}

TerminalCommand usageError(std::string reason)
{
    TerminalCommand command;
    command.kind = TerminalCommandKind::UsageError;
    command.error = std::move(reason);
    return command;
}

bool isDecimalDigits(std::string_view text)
{
    if (text.empty()) {
        return false;
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

/// 按“纯十进制数字且不超出 Pid_t 上限”解析 PID;越界检测在累乘前进行。
bool parsePidText(std::string_view text, tpe::platform::Pid_t& pid)
{
    if (!isDecimalDigits(text)) {
        return false;
    }
    constexpr unsigned long long kMaxPid = (std::numeric_limits<tpe::platform::Pid_t>::max)();
    unsigned long long value = 0;
    for (const char ch : text) {
        const unsigned long long digit = static_cast<unsigned long long>(ch - '0');
        if (value > (kMaxPid - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    pid = static_cast<tpe::platform::Pid_t>(value);
    return true;
}

/// 十六进制地址解析(契约 C-R4):`0x` / `0X` 前缀可省;至少 1 位 hex 数字;
/// 大小写不敏感;必须不超出 tpe::Address 上限(溢出在累乘前检测)。
bool parseHexAddress(std::string_view text, tpe::Address& address)
{
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return false;
    }
    constexpr unsigned long long kMaxAddress = (std::numeric_limits<tpe::Address>::max)();
    unsigned long long value = 0;
    for (const char ch : text) {
        unsigned digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<unsigned>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<unsigned>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            digit = static_cast<unsigned>(ch - 'A' + 10);
        } else {
            return false;
        }
        if (value > (kMaxAddress - digit) / 16) {
            return false;
        }
        value = value * 16 + digit;
    }
    address = static_cast<tpe::Address>(value);
    return true;
}

} // namespace

std::vector<std::string> CliParseResult::flags() const
{
    std::vector<std::string> names;
    for (const CliToken& token : tokens) {
        if (token.isFlag) {
            names.push_back(token.text.substr(2));
        }
    }
    return names;
}

std::vector<const CliToken*> CliParseResult::positionals() const
{
    std::vector<const CliToken*> items;
    for (const CliToken& token : tokens) {
        if (!token.isFlag) {
            items.push_back(&token);
        }
    }
    return items;
}

bool CliParseResult::hasFlag(std::string_view name) const
{
    const std::string_view wanted = stripFlagPrefix(name);
    for (const CliToken& token : tokens) {
        if (token.isFlag && stripFlagPrefix(token.text) == wanted) {
            return true;
        }
    }
    return false;
}

CliParseResult CliParser::parse(const std::string& line)
{
    CliParseResult result;
    result.raw = line;

    std::size_t index = 0;
    while (index < line.size()) {
        if (isWhitespace(line[index])) {
            ++index;
            continue;
        }
        const std::size_t begin = index;
        while (index < line.size() && !isWhitespace(line[index])) {
            ++index;
        }
        CliToken token;
        token.text = line.substr(begin, index - begin);
        token.begin = begin;
        token.end = index;
        token.isFlag = isFlagText(token.text);
        result.tokens.push_back(std::move(token));
    }
    return result;
}

TerminalCommand parseTerminalCommand(const std::vector<std::string>& args)
{
    if (args.empty()) {
        return TerminalCommand{}; // 无参数 ≡ --help(kind 默认 Help)
    }

    const std::string& first = args.front();
    if (first == "--help" || first == "--version" || first == "--all-processes") {
        if (args.size() > 1) {
            return usageError("Unexpected argument: " + args[1]);
        }
        TerminalCommand command;
        if (first == "--help") {
            command.kind = TerminalCommandKind::Help;
        } else if (first == "--version") {
            command.kind = TerminalCommandKind::Version;
        } else {
            command.kind = TerminalCommandKind::AllProcesses;
        }
        return command;
    }

    if (first == "--search-process" || first == "--open-process") {
        if (args.size() < 2) {
            return usageError("Missing PID for " + first + ".");
        }
        if (args.size() > 2) {
            return usageError("Unexpected argument: " + args[2]);
        }
        tpe::platform::Pid_t pid = 0;
        if (!parsePidText(args[1], pid)) {
            return usageError("Invalid PID: " + args[1] +
                              " (expected a non-negative decimal integer).");
        }
        TerminalCommand command;
        command.kind = (first == "--search-process") ? TerminalCommandKind::SearchProcess
                                                     : TerminalCommandKind::OpenProcess;
        command.pid = pid;
        return command;
    }

    if (first.rfind("--", 0) == 0) {
        return usageError("Unknown option: " + first);
    }
    return usageError("Unknown argument: " + first);
}

int parseExitCode(TerminalCommandKind kind)
{
    return kind == TerminalCommandKind::UsageError ? kExitUsageError : kExitOk;
}

std::string_view terminalHelpText()
{
    return
        "TinyProcessEngine - process memory scanner\n"
        "\n"
        "Usage: TinyProcessEngine.exe [command]\n"
        "\n"
        "Commands:\n"
        "  --all-processes         List all processes (PID and process name)\n"
        "  --search-process <PID>  Print the process name for the given PID\n"
        "  --open-process <PID>    Open the process and enter the interactive REPL\n"
        "  --version               Print version information\n"
        "  --help                  Show this help\n"
        "\n"
        "PID is a non-negative decimal process identifier.\n"
        "Without arguments the same help is printed.\n";
}

// ---------------------------------------------------------------------------
// REPL 命令解析(spec 004 US2;契约 C-R1–C-R6)
// ---------------------------------------------------------------------------

namespace {

/// 各命令的用法提示(用法错误统一以 "<原因>\n<Usage>" 呈现;契约 C-R5)。
std::string_view replUsageFor(ReplCommandKind kind)
{
    switch (kind) {
    case ReplCommandKind::NewScan:
        return "Usage: new-scan [--equal] [<value-type>] <value>";
    case ReplCommandKind::NextScan:
        return "Usage: next-scan [--equal] [<value-type>] <value> | next-scan "
               "[--greater|--less|--changed|--unchanged] [<value-type>]";
    case ReplCommandKind::List:
        return "Usage: list [<page>] | list --all";
    case ReplCommandKind::Write:
        return "Usage: write <address> <new-value>";
    case ReplCommandKind::Undo:
        return "Usage: undo";
    case ReplCommandKind::Exit:
        return "Usage: exit";
    default:
        return "Usage: help";
    }
}

ReplCommand replError(ReplCommandKind kind, std::string reason)
{
    ReplCommand command;
    command.kind = kind;
    command.error = std::move(reason);
    command.error += "\n";
    command.error += replUsageFor(kind);
    return command;
}

/// 扫描类型旗标名 → ReplScanType;非扫描旗标返回 false。
bool scanFlagToType(std::string_view name, ReplScanType& out)
{
    if (name == "equal") {
        out = ReplScanType::Equal;
        return true;
    }
    if (name == "unknown") {
        out = ReplScanType::Unknown;
        return true;
    }
    if (name == "greater") {
        out = ReplScanType::Greater;
        return true;
    }
    if (name == "less") {
        out = ReplScanType::Less;
        return true;
    }
    if (name == "changed") {
        out = ReplScanType::Changed;
        return true;
    }
    if (name == "unchanged") {
        out = ReplScanType::Unchanged;
        return true;
    }
    return false;
}

/// 无参数命令(exit / undo):出现任何旗标或位置参数 → 用法错误。
ReplCommand noArgumentCommand(ReplCommandKind kind, const CliParseResult& parsed)
{
    ReplCommand command;
    command.kind = kind;
    if (parsed.tokens.size() > 1) {
        const CliToken& extra = parsed.tokens[1];
        return replError(kind, std::string(extra.isFlag ? "Unknown option: " : "Unexpected argument: ") +
                                   extra.text);
    }
    return command;
}

/// new-scan / next-scan 解析(旗标、取值规则与缺值判定;契约 C-R1/C-R2)。
ReplCommand parseScanCommand(const std::string& line, const CliParseResult& parsed,
                             ReplCommandKind kind)
{
    ReplCommand command;
    command.kind = kind;
    const bool isNewScan = (kind == ReplCommandKind::NewScan);

    bool scanTypeSeen = false;
    const CliToken* stringFlagToken = nullptr;
    std::vector<const CliToken*> positionals;

    for (std::size_t index = 1; index < parsed.tokens.size(); ++index) {
        const CliToken& token = parsed.tokens[index];
        if (!token.isFlag) {
            positionals.push_back(&token);
            continue;
        }
        const std::string_view name = stripFlagPrefix(token.text);
        if (const CliValueType* vt = findCliValueTypeByFlag(name)) {
            if (command.valueType != nullptr) {
                return replError(kind, "Duplicate or conflicting value type option: " + token.text);
            }
            command.valueType = vt;
            if (vt->kind == CliValueKind::String) {
                stringFlagToken = &token;
            }
            continue;
        }
        ReplScanType detected = ReplScanType::Equal;
        if (scanFlagToType(name, detected)) {
            if (scanTypeSeen) {
                return replError(kind, "Duplicate or conflicting scan type option: " + token.text);
            }
            scanTypeSeen = true;
            command.scanType = detected;
            continue;
        }
        return replError(kind, "Unknown option: " + token.text);
    }

    if (stringFlagToken != nullptr) {
        // `--string` 值 = 该旗标之后剩余整行(允许空格;P2 定稿)。
        // 为界定值区:--string 之后不得再出现旗标;其前不得有位置参数。
        for (std::size_t index = 1; index < parsed.tokens.size(); ++index) {
            const CliToken& token = parsed.tokens[index];
            if (token.isFlag && token.begin > stringFlagToken->begin) {
                return replError(kind, "--string must be the last option on the line.");
            }
        }
        for (const CliToken* pos : positionals) {
            if (pos->begin < stringFlagToken->begin) {
                return replError(kind, "Unexpected argument: " + pos->text);
            }
        }
        const std::string remainder = line.substr(stringFlagToken->end);
        const std::size_t start = remainder.find_first_not_of(" \t\r\n\v\f");
        if (start != std::string::npos) {
            command.value = remainder.substr(start);
            command.hasValue = true;
        }
    } else {
        if (positionals.size() > 1) {
            return replError(kind, "Unexpected argument: " + positionals[1]->text);
        }
        if (!positionals.empty()) {
            command.value = positionals[0]->text;
            command.hasValue = true;
        }
    }

    if (isNewScan) {
        switch (command.scanType) {
        case ReplScanType::Changed:
        case ReplScanType::Unchanged:
            return replError(kind, "Unknown option: --" + std::string(scanTypeName(command.scanType)));
        case ReplScanType::Unknown:
            // §11.5:--unknown 不传值;带值属未定义情形 → 按不传值旗标的严格策略拒绝
            if (command.hasValue) {
                return replError(kind, "Unexpected argument: " + command.value);
            }
            command.placeholder = true; // C-R7:占位,仅切换 scan-type(不执行扫描)
            break;
        case ReplScanType::Greater:
        case ReplScanType::Less:
            // §11.5:new-scan 的 --greater / --less 值必传;缺值 → 用法错误(不执行、不切换)
            if (!command.hasValue) {
                return replError(kind, "Missing value for new-scan.");
            }
            command.placeholder = true; // 占位:值仅用于语法识别,不解析、不执行扫描
            break;
        default: // Equal(已实现)
            if (!command.hasValue) {
                return replError(kind, "Missing value for new-scan.");
            }
            break;
        }
    } else {
        if (command.scanType == ReplScanType::Unknown) {
            // FR-013:next-scan MUST NOT 接受 --unknown
            return replError(kind, "next-scan does not accept --unknown.");
        }
        if (command.scanType == ReplScanType::Equal) {
            if (!command.hasValue) {
                return replError(kind, "Missing value for next-scan.");
            }
        } else if (command.hasValue) {
            // 严格策略:不传值条件出现多余位置参数 → 用法错误(不执行)
            return replError(kind, "Unexpected argument: " + command.value);
        }
    }
    return command;
}

/// list 解析(契约 C-R3:--all 或正整数页码;越界判定由 REPL 状态层按总量完成)。
ReplCommand parseListCommand(const CliParseResult& parsed)
{
    constexpr ReplCommandKind kKind = ReplCommandKind::List;
    ReplCommand command;
    command.kind = kKind;

    std::vector<const CliToken*> positionals;
    for (std::size_t index = 1; index < parsed.tokens.size(); ++index) {
        const CliToken& token = parsed.tokens[index];
        if (!token.isFlag) {
            positionals.push_back(&token);
            continue;
        }
        if (token.text != "--all") {
            return replError(kKind, "Unknown option: " + token.text);
        }
        if (command.listAll) {
            return replError(kKind, "Duplicate option: --all");
        }
        command.listAll = true;
    }

    if (command.listAll) {
        if (!positionals.empty()) {
            return replError(kKind, "Unexpected argument: " + positionals[0]->text);
        }
        return command;
    }

    if (positionals.size() > 1) {
        return replError(kKind, "Unexpected argument: " + positionals[1]->text);
    }
    if (positionals.empty()) {
        return command; // 默认第 1 页
    }

    const std::string& text = positionals[0]->text;
    if (!isDecimalDigits(text)) {
        return replError(kKind, "Invalid page: " + text + " (expected a positive integer).");
    }
    constexpr unsigned long long kMaxPage = (std::numeric_limits<unsigned>::max)();
    unsigned long long value = 0;
    for (const char ch : text) {
        const unsigned long long digit = static_cast<unsigned long long>(ch - '0');
        if (value > (kMaxPage - digit) / 10) {
            return replError(kKind, "Invalid page: " + text + " (out of range).");
        }
        value = value * 10 + digit;
    }
    if (value == 0) {
        return replError(kKind, "Invalid page: 0 (expected a positive integer).");
    }
    command.page = static_cast<unsigned>(value);
    return command;
}

/// write 解析(契约 C-R4):第一个位置参数 = 十六进制地址(`0x` 前缀可省);
/// 其后剩余整行 = 值原文(前导空白去除;是否单 token 依当前数值类型在执行时判定)。
ReplCommand parseWriteCommand(const std::string& line, const CliParseResult& parsed)
{
    constexpr ReplCommandKind kKind = ReplCommandKind::Write;
    ReplCommand command;
    command.kind = kKind;

    if (parsed.tokens.size() < 2) {
        return replError(kKind, "Missing address for write.");
    }
    const CliToken& addressToken = parsed.tokens[1];
    if (addressToken.isFlag) {
        // write 语法无旗标(值类型由 REPL 当前 `value-type` 决定)
        return replError(kKind, "Unknown option: " + addressToken.text);
    }

    tpe::Address address = 0;
    if (!parseHexAddress(addressToken.text, address)) {
        return replError(kKind, "Invalid address: " + addressToken.text +
                                     " (expected a hexadecimal address).");
    }
    command.address = address;

    const std::string remainder = line.substr(addressToken.end);
    const std::size_t start = remainder.find_first_not_of(" \t\r\n\v\f");
    if (start == std::string::npos) {
        return replError(kKind, "Missing value for write.");
    }
    command.value = remainder.substr(start);
    command.hasValue = true;
    return command;
}

} // namespace

ReplCommand parseReplCommand(const std::string& line)
{
    const CliParseResult parsed = CliParser::parse(line);

    if (parsed.tokens.empty()) {
        ReplCommand command;
        command.kind = ReplCommandKind::Empty;
        return command;
    }

    const CliToken& first = parsed.tokens.front();
    if (first.isFlag) {
        ReplCommand command; // 旗标开头的行 → 未知命令(显示帮助)
        return command;
    }

    const std::string& name = first.text;
    if (name == "help") {
        ReplCommand command;
        command.kind = ReplCommandKind::Help; // §11.4:参数错误时默认显示帮助
        return command;
    }
    if (name == "exit") {
        return noArgumentCommand(ReplCommandKind::Exit, parsed);
    }
    if (name == "undo") {
        return noArgumentCommand(ReplCommandKind::Undo, parsed);
    }
    if (name == "new-scan") {
        return parseScanCommand(line, parsed, ReplCommandKind::NewScan);
    }
    if (name == "next-scan") {
        return parseScanCommand(line, parsed, ReplCommandKind::NextScan);
    }
    if (name == "list") {
        return parseListCommand(parsed);
    }
    if (name == "write") {
        return parseWriteCommand(line, parsed);
    }

    ReplCommand command; // 未知命令
    return command;
}

ReplOutcome planReplOutcome(const ReplCommand& command)
{
    // 用法错误优先:打印 error、不执行、不退出
    if (!command.error.empty()) {
        return ReplOutcome::UsageError;
    }
    switch (command.kind) {
    case ReplCommandKind::Empty:
        return ReplOutcome::Noop;
    case ReplCommandKind::Help:
    case ReplCommandKind::Unknown:
        return ReplOutcome::ShowHelp;
    case ReplCommandKind::Exit:
        return ReplOutcome::Exit;
    case ReplCommandKind::NewScan:
        // 占位命令(US4 / C-R7):打印固定文案、仅切换提示符 scan-type
        return command.placeholder ? ReplOutcome::Placeholder : ReplOutcome::Execute;
    case ReplCommandKind::NextScan:
    case ReplCommandKind::List:
    case ReplCommandKind::Write:
    case ReplCommandKind::Undo:
        return ReplOutcome::Execute;
    }
    return ReplOutcome::ShowHelp;
}

std::optional<std::string> extractWriteValueText(const ReplCommand& command,
                                                 const CliValueType& valueType,
                                                 std::string& error)
{
    if (valueType.kind == CliValueKind::String) {
        return command.value; // 地址后剩余整行原样(允许空格;P2 定稿)
    }

    // 非 string:必须恰为单个 token(多余 token → 用法错误、不执行)
    const CliParseResult parsed = CliParser::parse(command.value);
    if (parsed.tokens.empty()) {
        error = "Missing value for write.";
        return std::nullopt;
    }
    if (parsed.tokens.size() > 1) {
        error = "Unexpected argument: " + parsed.tokens[1].text;
        return std::nullopt;
    }
    return parsed.tokens[0].text; // 去除了尾随空白(同 new-scan/next-scan 的单 token 规则)
}

std::optional<ScanCondition> toScanCondition(ReplScanType type)
{
    switch (type) {
    case ReplScanType::Equal:
        return ScanCondition::ExactValue;
    case ReplScanType::Greater:
        return ScanCondition::Increased;
    case ReplScanType::Less:
        return ScanCondition::Decreased;
    case ReplScanType::Changed:
        return ScanCondition::Changed;
    case ReplScanType::Unchanged:
        return ScanCondition::Unchanged;
    case ReplScanType::Unknown:
        return std::nullopt; // 仅 new-scan 占位(US4),无引擎条件
    }
    return std::nullopt;
}

std::string_view replHelpText()
{
    return
        "REPL commands:\n"
        "  new-scan [--equal] [--<value-type>] <value>   Start a new scan (destroys previous results)\n"
        "  next-scan [--equal <value>] [--<value-type>]\n"
        "  next-scan [--greater|--less|--changed|--unchanged] [--<value-type>]\n"
        "                                                Filter the previous scan results\n"
        "  list [<page>] | list --all                    Show matches (20 per page)\n"
        "  write <address> <new-value>                   Overwrite the value at an address\n"
        "  undo                                          Restore the previous match set\n"
        "  help                                          Show this help\n"
        "  exit                                          Leave the REPL (exit code 0)\n"
        "\n"
        "Value types: u8, i16, i32 (default), i64, float, double, string\n"
        "Scan types (new-scan): --equal <value> (default) | --unknown | --greater <value> | --less <value>\n"
        "The --unknown / --greater / --less scan types are recognized but not implemented yet.\n"
        "The --string value takes the rest of the line (spaces allowed).\n";
}

std::string_view replPlaceholderText()
{
    // §11.8 / FR-020:逐字固定,不得本地化或改写
    return "This feature is not implemented yet.";
}

} // namespace tpe::cli

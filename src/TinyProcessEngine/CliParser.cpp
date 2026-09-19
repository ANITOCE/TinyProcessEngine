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
bool parsePidText(std::string_view text, Pid_t& pid)
{
    if (!isDecimalDigits(text)) {
        return false;
    }
    constexpr unsigned long long kMaxPid = (std::numeric_limits<Pid_t>::max)();
    unsigned long long value = 0;
    for (const char ch : text) {
        const unsigned long long digit = static_cast<unsigned long long>(ch - '0');
        if (value > (kMaxPid - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    pid = static_cast<Pid_t>(value);
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
        Pid_t pid = 0;
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

int parseExitCode(TerminalCommandKind)
{
    // TDD 红阶段占位:映射行为待实现;断言失败后于绿阶段实现。
    return -1;
}

} // namespace tpe::cli

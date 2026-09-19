#include "CliParser.h"

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

} // namespace tpe::cli

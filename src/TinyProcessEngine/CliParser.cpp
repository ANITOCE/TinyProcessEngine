#include "CliParser.h"

namespace tpe::cli {

std::vector<std::string> CliParseResult::flags() const
{
    return {};
}

std::vector<const CliToken*> CliParseResult::positionals() const
{
    return {};
}

bool CliParseResult::hasFlag(std::string_view) const
{
    return false;
}

CliParseResult CliParser::parse(const std::string&)
{
    return {};
}

} // namespace tpe::cli

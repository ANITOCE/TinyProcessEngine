#include "CliValueType.h"

namespace tpe::cli {

const std::vector<CliValueType>& cliValueTypes()
{
    static const std::vector<CliValueType> kNone;
    return kNone;
}

const CliValueType* findCliValueTypeByFlag(std::string_view)
{
    return nullptr;
}

const CliValueType* findCliValueTypeByShortName(std::string_view)
{
    return nullptr;
}

const CliValueType& defaultCliValueType()
{
    static const CliValueType kDefault;
    return kDefault;
}

} // namespace tpe::cli

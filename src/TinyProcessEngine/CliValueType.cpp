#include "CliValueType.hpp"

#include <cassert>

namespace tpe::cli {

namespace {

// 静态实例:注册 7 个 CLI 值类型;ValueType 无状态,生命周期覆盖进程
const UnsignedByte kUnsignedByte{};
const Int16 kInt16{};
const Int32 kInt32{};
const Int64 kInt64{};
const Float kFloat{};
const Double kDouble{};
const String kString{};

std::string_view stripFlagPrefix(std::string_view name)
{
    if (name.size() > 2 && name[0] == '-' && name[1] == '-') {
        return name.substr(2);
    }
    return name;
}

} // namespace

const std::vector<CliValueType>& cliValueTypes()
{
    static const std::vector<CliValueType> kTypes{
        {"u8", "u8", CliValueKind::U8, &kUnsignedByte},
        {"i16", "i16", CliValueKind::I16, &kInt16},
        {"i32", "i32", CliValueKind::I32, &kInt32},
        {"i64", "i64", CliValueKind::I64, &kInt64},
        {"float", "float", CliValueKind::Float, &kFloat},
        {"double", "double", CliValueKind::Double, &kDouble},
        {"string", "string", CliValueKind::String, &kString},
    };
    return kTypes;
}

const CliValueType* findCliValueTypeByFlag(std::string_view flag)
{
    const std::string_view wanted = stripFlagPrefix(flag);
    for (const CliValueType& entry : cliValueTypes()) {
        if (entry.flagName == wanted) {
            return &entry;
        }
    }
    return nullptr;
}

const CliValueType* findCliValueTypeByShortName(std::string_view shortName)
{
    for (const CliValueType& entry : cliValueTypes()) {
        if (entry.shortName == shortName) {
            return &entry;
        }
    }
    return nullptr;
}

const CliValueType& defaultCliValueType()
{
    const CliValueType* entry = findCliValueTypeByFlag("i32");
    assert(entry != nullptr);
    return *entry;
}

} // namespace tpe::cli

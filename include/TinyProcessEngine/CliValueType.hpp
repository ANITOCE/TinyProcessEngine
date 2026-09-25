#pragma once

#include <string_view>
#include <vector>

#include "ValueType.hpp"

namespace tpe::cli {

/// CLI 值类型类别:决定解析宽度与展示格式(见 ValueFormatter)。
enum class CliValueKind { U8, I16, I32, I64, Float, Double, String };

/// 一项 CLI 值类型映射:旗标名 ↔ 短名 ↔ ValueType 实现。
struct CliValueType {
    std::string_view flagName;       // 旗标名(不含 "--",如 "u8")
    std::string_view shortName;      // 短名(如 "u8")
    CliValueKind kind = CliValueKind::I32;
    const ValueType* type = nullptr; // 静态实例,生命周期覆盖进程
};

/// 全部 7 项映射(固定顺序:u8 / i16 / i32 / i64 / float / double / string)。
const std::vector<CliValueType>& cliValueTypes();

/// 按旗标名查找(接受 "u8" 或 "--u8");未命中返回 nullptr。
const CliValueType* findCliValueTypeByFlag(std::string_view flag);

/// 按短名精确查找(如 "i16",不接受 "--" 前缀);未命中返回 nullptr。
const CliValueType* findCliValueTypeByShortName(std::string_view shortName);

/// 默认值类型:i32(规范 §11.5)。
const CliValueType& defaultCliValueType();

} // namespace tpe::cli

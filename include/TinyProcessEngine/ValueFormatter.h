#pragma once

#include <string>

#include "CliValueType.h"
#include "ScanTypes.h"

namespace tpe::cli {

/// 地址 → "0x" + 16 位大写零填充 hex(规范 §11.6,如 0x00000000001C0A10)。
std::string formatAddress(tpe::Address address);

/// 扫描快照值 → 展示文本(按 CLI 值类型):
/// - u8 → 无符号十进制;i16 / i32 / i64 → 有符号十进制;
/// - float / double → C++ 默认流式格式化;
/// - string → 原样字节。
/// 快照字节按小端解码;快照宽度 ≠ 类型宽度(除 string,按原样)时回退为十六进制字节串。
std::string formatValue(const ScanRecord& record, const CliValueType& valueType);

} // namespace tpe::cli

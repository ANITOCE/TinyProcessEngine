#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "CliValueType.hpp"
#include "ScanTypes.hpp"

namespace tpe::cli {

/// 地址 → "0x" + 16 位大写零填充 hex(规范 §11.6,如 0x00000000001C0A10)。
std::string formatAddress(tpe::Address address);

/// 展示字节 → 文本(按 CLI 值类型;`formatValue`(快照)与 `formatValueBytes`(实时)共用规则):
/// - u8 → 无符号十进制;i16 / i32 / i64 → 有符号十进制;
/// - float / double → C++ 默认流式格式化;
/// - string → 原样字节。
/// 字节按小端解码;宽度 ≠ 类型宽度(除 string,按原样)时回退为十六进制字节串。
std::string formatValueBytes(const tpe::Memory& bytes, const CliValueType& valueType);

/// 扫描快照值 → 展示文本(委托 `formatValueBytes`;规则与输出保持一致)。
std::string formatValue(const ScanRecord& record, const CliValueType& valueType);

/// `list --all` 单次展示上限(规范 §11.6:10000 条)。
inline constexpr std::uint64_t kListDisplayCap = 10000;

/// list 条目行:`  <0x16位hex> | <值>`(规范 §11.6 示例格式;契约 C-R3)。
std::string formatListEntry(const ScanRecord& record, const CliValueType& valueType);

/// list 条目行(实时值版;FR-013–016/C-D6):`  <0x16位hex> | <值>`;
/// `liveBytes == nullopt`(读取失败 / 短读 / 零宽)→ 值列逐字 `??`(不回退快照)。
std::string formatListEntry(tpe::Address address, const std::optional<tpe::Memory>& liveBytes,
                            const CliValueType& valueType);

/// `list --all` 首行:`Total: <N> matches`(规范 §11.6;恒用 "matches")。
std::string formatMatchesTotal(std::uint64_t total);

/// `list --all` 截断追加行:`... and <N> more`(N = 未显示条数;规范 §11.6)。
std::string formatTruncationNotice(std::uint64_t hidden);

} // namespace tpe::cli

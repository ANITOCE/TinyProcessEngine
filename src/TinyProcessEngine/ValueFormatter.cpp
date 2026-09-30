#include "ValueFormatter.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <type_traits>

namespace tpe::cli {

namespace {

// 从快照字节按小端还原 T:先组装低位字节,再按位拷贝(避免 unsigned→signed 的实现定义转换)
template <class T>
T decodeLittleEndian(const std::uint8_t* bytes)
{
    static_assert(std::is_trivially_copyable<T>::value, "snapshot decode requires trivial types");
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bits |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    }
    T value{};
    std::memcpy(&value, &bits, sizeof(T));
    return value;
}

// 快照宽度与类型宽度不符时的回退:内存序十六进制字节串(大写、空格分隔)
std::string hexBytes(const std::uint8_t* data, std::size_t size)
{
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        if (i > 0) {
            out.push_back(' ');
        }
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

// C++ 默认流式格式化(浮点按解码后的值原样输出)
template <class T>
std::string toDefaultText(T value)
{
    std::ostringstream out;
    out << value;
    return out.str();
}

} // namespace

std::string formatAddress(tpe::Address address)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0')
        << static_cast<std::uint64_t>(address);
    return out.str();
}

std::string formatValueBytes(const tpe::Memory& bytes, const CliValueType& valueType)
{
    const std::uint8_t* data = bytes.data();
    const std::size_t size = bytes.size();

    switch (valueType.kind) {
    case CliValueKind::U8:
        if (size != sizeof(std::uint8_t)) {
            return hexBytes(data, size);
        }
        return std::to_string(static_cast<unsigned>(decodeLittleEndian<std::uint8_t>(data)));
    case CliValueKind::I16:
        if (size != sizeof(std::int16_t)) {
            return hexBytes(data, size);
        }
        return std::to_string(decodeLittleEndian<std::int16_t>(data));
    case CliValueKind::I32:
        if (size != sizeof(std::int32_t)) {
            return hexBytes(data, size);
        }
        return std::to_string(decodeLittleEndian<std::int32_t>(data));
    case CliValueKind::I64:
        if (size != sizeof(std::int64_t)) {
            return hexBytes(data, size);
        }
        return std::to_string(decodeLittleEndian<std::int64_t>(data));
    case CliValueKind::Float:
        if (size != sizeof(float)) {
            return hexBytes(data, size);
        }
        return toDefaultText(decodeLittleEndian<float>(data));
    case CliValueKind::Double:
        if (size != sizeof(double)) {
            return hexBytes(data, size);
        }
        return toDefaultText(decodeLittleEndian<double>(data));
    case CliValueKind::String:
        // string 无固定宽度:字节按原样输出;空字节序列 → 空串(空 vector 的 data() 可能为空)
        if (size == 0) {
            return {};
        }
        return std::string(reinterpret_cast<const char*>(data), size);
    }
    return {};
}

std::string formatValue(const ScanRecord& record, const CliValueType& valueType)
{
    // 委托实时字节路径(同规则);快照 ≤8 字节,复制成本可忽略。
    // 防御性截断:snapshot_size 语义上限 = sizeof(snapshot_data)(构造器已保证)。
    const std::size_t size =
        (std::min)(static_cast<std::size_t>(record.snapshot_size), sizeof(record.snapshot_data));
    return formatValueBytes(tpe::Memory(record.snapshot_data, record.snapshot_data + size),
                            valueType);
}

// ---------------------------------------------------------------------------
// list 渲染(契约 C-R3 / 规范 §11.6)
// ---------------------------------------------------------------------------

std::string formatListEntry(const ScanRecord& record, const CliValueType& valueType)
{
    // "  " + 0x16位hex + " | " + 值(规范 §11.6 示例)
    std::string out = "  ";
    out += formatAddress(record.address);
    out += " | ";
    out += formatValue(record, valueType);
    return out;
}

std::string formatListEntry(tpe::Address address, const std::optional<tpe::Memory>& liveBytes,
                            const CliValueType& valueType)
{
    // "  " + 0x16位hex + " | " + 值(实时读取;C-D6/E4:
    // nullopt = 读取失败/短读/零宽 → 值列逐字 `??`,不回退快照)
    std::string out = "  ";
    out += formatAddress(address);
    out += " | ";
    out += liveBytes.has_value() ? formatValueBytes(*liveBytes, valueType) : "??";
    return out;
}

std::string formatMatchesTotal(std::uint64_t total)
{
    return "Total: " + std::to_string(total) + " matches";
}

std::string formatTruncationNotice(std::uint64_t hidden)
{
    return "... and " + std::to_string(hidden) + " more";
}

} // namespace tpe::cli

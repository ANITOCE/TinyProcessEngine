#ifndef _VALUETYPE_H_
#define _VALUETYPE_H_

#include <string>
#include <cstdint>
#include <cassert>
#include <cstddef>
#include <sstream>
#include <memory>
#include <array>
#include <vector>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

#include "MemoryPage.h"
#include "HelpFunction.h"

struct ValueType
{
    static const ValueType &choose_type();

    const std::string name;

    ValueType(const std::string &name) : name{name} {}
    virtual tpe::Memory askValue() const = 0;

    /// 非交互解析:text → 内存表示(小端);失败时写入英文 error 并返回 nullopt。
    /// 由 `SimpleValueType<T>` 提供通用实现(见本头文件);本 Phase 的 7 个 CLI 类型必须可用。
    virtual std::optional<tpe::Memory> parse(std::string_view text, std::string &error) const;

    /// 类型的内存宽度(字节);0 = 变长(宽度由具体值决定,如 string)。
    /// 供非交互扫描确定读取宽度,替代交互式 askValue().size()(spec 004 T022a)。
    virtual std::size_t byteWidth() const { return 0; }

    virtual ~ValueType() = default;
};



template <class T>
struct SimpleValueType : public ValueType
{
    SimpleValueType(const std::string &name) : ValueType(name) {}

    tpe::Memory askValue() const override;

    /// 固定宽度类型:内存宽度即 sizeof(T)。
    std::size_t byteWidth() const override { return sizeof(T); }

    /// 通用非交互解析:整数按 int64 解析后按 T 范围与 isValid 校验;
    /// 浮点等其它类型按 T 直接解析;两者都必须完整消费输入文本。
    std::optional<tpe::Memory> parse(std::string_view text, std::string &error) const override;

    virtual tpe::Memory representation(const T &value) const;
    virtual bool isValid(const T &value) const { return true; }
    virtual std::istream &read(std::istream &in, T &t) const { return in >> t; }
};

// since uint8_t is a char but the user is expecting to enter a number, we need
// to ask for a regular int and cast
struct UnsignedByte : SimpleValueType<std::uint32_t>
{
    UnsignedByte() : SimpleValueType{"unsigned byte"} {}

    bool isValid(const std::uint32_t &value) const override
    {
        return 0 <= value && value <= 255; // check that it is 8 bits
    }
    tpe::Memory representation(const int32_t &value) const
    {
        uint8_t byte = value;
        return {static_cast<tpe::Byte>(byte)}; // only extract 1 byte
    }
};

struct Character : SimpleValueType<char>
{
    Character() : SimpleValueType{"character"} {}
};

struct Int16 : SimpleValueType<std::int16_t>
{
    Int16() : SimpleValueType{"16-bit integer"} {}
};

struct Int32 : SimpleValueType<std::int32_t>
{
    Int32() : SimpleValueType{"32-bit integer"} {}
};

struct Int64 : SimpleValueType<std::int64_t>
{
    Int64() : SimpleValueType{"64-bit integer"} {}
};

struct Float : SimpleValueType<float>
{
    Float() : SimpleValueType{"float"} {}
};

struct Double : SimpleValueType<double>
{
    Double() : SimpleValueType{"double"} {}
};

struct String : SimpleValueType<std::string>
{
    String() : SimpleValueType{"string"} {}

    /// string 为变长:宽度由具体值决定(0 = 变长标记)。
    std::size_t byteWidth() const override { return 0; }

    std::istream &read(std::istream &in, std::string &value) const override
    {
        in.ignore();
        return std::getline(in, value);
    }

    /// 整段文本按字节拷贝(允许空格);空文本视为缺值错误。
    std::optional<tpe::Memory> parse(std::string_view text, std::string &error) const override
    {
        if (text.empty()) {
            error = "empty " + name;
            return std::nullopt;
        }
        return tpe::Memory(text.begin(), text.end());
    }

    tpe::Memory representation(const std::string &value) const override
    {
        // we can't look at the direct std::string representation, we need to copy
        // the actual chars (we're not searching for the internal char* and other
        // members, we're looking for the actual chars!)
        return tpe::Memory(std::begin(value), std::end(value));
    }
};

template <class T>
tpe::Memory SimpleValueType<T>::askValue() const
{
    std::stringstream query, error;
    query << "Value for " << name;
    error << "Invalid " << name;
    const Validator<T> validate = [&](const T &value)
    { return isValid(value); };
    const Reader<T> read_value = [&](std::istream &in, T &t) -> std::istream &
    { return read(in, t); };
    const auto value = ask_for<T>(query.str(), error.str(), validate, read_value);
    return representation(value);
}

template <class T>
std::optional<tpe::Memory> SimpleValueType<T>::parse(std::string_view text, std::string &error) const
{
    if (text.empty()) {
        error = "empty " + name;
        return std::nullopt;
    }

    std::istringstream in{std::string(text)};

    if constexpr (std::is_integral_v<T>) {
        // 整数统一按 int64 解析,再按 T 的数值范围与 isValid 校验(拒绝 300 于 16-bit 等)
        std::int64_t parsed = 0;
        if (!(in >> parsed)) {
            error = "invalid " + name;
            return std::nullopt;
        }
        in >> std::ws;
        if (!in.eof()) {
            error = "invalid " + name;
            return std::nullopt;
        }
        // 注:Platform.h 链路会引入 Windows.h 的 min/max 宏,故用括号形式取边界
        if (parsed < static_cast<std::int64_t>((std::numeric_limits<T>::min)()) ||
            parsed > static_cast<std::int64_t>((std::numeric_limits<T>::max)())) {
            error = "out of range for " + name;
            return std::nullopt;
        }
        const T value = static_cast<T>(parsed);
        if (!isValid(value)) {
            error = "out of range for " + name;
            return std::nullopt;
        }
        return representation(value);
    } else {
        // 浮点等其它类型:按 T 直接解析(仍要求完整消费)
        T value{};
        if (!(in >> value)) {
            error = "invalid " + name;
            return std::nullopt;
        }
        in >> std::ws;
        if (!in.eof()) {
            error = "invalid " + name;
            return std::nullopt;
        }
        if (!isValid(value)) {
            error = "out of range for " + name;
            return std::nullopt;
        }
        return representation(value);
    }
}

template <class T>
tpe::Memory SimpleValueType<T>::representation(const T &value) const
{
    auto bytes = reinterpret_cast<const tpe::Byte *>(&value);
    return tpe::Memory(bytes, bytes + sizeof(T));
}

#endif // _VALUETYPE_H_
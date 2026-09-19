#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "ValueType.h"

namespace {

std::vector<tpe::Byte> bytes(std::initializer_list<unsigned> values)
{
    std::vector<tpe::Byte> out;
    out.reserve(values.size());
    for (const unsigned value : values) {
        out.push_back(static_cast<tpe::Byte>(value));
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// ValueType::parse — 各类型成功路径(小端表示)
// ---------------------------------------------------------------------------

TEST(ValueParse, Int32ParsesSignedDecimalLittleEndian)
{
    const Int32 type;
    std::string error;

    const auto positive = type.parse("42", error);
    ASSERT_TRUE(positive.has_value()) << error;
    EXPECT_EQ(*positive, bytes({0x2A, 0x00, 0x00, 0x00}));

    const auto negative = type.parse("-2", error);
    ASSERT_TRUE(negative.has_value()) << error;
    EXPECT_EQ(*negative, bytes({0xFE, 0xFF, 0xFF, 0xFF}));
}

TEST(ValueParse, Int16ParsesSignedDecimalLittleEndian)
{
    const Int16 type;
    std::string error;

    const auto result = type.parse("300", error);
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_EQ(*result, bytes({0x2C, 0x01}));

    const auto upper = type.parse("32767", error);
    ASSERT_TRUE(upper.has_value()) << error;
    EXPECT_EQ(*upper, bytes({0xFF, 0x7F}));

    const auto lower = type.parse("-32768", error);
    ASSERT_TRUE(lower.has_value()) << error;
    EXPECT_EQ(*lower, bytes({0x00, 0x80}));
}

TEST(ValueParse, Int64ParsesSignedDecimalLittleEndian)
{
    const Int64 type;
    std::string error;

    const auto result = type.parse("1099511627776", error); // 2^40
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_EQ(*result, bytes({0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}));

    const auto upper = type.parse("9223372036854775807", error); // INT64_MAX
    ASSERT_TRUE(upper.has_value()) << error;
    EXPECT_EQ(*upper, bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F}));
}

TEST(ValueParse, UnsignedByteAcceptsRangeBoundaries)
{
    const UnsignedByte type;
    std::string error;

    const auto low = type.parse("0", error);
    ASSERT_TRUE(low.has_value()) << error;
    ASSERT_FALSE(low->empty());
    EXPECT_EQ(low->front(), static_cast<tpe::Byte>(0x00));

    const auto high = type.parse("255", error);
    ASSERT_TRUE(high.has_value()) << error;
    ASSERT_FALSE(high->empty());
    EXPECT_EQ(high->front(), static_cast<tpe::Byte>(0xFF));
    // 注:u8 序列化宽度(4 字节回退)的修复属开发指南 Phase 05 范围,本用例只断言最低字节
}

TEST(ValueParse, FloatParsesDecimalLittleEndian)
{
    const Float type;
    std::string error;

    const auto result = type.parse("1.5", error);
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_EQ(*result, bytes({0x00, 0x00, 0xC0, 0x3F}));

    const auto negative = type.parse("-2.25", error);
    ASSERT_TRUE(negative.has_value()) << error;
    EXPECT_EQ(*negative, bytes({0x00, 0x00, 0x10, 0xC0}));
}

TEST(ValueParse, DoubleParsesDecimalLittleEndian)
{
    const Double type;
    std::string error;

    const auto result = type.parse("2.5", error);
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_EQ(*result, bytes({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40}));
}

TEST(ValueParse, StringTakesWholeTextAsBytes)
{
    const String type;
    std::string error;

    const auto result = type.parse("abc def", error);
    ASSERT_TRUE(result.has_value()) << error;
    EXPECT_EQ(*result, bytes({'a', 'b', 'c', ' ', 'd', 'e', 'f'}));

    const auto folded = type.parse("a  b", error);
    ASSERT_TRUE(folded.has_value()) << error;
    EXPECT_EQ(*folded, bytes({'a', ' ', ' ', 'b'}));
}

// ---------------------------------------------------------------------------
// ValueType::parse — 失败路径(空文本 / 非数字 / 尾随垃圾 / 越界)
// ---------------------------------------------------------------------------

TEST(ValueParse, RejectsEmptyTextForEveryType)
{
    const UnsignedByte u8;
    const Int16 i16;
    const Int32 i32;
    const Int64 i64;
    const Float f;
    const Double d;
    const String s;

    std::string error;
    for (const ValueType* type : {static_cast<const ValueType*>(&u8),
                                  static_cast<const ValueType*>(&i16),
                                  static_cast<const ValueType*>(&i32),
                                  static_cast<const ValueType*>(&i64),
                                  static_cast<const ValueType*>(&f),
                                  static_cast<const ValueType*>(&d),
                                  static_cast<const ValueType*>(&s)}) {
        error.clear();
        EXPECT_FALSE(type->parse("", error).has_value()) << type->name;
        EXPECT_FALSE(error.empty()) << type->name;
    }
}

TEST(ValueParse, RejectsTrailingGarbage)
{
    const Int32 i32;
    const Int64 i64;
    const Float f;
    const Double d;

    std::string error;
    error.clear();
    EXPECT_FALSE(i32.parse("12abc", error).has_value());
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(i64.parse("1234567890123x", error).has_value());

    error.clear();
    EXPECT_FALSE(f.parse("1.5abc", error).has_value());

    error.clear();
    EXPECT_FALSE(d.parse("2.5x", error).has_value());
}

TEST(ValueParse, RejectsNonNumericText)
{
    const Int32 i32;
    const Float f;

    std::string error;
    error.clear();
    EXPECT_FALSE(i32.parse("abc", error).has_value());
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(i32.parse("0x10", error).has_value()); // 值输入只接受十进制

    error.clear();
    EXPECT_FALSE(f.parse("abc", error).has_value());
}

TEST(ValueParse, RejectsOutOfRangeIntegersWithRangeError)
{
    const UnsignedByte u8;
    const Int16 i16;
    const Int32 i32;

    std::string error;
    error.clear();
    EXPECT_FALSE(i16.parse("32768", error).has_value());
    EXPECT_NE(error.find("range"), std::string::npos) << error;

    error.clear();
    EXPECT_FALSE(i32.parse("3000000000", error).has_value());
    EXPECT_NE(error.find("range"), std::string::npos) << error;

    error.clear();
    EXPECT_FALSE(u8.parse("256", error).has_value());
    EXPECT_NE(error.find("range"), std::string::npos) << error;

    error.clear();
    EXPECT_FALSE(u8.parse("-1", error).has_value());
    EXPECT_NE(error.find("range"), std::string::npos) << error;
}

TEST(ValueParse, RejectsOutOfRangeFloatsAndInt64Overflow)
{
    const Int64 i64;
    const Float f;
    const Double d;

    std::string error;
    error.clear();
    EXPECT_FALSE(i64.parse("9223372036854775808", error).has_value());

    error.clear();
    EXPECT_FALSE(f.parse("1e40", error).has_value());

    error.clear();
    EXPECT_FALSE(d.parse("1e400", error).has_value());
}

#include <gtest/gtest.h>

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "CliParser.h"
#include "CliValueType.h"
#include "ValueFormatter.h"
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

// ---------------------------------------------------------------------------
// write 值转换(US3 T026;契约 C-R4:值文本 → ValueType::parse)
// ---------------------------------------------------------------------------

TEST(WriteValue, ConvertsSingleTokenToTypeRepresentation)
{
    const tpe::cli::CliValueType* i32 = tpe::cli::findCliValueTypeByShortName("i32");
    ASSERT_NE(i32, nullptr);
    const tpe::cli::ReplCommand command = tpe::cli::parseReplCommand("write 0x10 -2");
    ASSERT_EQ(command.kind, tpe::cli::ReplCommandKind::Write);

    std::string error;
    const std::optional<std::string> text =
        tpe::cli::extractWriteValueText(command, *i32, error);
    ASSERT_TRUE(text.has_value()) << error;
    const std::optional<tpe::Memory> data = i32->type->parse(*text, error);
    ASSERT_TRUE(data.has_value()) << error;
    EXPECT_EQ(*data, bytes({0xFE, 0xFF, 0xFF, 0xFF}));
}

TEST(WriteValue, StringWriteKeepsSpacesAndConvertsRawBytes)
{
    const tpe::cli::CliValueType* stringType = tpe::cli::findCliValueTypeByShortName("string");
    ASSERT_NE(stringType, nullptr);
    const tpe::cli::ReplCommand command = tpe::cli::parseReplCommand("write 0x10 hello world");
    ASSERT_EQ(command.kind, tpe::cli::ReplCommandKind::Write);

    std::string error;
    const std::optional<std::string> text =
        tpe::cli::extractWriteValueText(command, *stringType, error);
    ASSERT_TRUE(text.has_value()) << error;
    const std::optional<tpe::Memory> data = stringType->type->parse(*text, error);
    ASSERT_TRUE(data.has_value()) << error;
    EXPECT_EQ(*data, bytes({'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'}));
}

TEST(WriteValue, OutOfRangeValueIsRejectedByTargetTypeParse)
{
    const tpe::cli::CliValueType* u8 = tpe::cli::findCliValueTypeByShortName("u8");
    ASSERT_NE(u8, nullptr);
    const tpe::cli::ReplCommand command = tpe::cli::parseReplCommand("write 0x10 256");
    ASSERT_EQ(command.kind, tpe::cli::ReplCommandKind::Write);

    std::string error;
    const std::optional<std::string> text =
        tpe::cli::extractWriteValueText(command, *u8, error);
    ASSERT_TRUE(text.has_value()) << error;
    error.clear();
    EXPECT_FALSE(u8->type->parse(*text, error).has_value());
    EXPECT_NE(error.find("range"), std::string::npos) << error;
}

// ---------------------------------------------------------------------------
// CliValueType — 旗标 ↔ 短名 ↔ ValueType 映射
// ---------------------------------------------------------------------------

TEST(CliValueType, ExposesSevenTypesInFixedOrder)
{
    const std::vector<tpe::cli::CliValueType>& types = tpe::cli::cliValueTypes();
    ASSERT_EQ(types.size(), 7u);
    const char* expected[] = {"u8", "i16", "i32", "i64", "float", "double", "string"};
    for (std::size_t i = 0; i < types.size(); ++i) {
        EXPECT_EQ(types[i].flagName, expected[i]);
        EXPECT_EQ(types[i].shortName, expected[i]);
        EXPECT_NE(types[i].type, nullptr) << types[i].shortName;
    }
}

TEST(CliValueType, LooksUpByFlagWithOrWithoutPrefix)
{
    const tpe::cli::CliValueType* plain = tpe::cli::findCliValueTypeByFlag("i16");
    const tpe::cli::CliValueType* prefixed = tpe::cli::findCliValueTypeByFlag("--i16");
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(prefixed, nullptr);
    EXPECT_EQ(plain, prefixed);
    EXPECT_EQ(plain->kind, tpe::cli::CliValueKind::I16);
    EXPECT_EQ(plain->type->name, std::string("16-bit integer"));
}

TEST(CliValueType, LooksUpByShortName)
{
    const tpe::cli::CliValueType* entry = tpe::cli::findCliValueTypeByShortName("string");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->kind, tpe::cli::CliValueKind::String);
    EXPECT_EQ(entry->type->name, std::string("string"));
}

TEST(CliValueType, ReturnsNullForUnknownNames)
{
    EXPECT_EQ(tpe::cli::findCliValueTypeByFlag("unknown"), nullptr);
    EXPECT_EQ(tpe::cli::findCliValueTypeByFlag("--nope"), nullptr);
    EXPECT_EQ(tpe::cli::findCliValueTypeByShortName("nope"), nullptr);
    EXPECT_EQ(tpe::cli::findCliValueTypeByShortName("--i32"), nullptr); // 短名查找不剥前缀
}

TEST(CliValueType, DefaultsToI32)
{
    const tpe::cli::CliValueType& entry = tpe::cli::defaultCliValueType();
    EXPECT_EQ(entry.kind, tpe::cli::CliValueKind::I32);
    EXPECT_EQ(entry.flagName, "i32");
    EXPECT_EQ(entry.type, tpe::cli::findCliValueTypeByFlag("i32")->type);
}

TEST(CliValueType, EveryMappedValueTypeParsesText)
{
    std::string error;
    for (const tpe::cli::CliValueType& entry : tpe::cli::cliValueTypes()) {
        ASSERT_NE(entry.type, nullptr) << entry.shortName;
        const auto parsed = entry.type->parse("42", error);
        EXPECT_TRUE(parsed.has_value()) << entry.shortName << ": " << error;
    }
}

// ---------------------------------------------------------------------------
// ValueFormatter — 地址与快照值格式化
// ---------------------------------------------------------------------------

namespace {

ScanRecord recordWith(tpe::Address address, std::initializer_list<unsigned> snapshot)
{
    tpe::Memory memory;
    memory.reserve(snapshot.size());
    for (const unsigned value : snapshot) {
        memory.push_back(static_cast<tpe::Byte>(value));
    }
    return ScanRecord{address, memory};
}

const tpe::cli::CliValueType& cliType(std::string_view name)
{
    const tpe::cli::CliValueType* entry = tpe::cli::findCliValueTypeByFlag(name);
    assert(entry != nullptr);
    return *entry;
}

} // namespace

TEST(ValueFormatter, FormatsAddressAs16DigitUppercaseHex)
{
    EXPECT_EQ(tpe::cli::formatAddress(0x1C0A10), "0x00000000001C0A10");
    EXPECT_EQ(tpe::cli::formatAddress(0), "0x0000000000000000");
    EXPECT_EQ(tpe::cli::formatAddress(~static_cast<tpe::Address>(0)), "0xFFFFFFFFFFFFFFFF");
}

TEST(ValueFormatter, FormatsSignedIntegersAsDecimal)
{
    EXPECT_EQ(tpe::cli::formatValue(recordWith(0x1000, {0x2A, 0x00, 0x00, 0x00}), cliType("i32")), "42");
    EXPECT_EQ(tpe::cli::formatValue(recordWith(0x1000, {0xFE, 0xFF, 0xFF, 0xFF}), cliType("i32")), "-2");
    EXPECT_EQ(tpe::cli::formatValue(recordWith(0x1000, {0xFF, 0x7F}), cliType("i16")), "32767");
    EXPECT_EQ(tpe::cli::formatValue(recordWith(0x1000, {0xFF, 0xFF}), cliType("i16")), "-1");
    EXPECT_EQ(tpe::cli::formatValue(
                  recordWith(0x1000, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), cliType("i64")),
              "-1");
    EXPECT_EQ(tpe::cli::formatValue(
                  recordWith(0x1000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}), cliType("i64")),
              "1099511627776");
}

TEST(ValueFormatter, FormatsUnsignedByteAsUnsignedDecimal)
{
    EXPECT_EQ(tpe::cli::formatValue(recordWith(0x1000, {0xC8}), cliType("u8")), "200");
    EXPECT_EQ(tpe::cli::formatValue(recordWith(0x1000, {0xFF}), cliType("u8")), "255");
}

TEST(ValueFormatter, FormatsFloatingPointWithDefaultStreamFormatting)
{
    EXPECT_EQ(tpe::cli::formatValue(recordWith(0x1000, {0x00, 0x00, 0xC0, 0x3F}), cliType("float")), "1.5");
    EXPECT_EQ(tpe::cli::formatValue(
                  recordWith(0x1000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40}), cliType("double")),
              "2.5");
}

TEST(ValueFormatter, FormatsStringAsRawBytes)
{
    EXPECT_EQ(tpe::cli::formatValue(recordWith(0x1000, {'a', 'b', 'c'}), cliType("string")), "abc");
    EXPECT_EQ(tpe::cli::formatValue(recordWith(0x1000, {}), cliType("string")), "");
}

TEST(ValueFormatter, FallsBackToHexBytesOnSnapshotSizeMismatch)
{
    // i32 需要 4 字节,快照只有 2 字节 → 十六进制字节串(内存序,空格分隔)
    EXPECT_EQ(tpe::cli::formatValue(recordWith(0x1000, {0x10, 0x0A}), cliType("i32")), "10 0A");
    // u8 需要 1 字节,快照 4 字节 → 同样回退
    EXPECT_EQ(tpe::cli::formatValue(recordWith(0x1000, {0xC8, 0x00, 0x00, 0x00}), cliType("u8")), "C8 00 00 00");
    // 空快照与非零宽度不匹配 → 空字节串
    EXPECT_EQ(tpe::cli::formatValue(recordWith(0x1000, {}), cliType("i64")), "");
}

// ---------------------------------------------------------------------------
// list 渲染格式(T024;契约 C-R3 / 规范 §11.6)
// ---------------------------------------------------------------------------

TEST(ListRendering, FormatsEntryAsIndentedAddressValuePair)
{
    EXPECT_EQ(tpe::cli::formatListEntry(recordWith(0x1C0A10, {0x64, 0x00, 0x00, 0x00}), cliType("i32")),
              "  0x00000000001C0A10 | 100");
}

TEST(ListRendering, FormatsEntryValueByCurrentType)
{
    // 首扫记录无快照(Phase 05 缺陷 #1 的可见表现):值列回退为空字节串
    EXPECT_EQ(tpe::cli::formatListEntry(recordWith(0x1000, {}), cliType("i32")),
              "  0x0000000000001000 | ");
    // string 值原样
    EXPECT_EQ(tpe::cli::formatListEntry(recordWith(0x1000, {'h', 'i'}), cliType("string")),
              "  0x0000000000001000 | hi");
}

TEST(ListRendering, FormatsMatchesTotalLine)
{
    EXPECT_EQ(tpe::cli::formatMatchesTotal(0), "Total: 0 matches");
    EXPECT_EQ(tpe::cli::formatMatchesTotal(1), "Total: 1 matches");
    EXPECT_EQ(tpe::cli::formatMatchesTotal(15234), "Total: 15234 matches");
}

TEST(ListRendering, FormatsTruncationNotice)
{
    EXPECT_EQ(tpe::cli::formatTruncationNotice(5234), "... and 5234 more");
}

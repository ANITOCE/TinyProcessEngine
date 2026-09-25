#include <gtest/gtest.h>
#include "ValueType.h"

using tpe::Character;
using tpe::Double;
using tpe::Float;
using tpe::Int16;
using tpe::Int32;
using tpe::Int64;
using tpe::String;
using tpe::UnsignedByte;
using tpe::ValueType;

// ============================================================
// UnsignedByte tests
// ============================================================
TEST(UnsignedByteTest, IsValidInRange) {
    UnsignedByte ub;
    EXPECT_TRUE(ub.isValid(0));
    EXPECT_TRUE(ub.isValid(128));
    EXPECT_TRUE(ub.isValid(255));
}

TEST(UnsignedByteTest, IsValidOutOfRange) {
    UnsignedByte ub;
    EXPECT_FALSE(ub.isValid(256));
}

TEST(UnsignedByteTest, RepresentationSingleByte) {
    UnsignedByte ub;
    auto mem = ub.representation(65); // 'A'
    ASSERT_EQ(mem.size(), 1u);
    EXPECT_EQ(mem[0], static_cast<tpe::Byte>(65));
}

// Phase 05 US3(缺陷 ②)/ FR-006 / C-S5:parse 必须产出 1 字节表示
//(修复前:旧 representation 签名不构成 override → 虚派发走基类 uint32 的 4 字节)
TEST(UnsignedByteTest, UnsignedByteParseProducesSingleByte) {
    UnsignedByte ub;
    std::string error;
    const auto parsed = ub.parse("200", error);
    ASSERT_TRUE(parsed.has_value()) << error;
    ASSERT_EQ(parsed->size(), 1u) << "u8 解析产物必须为 1 字节(FR-006)";
    EXPECT_EQ((*parsed)[0], 200);
}

// ============================================================
// Int16 tests
// ============================================================
TEST(Int16Test, ValidValues) {
    Int16 t;
    EXPECT_TRUE(t.isValid(0));
    EXPECT_TRUE(t.isValid(32767));
    EXPECT_TRUE(t.isValid(-32768));
}

TEST(Int16Test, RepresentationSize) {
    Int16 t;
    auto mem = t.representation(std::int16_t(42));
    EXPECT_EQ(mem.size(), sizeof(std::int16_t));
}

// ============================================================
// Int32 tests
// ============================================================
TEST(Int32Test, RepresentationSize) {
    Int32 t;
    auto mem = t.representation(std::int32_t(123456));
    EXPECT_EQ(mem.size(), sizeof(std::int32_t));
}

// ============================================================
// Int64 tests
// ============================================================
TEST(Int64Test, RepresentationSize) {
    Int64 t;
    auto mem = t.representation(std::int64_t(1234567890123LL));
    EXPECT_EQ(mem.size(), sizeof(std::int64_t));
}

// ============================================================
// Float tests
// ============================================================
TEST(FloatTest, RepresentationSize) {
    Float t;
    auto mem = t.representation(3.14159f);
    EXPECT_EQ(mem.size(), sizeof(float));
}

TEST(FloatTest, ValidAnyFloat) {
    Float t;
    EXPECT_TRUE(t.isValid(0.0f));
    EXPECT_TRUE(t.isValid(-1.5f));
}

// ============================================================
// Double tests
// ============================================================
TEST(DoubleTest, RepresentationSize) {
    Double t;
    auto mem = t.representation(3.14159265358979);
    EXPECT_EQ(mem.size(), sizeof(double));
}

// ============================================================
// Character tests
// ============================================================
TEST(CharacterTest, RepresentationSingleChar) {
    Character t;
    auto mem = t.representation('X');
    ASSERT_EQ(mem.size(), 1u);
    EXPECT_EQ(mem[0], static_cast<tpe::Byte>('X'));
}

// ============================================================
// String tests
// ============================================================
TEST(StringTest, RepresentationCopyChars) {
    String t;
    std::string value = "Hello";
    auto mem = t.representation(value);
    ASSERT_EQ(mem.size(), value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        EXPECT_EQ(mem[i], static_cast<tpe::Byte>(value[i]));
    }
}

// ============================================================
// byteWidth tests (T022a)
//   固定宽度类型必须与实际序列化宽度一致;string 为变长(0)。
//   注:u8 的序列化宽度缺陷属 Phase 05;本测试不钉具体值,
//   只钉“byteWidth 与 parse 产物一致”这一不变式。
// ============================================================
TEST(ByteWidthTest, FixedWidthTypesMatchParseOutput)
{
    const UnsignedByte u8;
    const Int16 i16;
    const Int32 i32;
    const Int64 i64;
    const Float f;
    const Double d;

    std::string error;
    for (const ValueType* type : {static_cast<const ValueType*>(&u8),
                                  static_cast<const ValueType*>(&i16),
                                  static_cast<const ValueType*>(&i32),
                                  static_cast<const ValueType*>(&i64),
                                  static_cast<const ValueType*>(&f),
                                  static_cast<const ValueType*>(&d)}) {
        error.clear();
        const auto parsed = type->parse("42", error);
        ASSERT_TRUE(parsed.has_value()) << type->name << ": " << error;
        EXPECT_EQ(type->byteWidth(), parsed->size()) << type->name;
    }
}

TEST(ByteWidthTest, StringIsVariableLength)
{
    const String s;
    EXPECT_EQ(s.byteWidth(), 0u);
}

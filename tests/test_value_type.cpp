#include <gtest/gtest.h>
#include "ValueType.h"

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

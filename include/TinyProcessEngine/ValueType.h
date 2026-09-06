#ifndef _VALUETYPE_H_
#define _VALUETYPE_H_

#include <string>
#include <cstdint>
#include <cassert>
#include <sstream>
#include <memory>
#include <array>
#include <vector>

#include "MemoryPage.h"
#include "HelpFunction.h"

struct ValueType
{
    static const ValueType &choose_type();

    const std::string name;

    ValueType(const std::string &name) : name{name} {}
    virtual tpe::Memory askValue() const = 0;
    virtual ~ValueType() = default;
};



template <class T>
struct SimpleValueType : public ValueType
{
    SimpleValueType(const std::string &name) : ValueType(name) {}

    tpe::Memory askValue() const override;

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

    std::istream &read(std::istream &in, std::string &value) const override
    {
        in.ignore();
        return std::getline(in, value);
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
tpe::Memory SimpleValueType<T>::representation(const T &value) const
{
    auto bytes = reinterpret_cast<const tpe::Byte *>(&value);
    return tpe::Memory(bytes, bytes + sizeof(T));
}

#endif // _VALUETYPE_H_
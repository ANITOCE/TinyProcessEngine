#pragma once

#include <string>
#include <iostream>
#include <functional>

#ifdef _WIN32
#include <Windows.h>
#endif

#define toWinString(str) (LPTSTR) str.c_str()
#define toStdString(str) str
#define HexFormat std::uppercase << std::setw(4) << std::setfill('0')
#define getError(function) \
    if (function == NULL)  \
    PrintError(#function)

namespace tpe {

#ifdef _WIN32
// UTF-8 与 UTF-16 互转（Windows 平台；替代已弃用的 wstring_convert/codecvt，FR-003/C-B2）
inline std::wstring to_wide_string(const std::string &input)
{
    if (input.empty())
    {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, input.c_str(),
                                         static_cast<int>(input.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()),
                        result.data(), size);
    return result;
}

// convert wstring to string
inline std::string to_byte_string(const std::wstring &input)
{
    if (input.empty())
    {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, input.c_str(),
                                         static_cast<int>(input.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}
#endif // _WIN32

// Declarations
template <class T>
using Validator = std::function<bool(const T &)>;

template <class T>
using Reader = std::function<std::istream &(std::istream &, T &)>;

template <class T>
constexpr Validator<T> DefaultValidator()
{
    return [](const T &)
    { return true; };
}

template <class T>
constexpr Reader<T> DefaultReader()
{
    return [](std::istream &in, T &t) -> std::istream &
    { return in >> t; };
}

template <class T>
T ask_for(const std::string &message, const std::string &error,
          Validator<T> validate = DefaultValidator<T>(),
          Reader<T> read = DefaultReader<T>());

// Definitions
template <class T>
T ask_for(const std::string &message, const std::string &error,
          Validator<T> validate, Reader<T> read)
{
    static const char PROMPT = ':';
    T t;
    std::cout << message << PROMPT << " ";
    while (!read(std::cin, t) || !validate(t))
    {
        std::cerr << error << std::endl;
        std::cout << message << PROMPT << " ";

        if (!std::cin)
        {
            std::cin.clear();
            std::cin.ignore(std::cin.rdbuf()->in_avail());
        }
    }

    return t;
}

} // namespace tpe
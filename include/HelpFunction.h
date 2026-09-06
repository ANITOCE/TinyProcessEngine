#ifndef _HELP_FUNCTION_H_
#define _HELP_FUNCTION_H_

#include <string>
#include <codecvt>
#include <locale>
#include <iostream>
#include <functional>

#define toWinString(str) (LPTSTR) str.c_str()
#define toStdString(str) str
#define HexFormat std::uppercase << std::setw(4) << std::setfill('0')
#define getError(function) \
    if (function == NULL)  \
    PrintError(#function)

inline std::wstring to_wide_string(const std::string &input)
{
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(input);
}

// convert wstring to string
inline std::string to_byte_string(const std::wstring &input)
{
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(input);
}

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

#endif // _HELP_FUNCTION_H_
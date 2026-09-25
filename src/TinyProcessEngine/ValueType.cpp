#include "ValueType.hpp"
#include "HelpFunction.hpp"

namespace tpe {

namespace {
    std::vector<std::unique_ptr<ValueType>> createTypes() {
        std::vector<std::unique_ptr<ValueType>> types;
        types.push_back(std::make_unique<UnsignedByte>());
        types.push_back(std::make_unique<Character>());
        types.push_back(std::make_unique<Int16>());
        types.push_back(std::make_unique<Int32>());
        types.push_back(std::make_unique<Int64>());
        types.push_back(std::make_unique<Float>());
        types.push_back(std::make_unique<Double>());
        types.push_back(std::make_unique<String>());
        return types;
    }
}

std::optional<tpe::Memory> ValueType::parse(std::string_view, std::string &error) const
{
    error = "value type '" + name + "' does not support text parsing";
    return std::nullopt;
}

const std::vector<std::unique_ptr<ValueType>> TYPES = createTypes();

const std::size_t TYPES_COUNT = std::distance(std::begin(TYPES), std::end(TYPES));

const ValueType& ValueType::choose_type()
{
    static const char FIRST = 'A';
    std::stringstream query, error;
    for (std::size_t i = 0; i < TYPES_COUNT; ++i)
    {
        query << static_cast<char>(FIRST + i) << ") "
              << TYPES[i]->name << std::endl;
    }
    query << "Value type";
    error << "Choice must be between '" << FIRST << "' and '"
          << static_cast<char>(FIRST + TYPES_COUNT - 1) << "'";

    const char choice = ask_for<char>(query.str(), error.str(), 
                                    [](char c){ return FIRST <= c && c < FIRST + TYPES_COUNT; });

    const int index = choice - FIRST;
    assert(0 <= index && index < TYPES_COUNT);
    return *TYPES[index];
}

} // namespace tpe

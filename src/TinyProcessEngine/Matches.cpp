#include "Matches.hpp"

namespace tpe {

void Matches::add(MemoryPage page, const std::vector<tpe::Offset> &offsets)
{
    if (!offsets.empty())
    {
        matches.emplace_back(page, offsets);
    }
}

std::vector<PageMatches>::size_type Matches::totalMatches() const
{
    return std::accumulate(std::begin(matches), std::end(matches), std::vector<PageMatches>::size_type{},
                           [](std::vector<PageMatches>::size_type acc, const PageMatches &matches) {
                               return acc + matches.getOffsets().size();
                           });
}

} // namespace tpe

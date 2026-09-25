#include "Matches.hpp"

#include <utility>

namespace tpe {

void Matches::add(MemoryPage page, const std::vector<tpe::Offset> &offsets)
{
    // 空 offsets 忽略(外部语义不变;C-E2:判定改经 create 工厂)
    if (std::optional<PageMatches> created = PageMatches::create(page, offsets))
    {
        matches.emplace_back(std::move(*created));
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

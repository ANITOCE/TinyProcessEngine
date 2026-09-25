#ifndef _MATCHES_H_
#define _MATCHES_H_

#include <vector>
#include <numeric>
#include <cstdint>

#include "MemoryPage.h"

namespace tpe {

using Offset = unsigned int;

class no_matches
{
};

/**
 * Represents found matches within a memory page.
 */
class PageMatches
{
    MemoryPage page;
    std::vector<tpe::Offset> offsets;

public:
    /**
     * If no offsets are provided, a no_matches exception is thrown.
     */
    PageMatches(MemoryPage page, const std::vector<tpe::Offset> &offsets)
        : page{page}, offsets{offsets}
    {
        if (offsets.empty())
            throw no_matches{};
    }

    const MemoryPage &getPage() const { return page; }
    const std::vector<tpe::Offset> &getOffsets() const { return offsets; }
};

/**
 * Represents all the memory matches on a process.
 */
class Matches
{
    std::vector<PageMatches> matches;

public:
    const std::vector<PageMatches> &getPageMatches() const { return matches; }

    void add(MemoryPage page, const std::vector<tpe::Offset> &offsets);

    std::vector<PageMatches>::size_type totalMatches() const;

    bool any() const { return totalMatches() > 0; }

};

} // namespace tpe

#endif // _MATCHES_H_
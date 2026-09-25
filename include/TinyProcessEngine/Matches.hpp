#pragma once

#include <vector>
#include <numeric>
#include <cstdint>
#include <optional>

#include "MemoryPage.hpp"

namespace tpe {

using Offset = unsigned int;

/**
 * Represents found matches within a memory page.
 */
class PageMatches
{
    MemoryPage page;
    std::vector<tpe::Offset> offsets;

    PageMatches(MemoryPage page, const std::vector<tpe::Offset> &offsets)
        : page{page}, offsets{offsets}
    {
    }

public:
    /**
     * Creates a PageMatches; empty offsets are rejected via std::nullopt
     * (failure is reported by return value; FR-014/C-E2).
     */
    [[nodiscard]] static std::optional<PageMatches> create(
        MemoryPage page, const std::vector<tpe::Offset> &offsets)
    {
        if (offsets.empty())
            return std::nullopt;
        return PageMatches(page, offsets);
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
#ifndef MEMORY_PAGE_H
#define MEMORY_PAGE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tpe {

using Byte    = unsigned char;
using Address = std::uintptr_t;
using Memory  = std::vector<Byte>;
using Size    = std::size_t;

} // namespace tpe

/**
 * Memory page of the memory space of a process.
 * We read the memory of a process one page at a time.
 */

struct MemoryPage
{
    tpe::Address start;
    tpe::Size    size;

    MemoryPage(tpe::Address start, tpe::Size size) : start{start}, size{size} {}
};

#endif

#include "AobPattern.hpp"

#include <sstream>
#include <cctype>
#include <algorithm>

namespace tpe {

// ============================================================
// AobPattern::parse — tokenize "48 8B ?? 00" style pattern
// ============================================================
std::optional<AobPattern> AobPattern::parse(const std::string& input, std::string* outError) {
    AobPattern result;
    result.original = input;

    std::istringstream iss(input);
    std::string token;
    std::vector<uint8_t> currentSegment;
    size_t byteOffset = 0;
    size_t segmentStart = 0;
    bool inSegment = false;

    while (iss >> token) {
        // Treat single '?' as wildcard too (e.g., "48 8B ?")
        if (token == "??" || token == "?" || token == "?") {
            // End current segment if it has bytes
            if (inSegment && !currentSegment.empty()) {
                AobPattern::Segment seg;
                seg.offset = segmentStart;
                seg.bytes = std::move(currentSegment);
                result.segments.push_back(std::move(seg));
                currentSegment.clear();
                inSegment = false;
            }
            ++byteOffset;
            continue;
        }

        // Parse as hex byte
        if (token.size() > 2) {
            if (outError) *outError = "Invalid hex byte: '" + token + "' (too long)";
            return std::nullopt;
        }

        // Validate hex chars
        for (char c : token) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                if (outError) *outError = "Invalid hex byte: '" + token + "' (non-hex character)";
                return std::nullopt;
            }
        }

        unsigned long byteVal = std::stoul(token, nullptr, 16);
        if (byteVal > 0xFF) {
            if (outError) *outError = "Invalid hex byte: '" + token + "' (out of range)";
            return std::nullopt;
        }

        if (!inSegment) {
            segmentStart = byteOffset;
            inSegment = true;
        }
        currentSegment.push_back(static_cast<uint8_t>(byteVal));
        ++byteOffset;
    }

    // Flush last segment
    if (inSegment && !currentSegment.empty()) {
        AobPattern::Segment seg;
        seg.offset = segmentStart;
        seg.bytes = std::move(currentSegment);
        result.segments.push_back(std::move(seg));
    }

    // Validate: must have at least one deterministic byte
    if (result.segments.empty()) {
        if (outError) *outError = "Pattern must contain at least one known byte";
        return std::nullopt;
    }

    // Find longest segment as anchor
    result.anchor = result.segments[0];
    for (size_t i = 1; i < result.segments.size(); ++i) {
        if (result.segments[i].bytes.size() > result.anchor.bytes.size()) {
            result.anchor = result.segments[i];
        }
    }

    return result;
}

// ============================================================
// AobPattern::totalLength
// ============================================================
size_t AobPattern::totalLength() const {
    if (segments.empty()) return 0;
    // Total length = offset of last segment + its size
    const auto& last = segments.back();
    return last.offset + last.bytes.size();
}

} // namespace tpe

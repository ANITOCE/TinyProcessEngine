#ifndef TPE_AOB_PATTERN_H_
#define TPE_AOB_PATTERN_H_

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

// ============================================================
// AobPattern — Array of Bytes 模式
//
// Example: "48 8B ?? ?? 00 10 ?? 00"
//   - "??" = 通配符，匹配任意单字节
//   - 连续确定性字节组成 Segment
//   - 最长 Segment 作为 BMH 锚点
// ============================================================
struct AobPattern {
    /// A segment of consecutive known (non-wildcard) bytes within the pattern.
    struct Segment {
        size_t   offset;                     // byte offset within full pattern
        std::vector<uint8_t> bytes;          // known byte values
    };

    std::vector<Segment> segments;           // all deterministic segments
    Segment              anchor;             // longest segment (for BMH search)
    std::string          original;           // user's original input string (for error messages)

    /// Parse a user-supplied AOB pattern string.
    /// Format: space-separated hex bytes; "??" or "?" = wildcard.
    /// Returns std::nullopt on parse error with a descriptive error message.
    static std::optional<AobPattern> parse(const std::string& input, std::string* outError = nullptr);

    /// Total byte length of the pattern (including wildcards).
    size_t totalLength() const;

    /// Whether the entire pattern is wildcards (invalid for search).
    bool isAllWildcards() const { return segments.empty(); }
};

#endif // TPE_AOB_PATTERN_H_

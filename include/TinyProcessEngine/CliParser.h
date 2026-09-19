#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tpe::cli {

/// 一行 CLI 文本中的单个令牌。
struct CliToken {
    std::string text;      // 令牌原始文本(保留大小写与内容)
    std::size_t begin = 0; // 相对原始行的起始偏移
    std::size_t end = 0;   // 相对原始行的结束偏移(该位置本身不含)
    bool isFlag = false;   // 以 "--" 开头且长度 > 2
};

/// CliParser::parse 的结果:保留原始行与有序令牌;命令级判定由上层负责。
struct CliParseResult {
    std::string raw;              // 原始行(未改动)
    std::vector<CliToken> tokens; // 有序令牌(按出现顺序)

    /// 旗标名(去掉 "--" 前缀;保序;裸 "--" 不是旗标,不计入)。
    std::vector<std::string> flags() const;

    /// 非旗标令牌(保序;指针指向 tokens 内部元素,生命周期与结果对象一致)。
    std::vector<const CliToken*> positionals() const;

    /// 是否含指定旗标(接受 "i32" 或 "--i32";大小写敏感)。
    bool hasFlag(std::string_view name) const;
};

/// 空白折叠分词器:终端 argv 与 REPL 行共用;纯逻辑,无 I/O,不抛异常。
class CliParser {
public:
    /// 对一行文本分词;空行/全空白行返回 0 个令牌。
    static CliParseResult parse(const std::string& line);
};

} // namespace tpe::cli

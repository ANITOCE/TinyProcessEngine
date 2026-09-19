#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "CliValueType.h"
#include "Platform.h" // Pid_t
#include "ReplState.h"
#include "ScanTypes.h"

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

// ---------------------------------------------------------------------------
// 终端命令分类(spec 004 US1;契约 C-T1–C-T6)
// ---------------------------------------------------------------------------

/// 终端调用(一次性命令)的分类结果。
enum class TerminalCommandKind {
    Help,          // --help 或无参数
    Version,       // --version
    AllProcesses,  // --all-processes
    SearchProcess, // --search-process <PID>
    OpenProcess,   // --open-process <PID>
    UsageError,    // 未知旗标 / 缺 PID / PID 非法 / 多余参数
};

/// parseTerminalCommand 的结果;UsageError 时 error 保存英文原因。
struct TerminalCommand {
    TerminalCommandKind kind = TerminalCommandKind::Help;
    Pid_t pid = 0;
    std::string error;
};

/// 分类终端调用参数(args 不含程序名;纯逻辑,无 I/O,不抛异常)。
/// 空参数等同 --help(C-T5);错误仅以 UsageError + 原因报告(退出码由调用方决定)。
TerminalCommand parseTerminalCommand(const std::vector<std::string>& args);

/// 终端退出码(契约:0 = 成功;1 = 运行期失败;2 = 参数/用法错误)。
inline constexpr int kExitOk = 0;
inline constexpr int kExitRuntimeError = 1;
inline constexpr int kExitUsageError = 2;

/// 解析阶段的退出码映射:UsageError → kExitUsageError,其余 → kExitOk;
/// 运行期失败(kExitRuntimeError)由命令执行层返回。
int parseExitCode(TerminalCommandKind kind);

/// 终端侧帮助文本(覆盖全部终端命令与用法;--help 与参数错误时打印,契约 C-T5/C-T6)。
std::string_view terminalHelpText();

// ---------------------------------------------------------------------------
// REPL 命令解析(spec 004 US2;契约 C-R1–C-R3、C-R5、C-R6)
// ---------------------------------------------------------------------------

/// REPL 命令分类(`write` 于 US3 落地;本阶段按未知命令处理)。
enum class ReplCommandKind {
    Empty,    // 空输入 / 全空白
    Help,     // help
    Exit,     // exit
    NewScan,  // new-scan
    NextScan, // next-scan
    List,     // list
    Undo,     // undo
    Unknown,  // 无法识别(含 write,US3 转正)
};

/// REPL 命令解析结果;error 非空 = 用法错误(含 "Usage"),主循环打印后不执行。
struct ReplCommand {
    ReplCommandKind kind = ReplCommandKind::Unknown;

    // new-scan / next-scan
    ReplScanType scanType = ReplScanType::Equal; // 默认 equal
    const CliValueType* valueType = nullptr;     // 显式 --<type> 旗标;nullptr = 沿用当前
    std::string value;                           // 值原文(--string 取旗标后剩余整行)
    bool hasValue = false;

    // list
    bool listAll = false; // list --all
    unsigned page = 1;    // list 页码(默认第 1 页)

    std::string error; // 非空 = 用法错误(含 "Usage")
};

/// 解析一行 REPL 输入(纯逻辑,无 I/O,不抛异常)。
ReplCommand parseReplCommand(const std::string& line);

/// 一行输入的顶层处置决策(FR-021;I/O 由 startup_cli 执行)。
enum class ReplOutcome {
    Noop,       // 空输入:不打印任何内容,仅刷新提示符
    ShowHelp,   // help / 未知命令:打印 REPL 帮助、不退出
    UsageError, // 用法错误:打印 error、不执行、不退出
    Exit,       // exit:退出 REPL(进程退出码 0)
    Execute,    // 已识别命令:交给主循环执行
};

/// 根据解析结果决定处置(纯逻辑;error 非空时优先 UsageError)。
ReplOutcome planReplOutcome(const ReplCommand& command);

/// REPL 扫描类型 → 引擎扫描条件(next-scan 执行用)。
/// Equal→ExactValue;Greater→Increased;Less→Decreased;Changed/Unchanged 同名;
/// Unknown(仅 new-scan 占位,US4)无对应条件 → nullopt。
std::optional<ScanCondition> toScanCondition(ReplScanType type);

/// REPL 帮助文本(覆盖 7 条命令;契约 C-R6)。
std::string_view replHelpText();

} // namespace tpe::cli

#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "CliParser.h"

using tpe::cli::CliParseResult;
using tpe::cli::CliParser;
using tpe::cli::CliToken;
using tpe::cli::parseTerminalCommand;
using tpe::cli::TerminalCommand;
using tpe::cli::TerminalCommandKind;

// ---------------------------------------------------------------------------
// 令牌化(空白折叠 / 偏移 / 旗标判定)
// ---------------------------------------------------------------------------

TEST(CliParser, EmptyLineYieldsNoTokens)
{
    const CliParseResult result = CliParser::parse("");
    EXPECT_EQ(result.raw, "");
    EXPECT_TRUE(result.tokens.empty());
    EXPECT_TRUE(result.flags().empty());
    EXPECT_TRUE(result.positionals().empty());
}

TEST(CliParser, WhitespaceOnlyLineYieldsNoTokens)
{
    const CliParseResult result = CliParser::parse("  \t \t ");
    EXPECT_TRUE(result.tokens.empty());
    EXPECT_TRUE(result.positionals().empty());
}

TEST(CliParser, FoldsRunsOfWhitespaceIntoTokens)
{
    const CliParseResult result = CliParser::parse("  new-scan \t  --i32   100 ");
    ASSERT_EQ(result.tokens.size(), 3u);
    EXPECT_EQ(result.tokens[0].text, "new-scan");
    EXPECT_EQ(result.tokens[1].text, "--i32");
    EXPECT_EQ(result.tokens[2].text, "100");
}

TEST(CliParser, KeepsRawLineUnchanged)
{
    const std::string line = "  new-scan \t  --i32   100 ";
    const CliParseResult result = CliParser::parse(line);
    EXPECT_EQ(result.raw, line);
}

TEST(CliParser, RecordsOffsetsRelativeToRawLine)
{
    const CliParseResult result = CliParser::parse("new-scan --i32 100");
    ASSERT_EQ(result.tokens.size(), 3u);
    EXPECT_EQ(result.tokens[0].begin, 0u);
    EXPECT_EQ(result.tokens[0].end, 8u);
    EXPECT_EQ(result.tokens[1].begin, 9u);
    EXPECT_EQ(result.tokens[1].end, 14u);
    EXPECT_EQ(result.tokens[2].begin, 15u);
    EXPECT_EQ(result.tokens[2].end, 18u);
    // 依据偏移可截取“该令牌之后的剩余整行”(--string 取值规则的基础)
    EXPECT_EQ(result.raw.substr(result.tokens[0].end), " --i32 100");
}

TEST(CliParser, ClassifiesTokensStartingWithDoubleDashAndLongerThanTwo)
{
    const CliParseResult result = CliParser::parse("--i32 100 -x -- --string");
    ASSERT_EQ(result.tokens.size(), 5u);
    EXPECT_TRUE(result.tokens[0].isFlag);  // "--i32"
    EXPECT_FALSE(result.tokens[1].isFlag); // "100"
    EXPECT_FALSE(result.tokens[2].isFlag); // "-x"(单横线不是旗标)
    EXPECT_FALSE(result.tokens[3].isFlag); // "--"(长度不足,不是旗标)
    EXPECT_TRUE(result.tokens[4].isFlag);  // "--string"
}

TEST(CliParser, TreatsQuotesAsOrdinaryCharacters)
{
    // 本项目不做引号分组:--string 取值依赖原始行偏移,而非引号解析
    const CliParseResult result = CliParser::parse("write 0x10 \"a b\"");
    ASSERT_EQ(result.tokens.size(), 4u);
    EXPECT_EQ(result.tokens[2].text, "\"a");
    EXPECT_EQ(result.tokens[3].text, "b\"");
}

// ---------------------------------------------------------------------------
// 便捷访问(flags / positionals / hasFlag)
// ---------------------------------------------------------------------------

TEST(CliParserFlags, ListsFlagNamesWithoutPrefixInOrder)
{
    const CliParseResult result = CliParser::parse("new-scan --i64 --string hello");
    const std::vector<std::string> flags = result.flags();
    ASSERT_EQ(flags.size(), 2u);
    EXPECT_EQ(flags[0], "i64");
    EXPECT_EQ(flags[1], "string");
}

TEST(CliParserFlags, HasFlagAcceptsPlainAndPrefixedNames)
{
    const CliParseResult result = CliParser::parse("next-scan --equal 100");
    EXPECT_TRUE(result.hasFlag("equal"));
    EXPECT_TRUE(result.hasFlag("--equal"));
    EXPECT_FALSE(result.hasFlag("changed"));
}

TEST(CliParserPositionals, ReturnsNonFlagTokensInOrder)
{
    const CliParseResult result = CliParser::parse("write 1C0A10 42");
    const std::vector<const CliToken*> positionals = result.positionals();
    ASSERT_EQ(positionals.size(), 3u);
    EXPECT_EQ(positionals[0]->text, "write");
    EXPECT_EQ(positionals[1]->text, "1C0A10");
    EXPECT_EQ(positionals[2]->text, "42");
    EXPECT_EQ(positionals[1]->begin, 6u);
}

TEST(CliParserPositionals, KeepsBareDoubleDashAsPositional)
{
    const CliParseResult result = CliParser::parse("-- 100");
    EXPECT_TRUE(result.flags().empty());
    ASSERT_EQ(result.positionals().size(), 2u);
    EXPECT_EQ(result.positionals()[0]->text, "--");
}

// ---------------------------------------------------------------------------
// 终端命令分类(契约 C-T1–C-T6 / FR-001–FR-006)
// ---------------------------------------------------------------------------

TEST(TerminalCommandParse, NoArgumentsBehavesLikeHelp)
{
    const TerminalCommand none = parseTerminalCommand({});
    const TerminalCommand help = parseTerminalCommand({"--help"});
    EXPECT_EQ(none.kind, TerminalCommandKind::Help);
    EXPECT_EQ(none.kind, help.kind); // 无参数 ≡ --help(C-T5)
    EXPECT_TRUE(none.error.empty());
}

TEST(TerminalCommandParse, RecognizesVersionFlag)
{
    const TerminalCommand command = parseTerminalCommand({"--version"});
    EXPECT_EQ(command.kind, TerminalCommandKind::Version);
    EXPECT_TRUE(command.error.empty());
}

TEST(TerminalCommandParse, RecognizesAllProcessesFlag)
{
    const TerminalCommand command = parseTerminalCommand({"--all-processes"});
    EXPECT_EQ(command.kind, TerminalCommandKind::AllProcesses);
}

TEST(TerminalCommandParse, ParsesSearchProcessWithPid)
{
    const TerminalCommand command = parseTerminalCommand({"--search-process", "1234"});
    ASSERT_EQ(command.kind, TerminalCommandKind::SearchProcess);
    EXPECT_EQ(command.pid, static_cast<Pid_t>(1234));
}

TEST(TerminalCommandParse, ParsesOpenProcessWithPid)
{
    const TerminalCommand command = parseTerminalCommand({"--open-process", "1234"});
    ASSERT_EQ(command.kind, TerminalCommandKind::OpenProcess);
    EXPECT_EQ(command.pid, static_cast<Pid_t>(1234));
}

TEST(TerminalCommandParse, AcceptsPidAtTypeMaximum)
{
    const unsigned long long maxPid = (std::numeric_limits<Pid_t>::max)();
    const TerminalCommand command =
        parseTerminalCommand({"--search-process", std::to_string(maxPid)});
    ASSERT_EQ(command.kind, TerminalCommandKind::SearchProcess);
    EXPECT_EQ(static_cast<unsigned long long>(command.pid), maxPid);
}

TEST(TerminalCommandParse, AcceptsDecimalPidWithLeadingZeros)
{
    const TerminalCommand command = parseTerminalCommand({"--open-process", "000123"});
    ASSERT_EQ(command.kind, TerminalCommandKind::OpenProcess);
    EXPECT_EQ(command.pid, static_cast<Pid_t>(123));
}

TEST(TerminalCommandParse, RejectsUnknownFlag)
{
    const TerminalCommand command = parseTerminalCommand({"--no-such"});
    ASSERT_EQ(command.kind, TerminalCommandKind::UsageError);
    EXPECT_FALSE(command.error.empty());
    EXPECT_NE(command.error.find("--no-such"), std::string::npos);
}

TEST(TerminalCommandParse, RejectsMixedFlags)
{
    const TerminalCommand command = parseTerminalCommand({"--help", "--version"});
    ASSERT_EQ(command.kind, TerminalCommandKind::UsageError);
    EXPECT_FALSE(command.error.empty());
}

TEST(TerminalCommandParse, RejectsUnexpectedArgumentAfterKnownFlag)
{
    EXPECT_EQ(parseTerminalCommand({"--all-processes", "extra"}).kind,
              TerminalCommandKind::UsageError);
    EXPECT_EQ(parseTerminalCommand({"--version", "extra"}).kind,
              TerminalCommandKind::UsageError);
    EXPECT_EQ(parseTerminalCommand({"--help", "extra"}).kind,
              TerminalCommandKind::UsageError);
}

TEST(TerminalCommandParse, RejectsMissingPidForSearchProcess)
{
    const TerminalCommand command = parseTerminalCommand({"--search-process"});
    ASSERT_EQ(command.kind, TerminalCommandKind::UsageError);
    EXPECT_NE(command.error.find("--search-process"), std::string::npos);
}

TEST(TerminalCommandParse, RejectsMissingPidForOpenProcess)
{
    const TerminalCommand command = parseTerminalCommand({"--open-process"});
    ASSERT_EQ(command.kind, TerminalCommandKind::UsageError);
    EXPECT_NE(command.error.find("--open-process"), std::string::npos);
}

TEST(TerminalCommandParse, RejectsNonNumericPid)
{
    const TerminalCommand command = parseTerminalCommand({"--search-process", "abc"});
    ASSERT_EQ(command.kind, TerminalCommandKind::UsageError);
    EXPECT_NE(command.error.find("abc"), std::string::npos);
}

TEST(TerminalCommandParse, RejectsHexSignedAndSpacedPid)
{
    EXPECT_EQ(parseTerminalCommand({"--open-process", "0x10"}).kind,
              TerminalCommandKind::UsageError);
    EXPECT_EQ(parseTerminalCommand({"--open-process", "-1"}).kind,
              TerminalCommandKind::UsageError);
    EXPECT_EQ(parseTerminalCommand({"--open-process", "12 34"}).kind,
              TerminalCommandKind::UsageError);
}

TEST(TerminalCommandParse, RejectsPidAboveTypeRange)
{
    const std::string tooLarge = std::to_string(
        static_cast<unsigned long long>((std::numeric_limits<Pid_t>::max)()) + 1);
    const TerminalCommand command = parseTerminalCommand({"--search-process", tooLarge});
    ASSERT_EQ(command.kind, TerminalCommandKind::UsageError);
    EXPECT_FALSE(command.error.empty());
}

TEST(TerminalCommandParse, RejectsExtraArgumentsAfterPid)
{
    const TerminalCommand command = parseTerminalCommand({"--search-process", "1", "2"});
    ASSERT_EQ(command.kind, TerminalCommandKind::UsageError);
    EXPECT_FALSE(command.error.empty());
}

TEST(TerminalCommandParse, RejectsUnknownBareArguments)
{
    // 裸 “--” 不做特殊解析,与其它未知参数同等对待
    EXPECT_EQ(parseTerminalCommand({"--"}).kind, TerminalCommandKind::UsageError);
    EXPECT_EQ(parseTerminalCommand({"foo"}).kind, TerminalCommandKind::UsageError);
    EXPECT_EQ(parseTerminalCommand({""}).kind, TerminalCommandKind::UsageError);
}

// ---------------------------------------------------------------------------
// 终端退出码映射(契约:0 = 成功;1 = 运行期失败;2 = 参数/用法错误)
// ---------------------------------------------------------------------------

TEST(TerminalExitCodes, ConstantsMatchContract)
{
    EXPECT_EQ(tpe::cli::kExitOk, 0);
    EXPECT_EQ(tpe::cli::kExitRuntimeError, 1);
    EXPECT_EQ(tpe::cli::kExitUsageError, 2);
}

TEST(TerminalExitCodes, UsageErrorMapsToExitCodeTwo)
{
    const TerminalCommand command = parseTerminalCommand({"--no-such"});
    ASSERT_EQ(command.kind, TerminalCommandKind::UsageError);
    EXPECT_EQ(tpe::cli::parseExitCode(command.kind), tpe::cli::kExitUsageError);
}

TEST(TerminalExitCodes, OtherKindsMapToExitCodeZero)
{
    EXPECT_EQ(tpe::cli::parseExitCode(TerminalCommandKind::Help), tpe::cli::kExitOk);
    EXPECT_EQ(tpe::cli::parseExitCode(TerminalCommandKind::Version), tpe::cli::kExitOk);
    EXPECT_EQ(tpe::cli::parseExitCode(TerminalCommandKind::AllProcesses), tpe::cli::kExitOk);
    EXPECT_EQ(tpe::cli::parseExitCode(TerminalCommandKind::SearchProcess), tpe::cli::kExitOk);
    EXPECT_EQ(tpe::cli::parseExitCode(TerminalCommandKind::OpenProcess), tpe::cli::kExitOk);
}

// ---------------------------------------------------------------------------
// REPL 命令解析(T020;契约 C-R1–C-R3、C-R5、C-R6)
// ---------------------------------------------------------------------------

using tpe::cli::parseReplCommand;
using tpe::cli::ReplCommand;
using tpe::cli::ReplCommandKind;
using tpe::cli::ReplScanType;

TEST(ReplCommandParse, ClassifiesEachCommandKind)
{
    EXPECT_EQ(parseReplCommand("new-scan 100").kind, ReplCommandKind::NewScan);
    EXPECT_EQ(parseReplCommand("next-scan 100").kind, ReplCommandKind::NextScan);
    EXPECT_EQ(parseReplCommand("list").kind, ReplCommandKind::List);
    EXPECT_EQ(parseReplCommand("write 0x10 42").kind, ReplCommandKind::Write);
    EXPECT_EQ(parseReplCommand("undo").kind, ReplCommandKind::Undo);
    EXPECT_EQ(parseReplCommand("help").kind, ReplCommandKind::Help);
    EXPECT_EQ(parseReplCommand("exit").kind, ReplCommandKind::Exit);
}

TEST(ReplCommandParse, EmptyAndWhitespaceInputAreEmpty)
{
    EXPECT_EQ(parseReplCommand("").kind, ReplCommandKind::Empty);
    EXPECT_EQ(parseReplCommand("   \t ").kind, ReplCommandKind::Empty);
    EXPECT_TRUE(parseReplCommand("").error.empty());
}

TEST(ReplCommandParse, UnknownCommandsAreUnrecognized)
{
    EXPECT_EQ(parseReplCommand("bogus").kind, ReplCommandKind::Unknown);
    EXPECT_EQ(parseReplCommand("--all").kind, ReplCommandKind::Unknown);
    EXPECT_EQ(parseReplCommand("NEW-SCAN 100").kind, ReplCommandKind::Unknown); // 大小写敏感
}

TEST(ReplCommandParse, NewScanDefaultsToEqualAndCurrentType)
{
    const ReplCommand command = parseReplCommand("new-scan 100");
    ASSERT_EQ(command.kind, ReplCommandKind::NewScan);
    EXPECT_EQ(command.scanType, ReplScanType::Equal);
    EXPECT_EQ(command.valueType, nullptr); // 沿用当前数值类型
    EXPECT_TRUE(command.hasValue);
    EXPECT_EQ(command.value, "100");
    EXPECT_TRUE(command.error.empty());
}

TEST(ReplCommandParse, NewScanAcceptsExplicitEqualFlag)
{
    const ReplCommand command = parseReplCommand("new-scan --equal --i32 100");
    ASSERT_EQ(command.kind, ReplCommandKind::NewScan);
    EXPECT_EQ(command.scanType, ReplScanType::Equal);
    ASSERT_NE(command.valueType, nullptr);
    EXPECT_EQ(command.valueType->shortName, "i32");
    EXPECT_TRUE(command.hasValue);
    EXPECT_EQ(command.value, "100");
    EXPECT_TRUE(command.error.empty());
}

TEST(ReplCommandParse, NewScanAcceptsValueTypeFlagOnly)
{
    const ReplCommand command = parseReplCommand("new-scan --i64 100");
    ASSERT_EQ(command.kind, ReplCommandKind::NewScan);
    EXPECT_EQ(command.scanType, ReplScanType::Equal);
    ASSERT_NE(command.valueType, nullptr);
    EXPECT_EQ(command.valueType->shortName, "i64");
    EXPECT_EQ(command.value, "100");
}

TEST(ReplCommandParse, NewScanStringTakesRemainderOfLine)
{
    const ReplCommand command = parseReplCommand("new-scan --string hello world");
    ASSERT_EQ(command.kind, ReplCommandKind::NewScan);
    ASSERT_NE(command.valueType, nullptr);
    EXPECT_EQ(command.valueType->shortName, "string");
    EXPECT_TRUE(command.hasValue);
    EXPECT_EQ(command.value, "hello world");

    // 旗标后多个空白:跳过分隔空白,保留值内部空白
    const ReplCommand spaced = parseReplCommand("new-scan --string   hello  world");
    EXPECT_EQ(spaced.value, "hello  world");

    const ReplCommand switched = parseReplCommand("next-scan --string a b");
    ASSERT_EQ(switched.kind, ReplCommandKind::NextScan);
    EXPECT_EQ(switched.value, "a b");
}

TEST(ReplCommandParse, NewScanRequiresValue)
{
    EXPECT_FALSE(parseReplCommand("new-scan").error.empty());
    EXPECT_FALSE(parseReplCommand("new-scan --equal").error.empty());
    EXPECT_FALSE(parseReplCommand("new-scan --string").error.empty());
    EXPECT_FALSE(parseReplCommand("new-scan --string    ").error.empty());
    EXPECT_NE(parseReplCommand("new-scan").error.find("Usage"), std::string::npos);
}

TEST(ReplCommandParse, NewScanRejectsExtraPositionalArguments)
{
    EXPECT_FALSE(parseReplCommand("new-scan 100 200").error.empty());
}

TEST(ReplCommandParse, NewScanRejectsConflictingOrUnknownFlags)
{
    EXPECT_FALSE(parseReplCommand("new-scan --i16 --i32 100").error.empty());
    EXPECT_FALSE(parseReplCommand("new-scan --no-such 100").error.empty());
    EXPECT_FALSE(parseReplCommand("new-scan --changed").error.empty()); // new-scan 旗标表无此项
}

TEST(ReplCommandParse, NewScanPlaceholderFlagsDeferredToUs4)
{
    // 占位功能属 US4(C-R7 / T030–T034):US2 阶段以“未实现”错误拒绝,
    // US4 将改为:解析通过 + 占位提示 + 仅切换 scan-type(本测试届时替换)。
    EXPECT_FALSE(parseReplCommand("new-scan --unknown").error.empty());
    EXPECT_FALSE(parseReplCommand("new-scan --greater").error.empty());
    EXPECT_FALSE(parseReplCommand("new-scan --less").error.empty());
}

TEST(ReplCommandParse, NextScanDefaultsToEqualWithValue)
{
    const ReplCommand command = parseReplCommand("next-scan 100");
    ASSERT_EQ(command.kind, ReplCommandKind::NextScan);
    EXPECT_EQ(command.scanType, ReplScanType::Equal);
    EXPECT_EQ(command.valueType, nullptr);
    EXPECT_TRUE(command.hasValue);
    EXPECT_EQ(command.value, "100");
    EXPECT_TRUE(command.error.empty());
}

TEST(ReplCommandParse, NextScanAcceptsValuelessConditions)
{
    const ReplCommand greater = parseReplCommand("next-scan --greater");
    ASSERT_EQ(greater.kind, ReplCommandKind::NextScan);
    EXPECT_EQ(greater.scanType, ReplScanType::Greater);
    EXPECT_FALSE(greater.hasValue);
    EXPECT_TRUE(greater.error.empty());

    const ReplCommand less = parseReplCommand("next-scan --less --i64");
    EXPECT_EQ(less.scanType, ReplScanType::Less);
    ASSERT_NE(less.valueType, nullptr);
    EXPECT_EQ(less.valueType->shortName, "i64");
    EXPECT_FALSE(less.hasValue);
    EXPECT_TRUE(less.error.empty());

    const ReplCommand changed = parseReplCommand("next-scan --changed");
    EXPECT_EQ(changed.scanType, ReplScanType::Changed);
    EXPECT_FALSE(changed.hasValue);
    EXPECT_TRUE(changed.error.empty());

    const ReplCommand unchanged = parseReplCommand("next-scan --unchanged");
    EXPECT_EQ(unchanged.scanType, ReplScanType::Unchanged);
    EXPECT_TRUE(unchanged.error.empty());
}

TEST(ReplCommandParse, NextScanRejectsUnknownCondition)
{
    // FR-013:next-scan MUST NOT 接受 --unknown
    const ReplCommand command = parseReplCommand("next-scan --unknown");
    ASSERT_EQ(command.kind, ReplCommandKind::NextScan);
    EXPECT_FALSE(command.error.empty());
    EXPECT_NE(command.error.find("--unknown"), std::string::npos);
}

TEST(ReplCommandParse, NextScanValuelessConditionsRejectExtraArguments)
{
    // 严格策略:不传值条件出现多余位置参数 → 用法错误(不执行、状态不变)
    EXPECT_FALSE(parseReplCommand("next-scan --changed 100").error.empty());
    EXPECT_FALSE(parseReplCommand("next-scan --greater extra").error.empty());
    EXPECT_FALSE(parseReplCommand("next-scan --less 42").error.empty());
    EXPECT_FALSE(parseReplCommand("next-scan --unchanged x").error.empty());
}

TEST(ReplCommandParse, NextScanRequiresValueForEqualForm)
{
    EXPECT_FALSE(parseReplCommand("next-scan").error.empty());
    EXPECT_FALSE(parseReplCommand("next-scan --equal").error.empty());
    EXPECT_NE(parseReplCommand("next-scan").error.find("Usage"), std::string::npos);
}

TEST(ReplCommandParse, NextScanRejectsConflictingConditions)
{
    EXPECT_FALSE(parseReplCommand("next-scan --greater --less").error.empty());
    EXPECT_FALSE(parseReplCommand("next-scan --changed --equal 5").error.empty());
}

TEST(ReplCommandParse, ListDefaultsToFirstPage)
{
    const ReplCommand command = parseReplCommand("list");
    ASSERT_EQ(command.kind, ReplCommandKind::List);
    EXPECT_FALSE(command.listAll);
    EXPECT_EQ(command.page, 1u);
    EXPECT_TRUE(command.error.empty());
}

TEST(ReplCommandParse, ListAllFlag)
{
    const ReplCommand command = parseReplCommand("list --all");
    ASSERT_EQ(command.kind, ReplCommandKind::List);
    EXPECT_TRUE(command.listAll);
    EXPECT_TRUE(command.error.empty());
}

TEST(ReplCommandParse, ListAcceptsPositivePageNumber)
{
    const ReplCommand command = parseReplCommand("list 3");
    ASSERT_EQ(command.kind, ReplCommandKind::List);
    EXPECT_EQ(command.page, 3u);
    EXPECT_TRUE(command.error.empty());
}

TEST(ReplCommandParse, ListRejectsInvalidPageNumbers)
{
    EXPECT_FALSE(parseReplCommand("list 0").error.empty());
    EXPECT_FALSE(parseReplCommand("list abc").error.empty());
    EXPECT_FALSE(parseReplCommand("list -1").error.empty());
    EXPECT_FALSE(parseReplCommand("list 0x10").error.empty());
    EXPECT_FALSE(parseReplCommand("list 99999999999999999999").error.empty()); // 超出页码范围
}

TEST(ReplCommandParse, ListRejectsCombinedOrExtraArguments)
{
    EXPECT_FALSE(parseReplCommand("list --all 2").error.empty());
    EXPECT_FALSE(parseReplCommand("list 1 --all").error.empty());
    EXPECT_FALSE(parseReplCommand("list 1 2").error.empty());
    EXPECT_FALSE(parseReplCommand("list --no-such").error.empty());
}

TEST(ReplCommandParse, UndoRejectsArguments)
{
    const ReplCommand command = parseReplCommand("undo");
    ASSERT_EQ(command.kind, ReplCommandKind::Undo);
    EXPECT_TRUE(command.error.empty());
    EXPECT_FALSE(parseReplCommand("undo x").error.empty());
    EXPECT_FALSE(parseReplCommand("undo --all").error.empty());
}

TEST(ReplCommandParse, HelpIgnoresArguments)
{
    // §11.4:help 在“无参数或参数错误”时默认调用 → 一律显示帮助,不报错
    EXPECT_EQ(parseReplCommand("help").kind, ReplCommandKind::Help);
    const ReplCommand withArgs = parseReplCommand("help me");
    EXPECT_EQ(withArgs.kind, ReplCommandKind::Help);
    EXPECT_TRUE(withArgs.error.empty());
}

TEST(ReplCommandParse, ExitRejectsArguments)
{
    const ReplCommand command = parseReplCommand("exit");
    ASSERT_EQ(command.kind, ReplCommandKind::Exit);
    EXPECT_TRUE(command.error.empty());
    EXPECT_FALSE(parseReplCommand("exit now").error.empty());
}

TEST(ReplCommandParse, UsageErrorsIncludeUsageHint)
{
    EXPECT_NE(parseReplCommand("new-scan").error.find("Usage"), std::string::npos);
    EXPECT_NE(parseReplCommand("next-scan --changed 1").error.find("Usage"), std::string::npos);
    EXPECT_NE(parseReplCommand("list 0").error.find("Usage"), std::string::npos);
}

// ---------------------------------------------------------------------------
// write 命令解析(US3 T026;契约 C-R4)
// ---------------------------------------------------------------------------

TEST(ReplCommandParse, WriteParsesHexAddressAndValue)
{
    const ReplCommand command = parseReplCommand("write 0x1C0A10 42");
    ASSERT_EQ(command.kind, ReplCommandKind::Write);
    EXPECT_EQ(command.address, static_cast<tpe::Address>(0x1C0A10));
    EXPECT_TRUE(command.hasValue);
    EXPECT_EQ(command.value, "42");
    EXPECT_TRUE(command.error.empty());
}

TEST(ReplCommandParse, WriteAddressHexPrefixIsOptional)
{
    // `0x` 前缀可省;hex 数字与前缀大小写不敏感
    EXPECT_EQ(parseReplCommand("write 1C0A10 42").address,
              static_cast<tpe::Address>(0x1C0A10));
    EXPECT_EQ(parseReplCommand("write 0x1c0a10 42").address,
              static_cast<tpe::Address>(0x1C0A10));
    EXPECT_EQ(parseReplCommand("write 0X1C0A10 42").address,
              static_cast<tpe::Address>(0x1C0A10));
    EXPECT_EQ(parseReplCommand("write 0 42").address, static_cast<tpe::Address>(0));
}

TEST(ReplCommandParse, WriteKeepsRemainderOfLineAsValue)
{
    // 地址后剩余整行(内部空格保留);string 目标的取值规则见 extractWriteValueText
    const ReplCommand command = parseReplCommand("write 0x10 hello  world");
    ASSERT_EQ(command.kind, ReplCommandKind::Write);
    EXPECT_TRUE(command.hasValue);
    EXPECT_EQ(command.value, "hello  world");
    EXPECT_TRUE(command.error.empty());
}

TEST(ReplCommandParse, WriteRequiresAddressAndValue)
{
    const ReplCommand missingAddress = parseReplCommand("write");
    EXPECT_FALSE(missingAddress.error.empty());
    EXPECT_NE(missingAddress.error.find("Usage"), std::string::npos);

    const ReplCommand missingValue = parseReplCommand("write 0x10");
    EXPECT_FALSE(missingValue.error.empty());
    EXPECT_NE(missingValue.error.find("Usage"), std::string::npos);

    const ReplCommand blankValue = parseReplCommand("write 0x10   ");
    EXPECT_FALSE(blankValue.error.empty());
    EXPECT_NE(blankValue.error.find("Usage"), std::string::npos);
}

TEST(ReplCommandParse, WriteRejectsInvalidAddresses)
{
    for (const char* line : {"write 0x 42", "write 0xZZ10 42", "write -1 42", "write 41G0 42",
                             "write FFFFFFFFFFFFFFFFFF 42"}) {
        const ReplCommand command = parseReplCommand(line);
        EXPECT_FALSE(command.error.empty()) << line;
        EXPECT_NE(command.error.find("Usage"), std::string::npos) << line;
    }
}

TEST(ReplCommandParse, WriteRejectsOptions)
{
    // write 语法无旗标:值类型由 REPL 当前 `value-type` 决定(规范 §11.7)
    const ReplCommand command = parseReplCommand("write --i32 0x10 42");
    EXPECT_FALSE(command.error.empty());
    EXPECT_NE(command.error.find("--i32"), std::string::npos);
}

TEST(ReplCommandParse, WriteValueRulesFollowTargetType)
{
    using tpe::cli::CliValueType;
    using tpe::cli::defaultCliValueType;
    using tpe::cli::extractWriteValueText;
    using tpe::cli::findCliValueTypeByShortName;

    std::string error;
    const CliValueType& i32 = defaultCliValueType();
    const CliValueType* stringType = findCliValueTypeByShortName("string");
    ASSERT_NE(stringType, nullptr);

    // 非 string:单 token 通过
    const ReplCommand single = parseReplCommand("write 0x10 -2");
    ASSERT_EQ(single.kind, ReplCommandKind::Write);
    const std::optional<std::string> singleText = extractWriteValueText(single, i32, error);
    ASSERT_TRUE(singleText.has_value()) << error;
    EXPECT_EQ(*singleText, "-2");

    // 非 string:多余 token → 用法错误(不执行)
    const ReplCommand extra = parseReplCommand("write 0x10 42 43");
    error.clear();
    EXPECT_FALSE(extractWriteValueText(extra, i32, error).has_value());
    EXPECT_NE(error.find("43"), std::string::npos) << error;

    // string:地址后剩余整行(允许空格)
    const ReplCommand text = parseReplCommand("write 0x10 hello world");
    error.clear();
    const std::optional<std::string> stringText = extractWriteValueText(text, *stringType, error);
    ASSERT_TRUE(stringText.has_value()) << error;
    EXPECT_EQ(*stringText, "hello world");
}

TEST(ReplCommandParse, WritePlansToExecute)
{
    EXPECT_EQ(tpe::cli::planReplOutcome(parseReplCommand("write 0x10 42")),
              tpe::cli::ReplOutcome::Execute);
    EXPECT_EQ(tpe::cli::planReplOutcome(parseReplCommand("write 0x10")),
              tpe::cli::ReplOutcome::UsageError);
    EXPECT_EQ(tpe::cli::planReplOutcome(parseReplCommand("write")),
              tpe::cli::ReplOutcome::UsageError);
}

TEST(ReplHelp, CoversAllSevenCommands)
{
    const std::string help(tpe::cli::replHelpText());
    for (const char* name : {"help", "exit", "new-scan", "next-scan", "list", "write", "undo"}) {
        EXPECT_NE(help.find(name), std::string::npos) << name;
    }
}

TEST(ReplScanConditionMap, MapsEachConditionToEngineValue)
{
    using tpe::cli::toScanCondition;
    EXPECT_EQ(toScanCondition(ReplScanType::Equal), ScanCondition::ExactValue);
    EXPECT_EQ(toScanCondition(ReplScanType::Greater), ScanCondition::Increased);
    EXPECT_EQ(toScanCondition(ReplScanType::Less), ScanCondition::Decreased);
    EXPECT_EQ(toScanCondition(ReplScanType::Changed), ScanCondition::Changed);
    EXPECT_EQ(toScanCondition(ReplScanType::Unchanged), ScanCondition::Unchanged);
    EXPECT_FALSE(toScanCondition(ReplScanType::Unknown).has_value());
}
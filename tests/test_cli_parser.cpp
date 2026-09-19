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
#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "CliParser.h"
#include "CliValueType.h"
#include "ReplState.h"

using tpe::cli::CliValueType;
using tpe::cli::ReplCommand;
using tpe::cli::ReplCommandKind;
using tpe::cli::ReplOutcome;
using tpe::cli::ReplScanType;
using tpe::cli::ReplState;

namespace {

const CliValueType& cliType(std::string_view name)
{
    const CliValueType* entry = tpe::cli::findCliValueTypeByFlag(name);
    EXPECT_NE(entry, nullptr) << name;
    return *entry;
}

} // namespace

// ---------------------------------------------------------------------------
// 提示符四段(data-model §1;契约 C-R1 提示符表 / FR-007)
// ---------------------------------------------------------------------------

TEST(ReplPrompt, DefaultsToEqualI32WithoutValue)
{
    const ReplState state("test.exe");
    EXPECT_EQ(state.prompt(), "test.exe-equal-i32> ");
    EXPECT_EQ(state.scanType, ReplScanType::Equal);
    EXPECT_EQ(state.valueType, &cliType("i32"));
    EXPECT_FALSE(state.lastValue.has_value());
    EXPECT_EQ(state.matchesTotal, 0u);
}

TEST(ReplPrompt, ShowsValueAfterValueScan)
{
    ReplState state("test.exe");
    state.onValueScan(ReplScanType::Equal, cliType("i32"), "100", 15234);
    EXPECT_EQ(state.prompt(), "test.exe-equal-i32-100> ");
    EXPECT_EQ(state.matchesTotal, 15234u);
    EXPECT_EQ(state.lastValue, std::optional<std::string>("100"));
}

TEST(ReplPrompt, HidesValueAfterChangedAndUnchanged)
{
    ReplState state("test.exe");
    state.onValueScan(ReplScanType::Equal, cliType("i32"), "100", 5);
    state.onValuelessScan(ReplScanType::Changed, cliType("i32"), 2);
    EXPECT_EQ(state.prompt(), "test.exe-changed-i32> ");
    EXPECT_FALSE(state.lastValue.has_value());
    EXPECT_EQ(state.matchesTotal, 2u);

    state.onValueScan(ReplScanType::Equal, cliType("i32"), "7", 3);
    state.onValuelessScan(ReplScanType::Unchanged, cliType("i32"), 1);
    EXPECT_EQ(state.prompt(), "test.exe-unchanged-i32> ");
    EXPECT_FALSE(state.lastValue.has_value());
}

TEST(ReplPrompt, KeepsValueAfterGreaterOrLess)
{
    ReplState state("test.exe");
    state.onValueScan(ReplScanType::Equal, cliType("i32"), "100", 5);
    state.onValuelessScan(ReplScanType::Greater, cliType("i32"), 4);
    EXPECT_EQ(state.prompt(), "test.exe-greater-i32-100> ");
    EXPECT_TRUE(state.lastValue.has_value());

    state.onValuelessScan(ReplScanType::Less, cliType("i64"), 3);
    EXPECT_EQ(state.prompt(), "test.exe-less-i64-100> ");
    EXPECT_EQ(state.valueType, &cliType("i64"));
}

TEST(ReplPrompt, NewValueScanReplacesDisplayedValue)
{
    ReplState state("test.exe");
    state.onValueScan(ReplScanType::Equal, cliType("i32"), "100", 5);
    state.onValueScan(ReplScanType::Equal, cliType("i32"), "250", 8);
    EXPECT_EQ(state.prompt(), "test.exe-equal-i32-250> ");
}

// ---------------------------------------------------------------------------
// 状态迁移(data-model §4:T2–T6)
// ---------------------------------------------------------------------------

TEST(ReplStateMigrate, ValueScanUpdatesScanTypeValueTypeAndTotal)
{
    ReplState state("test.exe");
    state.onValueScan(ReplScanType::Equal, cliType("i64"), "102", 42);
    EXPECT_EQ(state.scanType, ReplScanType::Equal);
    EXPECT_EQ(state.valueType, &cliType("i64"));
    EXPECT_EQ(state.lastValue, std::optional<std::string>("102"));
    EXPECT_EQ(state.matchesTotal, 42u);
}

TEST(ReplStateMigrate, ValueTypeSwitchPersistsAcrossCommands)
{
    ReplState state("test.exe");
    state.setValueType(cliType("u8"));
    EXPECT_EQ(state.prompt(), "test.exe-equal-u8> ");
    state.onValuelessScan(ReplScanType::Greater, cliType("u8"), 9);
    EXPECT_EQ(state.prompt(), "test.exe-greater-u8> ");
}

TEST(ReplStateMigrate, PlaceholderSwitchesScanTypeOnly)
{
    // T4:占位命令仅切换 scanType;匹配集与 [<value>] 均不变
    ReplState state("test.exe");
    state.onValueScan(ReplScanType::Equal, cliType("i32"), "100", 15234);
    state.onPlaceholder(ReplScanType::Unknown, nullptr);
    EXPECT_EQ(state.scanType, ReplScanType::Unknown);
    EXPECT_EQ(state.prompt(), "test.exe-unknown-i32-100> ");
    EXPECT_EQ(state.matchesTotal, 15234u);
    EXPECT_TRUE(state.lastValue.has_value());
}

TEST(ReplStateMigrate, PlaceholderWithValueTypeFlag)
{
    // 语法可识别:new-scan --unknown --i16 组合
    ReplState state("test.exe");
    state.onPlaceholder(ReplScanType::Greater, &cliType("i16"));
    EXPECT_EQ(state.prompt(), "test.exe-greater-i16> ");
}

TEST(ReplStateMigrate, UndoRestoresTotalWithoutTouchingPrompt)
{
    // T6:matchesTotal 回退;提示符其余不变(含 lastValue 与 scanType)
    ReplState state("test.exe");
    state.onValueScan(ReplScanType::Equal, cliType("i32"), "100", 30);
    state.onValuelessScan(ReplScanType::Greater, cliType("i32"), 10);
    EXPECT_EQ(state.matchesTotal, 10u);

    state.onUndo(30);
    EXPECT_EQ(state.matchesTotal, 30u);
    EXPECT_EQ(state.prompt(), "test.exe-greater-i32-100> ");
}

TEST(ReplStateMigrate, UndoKeepsClearedValueForChangedScan)
{
    ReplState state("test.exe");
    state.onValueScan(ReplScanType::Equal, cliType("i32"), "100", 30);
    state.onValuelessScan(ReplScanType::Changed, cliType("i32"), 5);
    state.onUndo(30);
    EXPECT_EQ(state.prompt(), "test.exe-changed-i32> ");
}

// ---------------------------------------------------------------------------
// list 分页(契约 C-R3;越界判定需要 matchesTotal 上下文)
// ---------------------------------------------------------------------------

TEST(ReplPages, CountsPagesFromTotal)
{
    ReplState state("test.exe");
    EXPECT_EQ(state.pageCount(), 0u);
    state.matchesTotal = 1;
    EXPECT_EQ(state.pageCount(), 1u);
    state.matchesTotal = 20;
    EXPECT_EQ(state.pageCount(), 1u);
    state.matchesTotal = 21;
    EXPECT_EQ(state.pageCount(), 2u);
    state.matchesTotal = 40;
    EXPECT_EQ(state.pageCount(), 2u);
    state.matchesTotal = 41;
    EXPECT_EQ(state.pageCount(), 3u);
}

TEST(ReplPages, ValidatesPageRange)
{
    ReplState state("test.exe");
    state.matchesTotal = 25;
    EXPECT_TRUE(state.isPageInRange(1));
    EXPECT_TRUE(state.isPageInRange(2));
    EXPECT_FALSE(state.isPageInRange(0));
    EXPECT_FALSE(state.isPageInRange(3));

    state.matchesTotal = 0;
    EXPECT_FALSE(state.isPageInRange(1)); // 空结果:任何页都无效
}

// ---------------------------------------------------------------------------
// 一行输入的处置决策(T021;契约 C-R6 / FR-021)
// ---------------------------------------------------------------------------

TEST(ReplOutcomePlan, EmptyCommandIsNoop)
{
    ReplCommand command;
    command.kind = ReplCommandKind::Empty;
    EXPECT_EQ(tpe::cli::planReplOutcome(command), ReplOutcome::Noop);
}

TEST(ReplOutcomePlan, UnknownCommandShowsHelp)
{
    ReplCommand command;
    command.kind = ReplCommandKind::Unknown;
    EXPECT_EQ(tpe::cli::planReplOutcome(command), ReplOutcome::ShowHelp);
}

TEST(ReplOutcomePlan, HelpShowsHelp)
{
    ReplCommand command;
    command.kind = ReplCommandKind::Help;
    EXPECT_EQ(tpe::cli::planReplOutcome(command), ReplOutcome::ShowHelp);
}

TEST(ReplOutcomePlan, ExitExits)
{
    ReplCommand command;
    command.kind = ReplCommandKind::Exit;
    EXPECT_EQ(tpe::cli::planReplOutcome(command), ReplOutcome::Exit);
}

TEST(ReplOutcomePlan, UsageErrorTakesPrecedence)
{
    ReplCommand command;
    command.kind = ReplCommandKind::NewScan;
    command.error = "Missing value for new-scan.\nUsage: new-scan <value>";
    EXPECT_EQ(tpe::cli::planReplOutcome(command), ReplOutcome::UsageError);
}

TEST(ReplOutcomePlan, RecognizedCommandsExecute)
{
    for (const ReplCommandKind kind : {ReplCommandKind::NewScan, ReplCommandKind::NextScan,
                                       ReplCommandKind::List, ReplCommandKind::Undo}) {
        ReplCommand command;
        command.kind = kind;
        EXPECT_EQ(tpe::cli::planReplOutcome(command), ReplOutcome::Execute);
    }
}

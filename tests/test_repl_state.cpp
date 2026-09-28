#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "CliParser.hpp"
#include "CliValueType.hpp"
#include "ReplState.hpp"

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

TEST(ReplStateMigrate, UnknownScanClearsDisplayedValue)
{
    // T005 红 / C-D1 / FR-004:new-scan --unknown 成功后值段隐藏(lastValue 清空)——
    // §11.3:new-scan 销毁旧进度,该次无值 → 不显示值段。
    ReplState state("test.exe");
    state.onValueScan(ReplScanType::Equal, cliType("i32"), "100", 15234);

    state.onValuelessScan(ReplScanType::Unknown, cliType("i32"), 4096);

    EXPECT_EQ(state.scanType, ReplScanType::Unknown);
    EXPECT_EQ(state.matchesTotal, 4096u);
    EXPECT_FALSE(state.lastValue.has_value()) << "unknown scan must clear the displayed value";
    EXPECT_EQ(state.prompt(), "test.exe-unknown-i32> ");
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

// ---------------------------------------------------------------------------
// 占位命令(US4 T030;FR-020 / C-R7 / SC-006)
// ---------------------------------------------------------------------------

TEST(ReplPlaceholder, FixedMessageIsContractText)
{
    // §11.8 / FR-020:占位提示逐字固定
    EXPECT_EQ(tpe::cli::replPlaceholderText(), "This feature is not implemented yet.");
}

TEST(ReplPlaceholder, PlansExecuteOutcomeForThreeScanTypes)
{
    // T020/FR-019:三旗标均为真实语义 → 解析通过(无用法错误)、处置决策恒为 Execute
    // (执行真实首扫;无占位分支)。
    for (const char* line : {"new-scan --unknown", "new-scan --unknown --i16",
                             "new-scan --greater 100", "new-scan --greater --i64 42",
                             "new-scan --less 100"}) {
        const ReplCommand command = tpe::cli::parseReplCommand(line);
        EXPECT_TRUE(command.error.empty()) << line;
        EXPECT_EQ(tpe::cli::planReplOutcome(command), ReplOutcome::Execute) << line;
    }
}

TEST(ReplPlaceholder, SimulatesMainLoopExecutionOutcome)
{
    // T020/FR-019 + C-D10:执行层成功后主循环调用的纯状态迁移(本用例模拟):
    // --unknown 无值 → 值段隐藏(lastValue 清空);--greater / --less 带值 → 显示本次比较值。
    ReplState state("test.exe");
    state.onValueScan(ReplScanType::Equal, cliType("i32"), "100", 15234);

    {
        const ReplCommand command = tpe::cli::parseReplCommand("new-scan --unknown");
        ASSERT_EQ(tpe::cli::planReplOutcome(command), ReplOutcome::Execute);
        const CliValueType& vt =
            command.valueType != nullptr ? *command.valueType : *state.valueType;
        state.onValuelessScan(command.scanType, vt, 47);
        EXPECT_EQ(state.prompt(), "test.exe-unknown-i32> ");
        EXPECT_FALSE(state.lastValue.has_value());
        EXPECT_EQ(state.matchesTotal, 47u);
    }

    const struct {
        const char* line;
        const char* prompt;
        const char* value;
    } cases[] = {
        {"new-scan --greater 250 --i64", "test.exe-greater-i64-250> ", "250"},
        {"new-scan --less --i32 5", "test.exe-less-i32-5> ", "5"},
    };
    for (const auto& item : cases) {
        const ReplCommand command = tpe::cli::parseReplCommand(item.line);
        ASSERT_EQ(tpe::cli::planReplOutcome(command), ReplOutcome::Execute) << item.line;
        const CliValueType& vt =
            command.valueType != nullptr ? *command.valueType : *state.valueType;
        state.onValueScan(command.scanType, vt, command.value, 7);
        EXPECT_EQ(state.prompt(), item.prompt) << item.line;
        EXPECT_EQ(state.matchesTotal, 7u) << item.line;
        EXPECT_EQ(state.lastValue, std::optional<std::string>(item.value)) << item.line;
    }
}

TEST(ReplPlaceholder, MissingValueIsUsageErrorAndLeavesStateUntouched)
{
    // §11.5:new-scan 的 --greater / --less 值必传;缺值 → 用法错误、不执行、不切换
    ReplState state("test.exe");
    state.onValueScan(ReplScanType::Equal, cliType("i32"), "100", 7);

    for (const char* line : {"new-scan --greater", "new-scan --less"}) {
        const ReplCommand command = tpe::cli::parseReplCommand(line);
        ASSERT_EQ(tpe::cli::planReplOutcome(command), ReplOutcome::UsageError) << line;
        EXPECT_NE(command.error.find("Usage"), std::string::npos) << line;
        EXPECT_NE(command.error.find("Missing value"), std::string::npos) << line;
        EXPECT_FALSE(command.placeholder) << line;
        // 主循环对 UsageError 只打印、不触碰状态
        EXPECT_EQ(state.prompt(), "test.exe-equal-i32-100> ") << line;
        EXPECT_EQ(state.matchesTotal, 7u) << line;
    }
}

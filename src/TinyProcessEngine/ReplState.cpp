#include "ReplState.h"

namespace tpe::cli {

// 红阶段 stub(T019/T021):绿阶段实现提示符渲染与状态迁移。

std::string_view scanTypeName(ReplScanType type)
{
    switch (type) {
    case ReplScanType::Equal:
        return "equal";
    case ReplScanType::Unknown:
        return "unknown";
    case ReplScanType::Greater:
        return "greater";
    case ReplScanType::Less:
        return "less";
    case ReplScanType::Changed:
        return "changed";
    case ReplScanType::Unchanged:
        return "unchanged";
    }
    return "equal";
}

ReplState::ReplState()
    : valueType(&defaultCliValueType())
{
}

ReplState::ReplState(std::string name)
    : processName(std::move(name)), valueType(&defaultCliValueType())
{
}

std::string ReplState::prompt() const
{
    // <process-name>-<scan-type>-<value-type> + (-<value> 仅带值扫描后存在) + "> "
    // 注:规范写法的 `[<value>]` 中方括号为“可选段”记号,不是字面字符(§11.3 示例)。
    std::string out = processName;
    out += '-';
    out += scanTypeName(scanType);
    out += '-';
    out += valueType != nullptr ? std::string(valueType->shortName) : std::string();
    if (lastValue.has_value()) {
        out += '-';
        out += *lastValue;
    }
    out += "> ";
    return out;
}

void ReplState::setValueType(const CliValueType& type)
{
    valueType = &type;
}

void ReplState::onValueScan(ReplScanType type, const CliValueType& vt, std::string value, uint64_t total)
{
    scanType = type;
    valueType = &vt;
    lastValue = std::move(value);
    matchesTotal = total;
}

void ReplState::onValuelessScan(ReplScanType type, const CliValueType& vt, uint64_t total)
{
    scanType = type;
    valueType = &vt;
    // `--changed` / `--unchanged` 清空 [<value>];`--greater` / `--less` 保持最近一次带值扫描的值
    if (type == ReplScanType::Changed || type == ReplScanType::Unchanged) {
        lastValue.reset();
    }
    matchesTotal = total;
}

void ReplState::onPlaceholder(ReplScanType type, const CliValueType* vt)
{
    // 占位命令:仅切换 scanType(可选 valueType);匹配集与 [<value>] 均不变
    scanType = type;
    if (vt != nullptr) {
        valueType = vt;
    }
}

void ReplState::onUndo(uint64_t total)
{
    // undo 仅回退匹配集总量;提示符其余段不变
    matchesTotal = total;
}

unsigned ReplState::pageCount() const
{
    if (matchesTotal == 0) {
        return 0;
    }
    return static_cast<unsigned>((matchesTotal + kReplPageSize - 1) / kReplPageSize);
}

bool ReplState::isPageInRange(unsigned page) const
{
    return page >= 1 && page <= pageCount();
}

} // namespace tpe::cli

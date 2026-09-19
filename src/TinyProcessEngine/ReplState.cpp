#include "ReplState.h"

namespace tpe::cli {

// 红阶段 stub(T019/T021):绿阶段实现提示符渲染与状态迁移。

std::string_view scanTypeName(ReplScanType)
{
    return {};
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
    return {};
}

void ReplState::setValueType(const CliValueType&)
{
}

void ReplState::onValueScan(ReplScanType, const CliValueType&, std::string, uint64_t)
{
}

void ReplState::onValuelessScan(ReplScanType, const CliValueType&, uint64_t)
{
}

void ReplState::onPlaceholder(ReplScanType, const CliValueType*)
{
}

void ReplState::onUndo(uint64_t)
{
}

unsigned ReplState::pageCount() const
{
    return 0;
}

bool ReplState::isPageInRange(unsigned) const
{
    return false;
}

} // namespace tpe::cli

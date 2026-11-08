#include "RemoteCommandExecutor.hpp"

namespace tpe::remote {

// 骨架实现：仅提供可编译的占位，命令分类与执行行为由 T017 实现。

RemoteCommandKind classifyRemoteCommand(std::string_view line)
{
    (void)line;
    return RemoteCommandKind::Unknown;
}

RemoteCommandResult executeRemoteCommand(std::string_view line, tpe::ProcessEngine& engine)
{
    (void)line;
    (void)engine;

    RemoteCommandResult result;
    result.status = ResponseStatus::CommandError;
    return result;
}

} // namespace tpe::remote

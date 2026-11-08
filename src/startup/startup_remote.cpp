#include "startup_remote.hpp"

namespace tpe::app {

namespace {

/// 默认远程入口:骨架阶段直接失败。
/// T018 补参数解析,T022/T023 补 Server/Client 网络逻辑。
int defaultRemoteEntry(const std::vector<std::string>& args)
{
    (void)args;
    return 1;
}

/// 当前生效的远程入口;测试构建可经 setRemoteEntryForTesting 替换。
RemoteEntryFn& remoteEntry()
{
    static RemoteEntryFn entry = &defaultRemoteEntry;
    return entry;
}

} // namespace

Result<RemoteStartupOptions, std::string>
parseRemoteStartupArgs(const std::vector<std::string>& args)
{
    (void)args;
    return Result<RemoteStartupOptions, std::string>::error("not implemented");
}

int runRemote(const std::vector<std::string>& args)
{
    return remoteEntry()(args);
}

void setRemoteEntryForTesting(RemoteEntryFn entry)
{
    remoteEntry() = (entry != nullptr) ? entry : &defaultRemoteEntry;
}

} // namespace tpe::app

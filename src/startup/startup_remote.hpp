#pragma once

#include "Result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tpe::app {

// ============================================================
// 远程启动
// ============================================================

/// 远程角色:Server 监听 / Client 连接;两者互斥。
enum class RemoteMode { Server, Client };

/// 远程启动参数。
/// - Server:`--server [-port <PORT>]`,省略端口时使用 8008;
/// - Client:`--connect <IP> -port <PORT>`,地址须为 IPv4 字面量。
struct RemoteStartupOptions {
    RemoteMode mode = RemoteMode::Server;
    std::uint16_t port = 0; ///< 1–65535,由解析层保证
    std::string host;       ///< 仅 Client 使用
};

/// 解析远程启动参数(args 不含程序名)。
/// 成功返回已校验的选项;失败返回一行原因,调用方按用法错误处理(退出码 2)。
/// 骨架阶段恒返回错误 "not implemented"(T018 实现)。
Result<RemoteStartupOptions, std::string>
parseRemoteStartupArgs(const std::vector<std::string>& args);

/// 远程模式入口:按参数启动 Server 或 Client,返回进程退出码(0 成功,1 失败)。
/// startup.cpp 识别到 `--server`/`--connect` 后调用(T024 接入);
/// 骨架阶段经默认入口返回 1。
int runRemote(const std::vector<std::string>& args);

/// 可注入远程入口(T012 测试构建):以测试替身断言 startup.cpp 的分流,
/// 不实际监听或连接;生产构建使用默认入口。
using RemoteEntryFn = int (*)(const std::vector<std::string>& args);

/// 替换 runRemote 使用的入口;传 nullptr 恢复默认实现(返回 1)。
void setRemoteEntryForTesting(RemoteEntryFn entry);

} // namespace tpe::app

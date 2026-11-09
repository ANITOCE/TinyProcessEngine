#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace tpe {

class ProcessEngine;

} // namespace tpe

namespace tpe::remote {

// ============================================================
// Server 监听与单会话（contracts/protocol.md §3–§7；data-model.md §3）
// ============================================================

/// 远程 Server（骨架：监听、状态机与日志由 T022/T029/T035 实现）。
///
/// 生命周期：
/// - `start()` 绑定并监听指定端口，开始接纳连接，失败时返回 false；
/// - `isRunning()` 报告是否处于监听状态；
/// - `stop()` 停止监听并关闭会话，可重复调用。
///
/// 实现细节（libhv 事件循环、会话状态机与日志）由 Pimpl 隐藏；
/// 头文件不包含 libhv，依赖不泄漏到调用方。
class RemoteServer
{
public:
    /// @param port   监听端口，取值 1–65535
    /// @param engine 命令执行使用的进程引擎
    RemoteServer(std::uint16_t port, tpe::ProcessEngine& engine);
    ~RemoteServer();

    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;

    /// 开始监听；绑定失败或端口被占用时返回 false。
    bool start();

    /// 停止监听并释放会话；未启动或已停止时无副作用。
    void stop();

    /// 是否处于监听状态。
    bool isRunning() const;

    /// 测试接缝：覆盖握手期限（仅存储，不改变握手语义）。
    /// 默认值保持契约（5000 ms，contracts/protocol.md §4）；T022 从 Impl 读取。
    void setHandshakeTimeoutForTesting(std::chrono::milliseconds timeout);

    /// 测试接缝：覆盖 Draining 清理延迟（仅存储，不改变清理语义）。
    /// 默认值保持契约行为（10 ms，contracts/protocol.md §3/§6），仅用于在测试中
    /// 拉长“会话已结束、清理未完成”的观测窗口；须在 `start()` 之前调用。
    void setDrainingCleanupDelayForTesting(std::chrono::milliseconds delay);

private:
    struct Impl; // 定义在 .cpp：libhv 事件循环与会话状态
    std::unique_ptr<Impl> m_impl;
};

} // namespace tpe::remote

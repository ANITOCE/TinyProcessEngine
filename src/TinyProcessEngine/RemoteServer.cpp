#include "RemoteServer.hpp"

namespace tpe::remote {

// 骨架实现：仅提供可编译的占位；监听、状态机与日志由 T022/T029/T035 实现。
// Impl 暂不持有状态，后续任务在此补充 libhv 事件循环与会话数据。
struct RemoteServer::Impl {
    // T022 使用：握手期限；默认值来自契约（5 秒），测试可注入缩短值。
    std::chrono::milliseconds handshakeTimeout{5000};
};

RemoteServer::RemoteServer(std::uint16_t port, tpe::ProcessEngine& engine)
    : m_impl(std::make_unique<Impl>())
{
    (void)port;
    (void)engine;
}

RemoteServer::~RemoteServer() = default;

bool RemoteServer::start()
{
    return false;
}

void RemoteServer::stop()
{
}

bool RemoteServer::isRunning() const
{
    return false;
}

void RemoteServer::setHandshakeTimeoutForTesting(std::chrono::milliseconds timeout)
{
    m_impl->handshakeTimeout = timeout;
}

} // namespace tpe::remote

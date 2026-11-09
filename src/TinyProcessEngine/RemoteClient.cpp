#include "RemoteClient.hpp"

namespace tpe::remote {

// 骨架实现：仅提供可编译的占位；连接、握手与 TPE> 循环由 T023/T028/T036 实现。
// Impl 暂不持有状态，后续任务在此补充连接、帧收发与期限判定数据。
struct RemoteClient::Impl {
    // T023 使用：可注入的帧接收期限；默认值来自契约（30 秒）。
    ClientTimeouts timeouts{};
};

RemoteClient::RemoteClient(std::string host, std::uint16_t port)
    : m_impl(std::make_unique<Impl>())
{
    (void)host;
    (void)port;
}

RemoteClient::~RemoteClient() = default;

int RemoteClient::run()
{
    return 1;
}

void RemoteClient::setTimeoutsForTesting(const ClientTimeouts& timeouts)
{
    m_impl->timeouts = timeouts;
}

} // namespace tpe::remote

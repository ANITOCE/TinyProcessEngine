#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace tpe::remote {

// ============================================================
// Client 连接与会话（contracts/protocol.md §3–§7；contracts/cli.md §1–§2/§7）
// ============================================================

/// Client 侧帧接收期限（contracts/protocol.md §4）：
/// - `firstByte`：连接建立后等待“任何帧字节”的静默期限；
/// - `frameComplete`：收到首字节后等待“完整帧”的期限。
/// 默认值即契约值（30 秒）。测试通过 `setTimeoutsForTesting` 注入毫秒级缩短值，
/// 用短期限验证 30 秒规则；T023 在收发路径上使用该字段。
struct ClientTimeouts {
    std::chrono::milliseconds firstByte{30000};
    std::chrono::milliseconds frameComplete{30000};
};

/// 远程 Client（骨架：连接、握手与 TPE> 循环由 T023/T028/T036 实现）。
///
/// `run()` 连接 Server，完成 HELLO/WELCOME 握手后进入远程 `TPE>`，
/// 返回进程退出码：0 正常结束，1 连接失败、断线或协议错误。
///
/// 实现细节（libhv TCP 客户端、帧收发与期限判定）由 Pimpl 隐藏；
/// 头文件不包含 libhv，依赖不泄漏到调用方。
class RemoteClient
{
public:
    /// @param host 目标地址，IPv4 字面量
    /// @param port 目标端口，取值 1–65535
    RemoteClient(std::string host, std::uint16_t port);
    ~RemoteClient();

    RemoteClient(const RemoteClient&) = delete;
    RemoteClient& operator=(const RemoteClient&) = delete;

    /// 运行会话直至结束；返回进程退出码。
    int run();

    /// 测试接缝：覆盖帧接收期限（仅存储，不改变握手语义）。
    /// 默认值保持契约（30 秒）；T023 的收发路径从 Impl 读取。
    void setTimeoutsForTesting(const ClientTimeouts& timeouts);

private:
    struct Impl; // 定义在 .cpp：TCP 连接、帧收发与 TPE> 状态
    std::unique_ptr<Impl> m_impl;
};

} // namespace tpe::remote

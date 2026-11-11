#include "RemoteClient.hpp"

#include "RemoteProtocol.hpp"

#include <hv/TcpClient.h>
#include <hv/hlog.h>

#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tpe::remote {
namespace {

// ============================================================
// 固定文本（contracts/cli.md §2/§7；contracts/protocol.md §4/§7）
// ============================================================

/// 握手/连接失败统一前缀；其后必须跟随具体原因（cli.md §2）。
constexpr std::string_view kConnectionFailedText = "Connection failed.";

/// Client 被接纳后写入标准输出的固定文本（cli.md §2）。
constexpr std::string_view kSuccessText = "Connection successful.";

/// 远程提示符；线上 prompt 不含尾随空格，渲染层添加（cli.md §5）。
constexpr std::string_view kPromptText = "TPE> ";

/// HELLO 的固定 clientId（UTF-8，≤64 字节；契约 §4 示例 "TPE"）。
constexpr std::string_view kClientId = "TPE";

/// BUSY 的固定载荷文本（protocol.md §2）。
constexpr std::string_view kBusyText = "Server is busy.";

/// 首字节静默期限到期（protocol.md §4；cli.md §7）。
constexpr std::string_view kNoResponseText = "No response from server.";

/// 首字节后完整帧期限到期（protocol.md §4；cli.md §7）。
constexpr std::string_view kIncompleteFrameText = "Server sent an incomplete message.";

/// 握手期收到 WELCOME/BUSY/ERROR 之外的完整帧（protocol.md §4）。
constexpr std::string_view kUnexpectedMessageText = "Server sent an unexpected message.";

/// 三种握手答复的载荷不符合契约（protocol.md §4）。
constexpr std::string_view kInvalidMessageText = "Server sent an invalid message.";

/// 活动期收到载荷非空的 BYE（protocol.md §7）。
constexpr std::string_view kInvalidSessionEndText = "Server sent an invalid session end.";

/// BYE 确认期限（1 秒）到期且连接仍在（protocol.md §7）。
constexpr std::string_view kEndNotAcknowledgedText =
    "Server did not acknowledge the session end.";

/// 未收到握手答复/会话确认前连接被对端关闭。
constexpr std::string_view kClosedByServerText = "The server closed the connection.";

/// ERROR 载荷的 reason 为空时的兜底原因（保证 "Connection failed." 后有非空原因）。
constexpr std::string_view kRejectedText = "The server rejected the connection.";

/// HELLO 写回失败（连接建立后立即断开）。
constexpr std::string_view kSendHandshakeFailedText = "Failed to send the handshake.";

/// 发送前校验失败文本（cli.md §3.1）；一律写标准输出、不发送、保留 TPE>。
constexpr std::string_view kUnencodableCommandText = "Command cannot be encoded as UTF-8.";
constexpr std::string_view kLineBreakCommandText = "Command must not contain line breaks.";
constexpr std::string_view kOversizeCommandText =
    "Command exceeds the maximum message size.";

/// 文本 → 载荷字节序列。
std::vector<std::uint8_t> toPayload(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

bool isContinuationByte(std::uint8_t byte)
{
    return (byte & 0xC0u) == 0x80u;
}

/// 严格 UTF-8 校验（与 RemoteProtocol 的载荷校验同一规则，不依赖 locale）：
/// 拒绝过长编码、UTF-16 代理区码点、超出 U+10FFFF 的序列与截断序列。
bool isValidUtf8(std::string_view text)
{
    std::size_t index = 0;
    const std::size_t size = text.size();
    while (index < size) {
        const auto lead = static_cast<std::uint8_t>(text[index]);
        if (lead <= 0x7Fu) { // ASCII
            ++index;
            continue;
        }

        std::size_t trailing = 0;      // 后续字节数
        std::uint8_t secondMin = 0x80; // 第 2 字节允许范围
        std::uint8_t secondMax = 0xBF;
        if (lead >= 0xC2u && lead <= 0xDFu) {
            trailing = 1;
        } else if (lead == 0xE0u) {
            trailing = 2;
            secondMin = 0xA0; // 拒绝过长编码
        } else if (lead >= 0xE1u && lead <= 0xECu) {
            trailing = 2;
        } else if (lead == 0xEDu) {
            trailing = 2;
            secondMax = 0x9F; // 拒绝 UTF-16 代理区 U+D800..U+DFFF
        } else if (lead >= 0xEEu && lead <= 0xEFu) {
            trailing = 2;
        } else if (lead == 0xF0u) {
            trailing = 3;
            secondMin = 0x90; // 拒绝过长编码
        } else if (lead >= 0xF1u && lead <= 0xF3u) {
            trailing = 3;
        } else if (lead == 0xF4u) {
            trailing = 3;
            secondMax = 0x8F; // 拒绝超过 U+10FFFF
        } else {
            return false; // 非法首字节（含 0x80..0xC1、0xF5..0xFF）
        }

        if (trailing > size - index - 1) {
            return false; // 序列被截断
        }
        const auto second = static_cast<std::uint8_t>(text[index + 1]);
        if (second < secondMin || second > secondMax) {
            return false;
        }
        for (std::size_t offset = 2; offset <= trailing; ++offset) {
            if (!isContinuationByte(static_cast<std::uint8_t>(text[index + offset]))) {
                return false;
            }
        }
        index += trailing + 1;
    }
    return true;
}

/// 发送前校验（cli.md §3.1）：顺序固定为 UTF-8 → 去除行尾后是否仍含 CR/LF
/// → 编码后字节数 ≤ 64 KiB。通过返回空视图，失败返回应写标准输出的固定文本。
std::string_view validateCommandLine(const std::string& line)
{
    if (!isValidUtf8(line)) {
        return kUnencodableCommandText;
    }
    if (line.find('\r') != std::string::npos || line.find('\n') != std::string::npos) {
        return kLineBreakCommandText;
    }
    if (line.size() > kMaxCommandBytes) {
        return kOversizeCommandText;
    }
    return {};
}

/// BYE 确认期限（契约固定 1 秒）。
constexpr std::chrono::seconds kSessionEndTimeout{1};

/// 静默 libhv 的默认日志器（每进程恰好一次）。
/// 与 Server 侧同样的理由（contracts/logging.md §4）：libhv 默认 logger 未设
/// handler 时会回落到工作目录的 `libhv.YYYYMMDD.log`，并可能向控制台输出。
/// `LOG_LEVEL_SILENT` 使 `logger_print` 在写文件之前即返回。
void silenceLibhvFileLogger()
{
    static std::once_flag once;
    std::call_once(once, [] { hlog_set_level(LOG_LEVEL_SILENT); });
}

/// 契约期限（毫秒）→ libhv 定时器毫秒值；至少 1 ms，避免 0 被当成无期限。
int toTimerMilliseconds(std::chrono::milliseconds value)
{
    const auto count = value.count();
    if (count < 1) {
        return 1;
    }
    constexpr auto kMax = static_cast<long long>(0x7FFFFFFF);
    if (count > kMax) {
        return static_cast<int>(kMax);
    }
    return static_cast<int>(count);
}

/// 握手失败输出：`Connection failed.` + 非空原因，写标准错误（cli.md §2）。
void printHandshakeFailure(const std::string& reason)
{
    std::cerr << kConnectionFailedText << ' '
              << (reason.empty() ? std::string(kRejectedText) : reason) << std::endl;
}

/// 包装层的“未连接”错误不是实际 connect 失败原因：
/// Linux 的 `getpeername` 失败会留下 `ENOTCONN`，Windows 对应
/// `WSAENOTCONN`（并可能经 C 运行库 `ENOTCONN` 暴露）。
bool isWrapperNotConnectedError(int error)
{
#ifdef _WIN32
    return error == ENOTCONN || error == WSAENOTCONN;
#else
    return error == ENOTCONN;
#endif
}

/// 在 close 回调内读取 `SO_ERROR`；libhv 在回调之后才 closesocket，
/// 因此 `Channel::fd()` 仍有效。失败或无待处理错误时返回 0。
int queryPendingSocketError(int fd)
{
    int error = 0;
#ifdef _WIN32
    int length = sizeof(error);
    if (::getsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&error), &length) == 0) {
        return error;
    }
#else
    socklen_t length = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0) {
        return error;
    }
#endif
    return 0;
}

/// 连接未建立时的实际原因：`SO_ERROR` 优先；仅在其不可得时回退
/// `Channel::error()`，并排除包装层 `ENOTCONN`/`WSAENOTCONN`。
/// Windows 上 libhv 建连计时器写入 C 运行库 `ETIMEDOUT`（MSVC 138），
/// 不是 Winsock `WSAETIMEDOUT`（10060），需先转换再渲染。
std::string connectionFailureReason(const hv::SocketChannelPtr& channel)
{
    if (!channel) {
        return {};
    }

    int error = 0;
    const int fd = channel->fd();
    if (fd >= 0) {
        error = queryPendingSocketError(fd);
    }
    if (isWrapperNotConnectedError(error)) {
        error = 0;
    }
    if (error == 0) {
        error = channel->error();
#ifdef _WIN32
        if (error == ETIMEDOUT) {
            error = WSAETIMEDOUT;
        }
#endif
        if (error <= 0 || isWrapperNotConnectedError(error)) {
            return {};
        }
    }

    const char* text = socket_strerror(error);
    return text != nullptr ? std::string(text) : std::string{};
}

} // namespace

// ============================================================
// Impl：libhv TCP 客户端、握手状态机与最小 TPE> 循环
// ============================================================

struct RemoteClient::Impl {
    /// 会话阶段：事件循环线程推进；Ending 由 run() 线程在发送 BYE 前设置。
    enum class Phase {
        Connecting,  // 等待 TCP 连接建立
        Handshaking, // 已发送 HELLO，等待 WELCOME/BUSY/ERROR
        Active,      // 已接纳，TPE> 交互
        Ending,      // 已发送 BYE，等待 Server 确认
        Finished,    // 握手失败或会话已终结
    };

    Impl(std::string targetHost, std::uint16_t targetPort)
        : host(std::move(targetHost))
        , port(targetPort)
    {
    }

    ~Impl()
    {
        // 显式 join 事件循环线程：定时器回调与消息回调在本函数返回前全部结束。
        stopClient();
    }

    // ---- 配置（run() 之前设置） ----
    std::string host;
    std::uint16_t port = 0;
    ClientTimeouts timeouts{};

    // ---- 跨线程结果（run() 线程与事件循环线程，受 mutex 保护） ----
    std::mutex mutex;
    std::condition_variable cv;
    Phase phase = Phase::Connecting;
    bool handshakeDone = false; // 握手结果已确定（成功或失败）
    bool handshakeOk = false;
    std::string failureText; // 握手失败原因
    bool byeAcknowledged = false;
    bool peerClosed = false;
    bool sessionFailed = false; // 活动期协议错误
    std::string sessionFailureText;
    bool awaitingResponse = false;                // run() 已发送 COMMAND，等待 RESPONSE
    std::optional<RemoteResponse> pendingResponse; // 已送达、尚未被 run() 消费的 RESPONSE

    // ---- 事件循环线程私有状态 ----
    bool firstByteSeen = false;
    std::vector<std::uint8_t> input; // 未消费的帧字节
    std::chrono::steady_clock::time_point firstByteDeadline{};
    std::chrono::steady_clock::time_point frameDeadline{};
    hv::TimerID firstByteTimer = INVALID_TIMER_ID;
    hv::TimerID frameTimer = INVALID_TIMER_ID;

    // ---- libhv 客户端；声明在最后，保证先于 mutex/cv 析构（回调会用到它们） ----
    std::unique_ptr<hv::TcpClient> client;

    // --------------------------------------------------------
    // 公开入口
    // --------------------------------------------------------

    int runSession()
    {
        // 每次运行都从干净状态开始（防御性，测试中每个实例只调用一次）。
        {
            const std::lock_guard<std::mutex> guard(mutex);
            phase = Phase::Connecting;
            handshakeDone = false;
            handshakeOk = false;
            failureText.clear();
            byeAcknowledged = false;
            peerClosed = false;
            sessionFailed = false;
            sessionFailureText.clear();
            awaitingResponse = false;
            pendingResponse.reset();
        }
        firstByteSeen = false;
        input.clear();
        firstByteTimer = INVALID_TIMER_ID;
        frameTimer = INVALID_TIMER_ID;

        silenceLibhvFileLogger();

        client = std::make_unique<hv::TcpClient>();
        client->onConnection = [this](const hv::SocketChannelPtr& channel) {
            onConnectionEvent(channel);
        };
        client->onMessage = [this](const hv::SocketChannelPtr&, hv::Buffer* buffer) {
            onMessageEvent(buffer);
        };

        // IPv4 字面量非法时 createsocket 同步失败；此时不启动事件循环，直接失败。
        if (client->createsocket(static_cast<int>(port), host.c_str()) < 0) {
            printHandshakeFailure("Failed to connect to " + host + ":" +
                                  std::to_string(port) + ".");
            stopClient();
            return 1;
        }
        // start(true)：启动后台事件循环线程并在其中发起连接。
        client->start(true);

        // 契约：Client 在收到握手答复前不设本地期限；结果由事件循环线程通知。
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return handshakeDone; });
        if (!handshakeOk) {
            const std::string reason = failureText;
            lock.unlock();
            printHandshakeFailure(reason);
            stopClient();
            return 1;
        }
        lock.unlock();

        std::cout << kSuccessText << std::endl;

        // ---- TPE> 循环（contracts/cli.md §3/§3.1/§5/§6；protocol.md §5） ----
        // 提示符在握手完成、每个本地拒绝与每条 RESPONSE 渲染之后各输出一次；
        // 循环内不再重复输出，避免同一输入前出现两个提示符。
        showInputPrompt();
        for (;;) {
            if (reportSessionFailureIfAny()) {
                return 1;
            }

            std::string line;
            if (!std::getline(std::cin, line)) {
                break; // 标准输入 EOF → 正常结束（T023 路径）
            }
            // std::getline 已剥离 LF；仅再去掉紧邻的 CR。内嵌 CR/LF 保留，
            // 由发送前校验按 cli.md §3.1 拒绝。
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            // 等待输入期间到达的 ERROR/断线：下一次输入返回后、处理任何
            // 命令（含 exit）之前优先报告（cli.md §7）。若等到发送失败再
            // 报告，只能回落到通用断线文本，丢失 Server 的 ERROR reason。
            if (reportSessionFailureIfAny()) {
                return 1;
            }

            if (line == "exit") {
                break; // exit 不发送 COMMAND（T023 路径）
            }

            const std::string_view rejection = validateCommandLine(line);
            if (!rejection.empty()) {
                // 本地拒绝：写标准输出、不发送、保留 TPE>（cli.md §3.1）。
                std::cout << rejection << std::endl;
                showInputPrompt();
                continue;
            }

            if (!exchangeCommand(line)) {
                return 1; // 已输出原因并以退出码 1 结束
            }
        }

        return finishSession();
    }

    // --------------------------------------------------------
    // run() 线程：活动期命令交换与渲染（T028）
    // --------------------------------------------------------

    /// 会话失败/对端关闭的当前原因；无失败返回空串。调用方必须持有 mutex。
    std::string currentSessionFailure() const
    {
        if (sessionFailed) {
            return sessionFailureText;
        }
        if (peerClosed) {
            return std::string(kClosedByServerText);
        }
        return {};
    }

    /// 已记录会话失败（活动期 ERROR/断线）时立即报告到标准错误并停止事件循环。
    /// EOF 与断线同时出现时由本检查与 finishSession() 共同保证断线优先
    /// （cli.md §7）。
    /// @retval true 已报告原因，调用方必须以退出码 1 结束；
    ///         false 尚无已记录的失败。
    bool reportSessionFailureIfAny()
    {
        std::string failure;
        {
            const std::lock_guard<std::mutex> guard(mutex);
            failure = currentSessionFailure();
        }
        if (failure.empty()) {
            return false;
        }
        stopClient();
        std::cerr << failure << std::endl;
        return true;
    }

    /// 渲染本地提示符（线上 prompt 不含尾随空格，渲染层添加；cli.md §5）。
    void showInputPrompt() const
    {
        std::cout << kPromptText << std::flush;
    }

    /// 渲染 RESPONSE（cli.md §5；protocol.md §5/§8）：
    /// 先原样输出 output；非空且不以 '\n' 结尾时补一个 '\n'；
    /// 随后输出线上 prompt + 一个空格（尾随空格由渲染层添加）。
    void renderResponse(const RemoteResponse& response) const
    {
        std::cout << response.output;
        if (!response.output.empty() && response.output.back() != '\n') {
            std::cout << '\n';
        }
        std::cout << response.prompt << ' ' << std::flush;
    }

    /// 发送一条已通过校验的命令并阻塞等待 RESPONSE，随后渲染。
    /// @retval true 已收到并渲染 RESPONSE，继续会话；
    ///         false 发送失败或等待期间对端关闭/会话失败，已输出原因、需退出 1。
    bool exchangeCommand(const std::string& line)
    {
        const std::vector<std::uint8_t> frame =
            encodeFrame(MessageType::Command, toPayload(line));

        {
            const std::lock_guard<std::mutex> guard(mutex);
            awaitingResponse = true;
            pendingResponse.reset();
        }
        if (client->send(frame.data(), static_cast<int>(frame.size())) < 0) {
            // 发送失败的同一时点可能已记录会话失败（例如 ERROR 与关闭事件
            // 先于此检查到达）：优先透传具体原因，不回落通用断线文本。
            std::string failure;
            {
                const std::lock_guard<std::mutex> guard(mutex);
                awaitingResponse = false;
                failure = currentSessionFailure();
            }
            stopClient();
            std::cerr << (failure.empty() ? std::string(kClosedByServerText) : failure)
                      << std::endl;
            return false;
        }

        RemoteResponse response;
        std::string failure;
        {
            std::unique_lock<std::mutex> lock(mutex);
            // 基本阻塞等待：RESPONSE、会话失败、对端关闭或 BYE 都会唤醒。
            // T037 再细化等待输入/响应的报告时机。
            cv.wait(lock, [this] {
                return pendingResponse.has_value() || sessionFailed || peerClosed ||
                       byeAcknowledged;
            });
            if (pendingResponse.has_value()) {
                response = std::move(*pendingResponse);
                pendingResponse.reset();
            } else {
                failure = currentSessionFailure();
                if (failure.empty()) {
                    // 等待 RESPONSE 期间收到未预期的 BYE：按对端结束处理。
                    failure = std::string(kClosedByServerText);
                }
            }
        }
        if (!failure.empty()) {
            stopClient();
            std::cerr << failure << std::endl;
            return false;
        }

        renderResponse(response);
        return true;
    }

    // --------------------------------------------------------
    // 事件循环线程：连接与帧接收
    // --------------------------------------------------------

    void onConnectionEvent(const hv::SocketChannelPtr& channel)
    {
        if (!channel) {
            return;
        }
        if (channel->isConnected()) {
            onConnected();
        } else {
            onPeerClosed(channel);
        }
    }

    void onConnected()
    {
        {
            const std::lock_guard<std::mutex> guard(mutex);
            if (handshakeDone || phase != Phase::Connecting) {
                return;
            }
            phase = Phase::Handshaking;
        }
        // 契约 §4：首字节静默期限从连接建立时起算。
        startFirstByteTimer();

        const std::vector<std::uint8_t> hello =
            encodeFrame(MessageType::Hello, toPayload(kClientId));
        if (client->send(hello.data(), static_cast<int>(hello.size())) < 0) {
            failHandshake(std::string(kSendHandshakeFailedText));
        }
    }

    void onMessageEvent(hv::Buffer* buffer)
    {
        if (buffer == nullptr || buffer->size() == 0) {
            return;
        }
        Phase current = Phase::Finished;
        {
            const std::lock_guard<std::mutex> guard(mutex);
            current = phase;
        }
        if (current == Phase::Finished) {
            return; // 已终结：不再解析任何字节
        }

        // 契约 §4：首字节到达后撤销静默期限，改为“完整帧”期限。
        // 期限以首字节的接收时点为准：事件循环繁忙时定时器回调可能晚于数据回调，
        // 因此这里用绝对期限判断迟到，而不是依赖回调先后。
        if (current == Phase::Handshaking && !firstByteSeen) {
            firstByteSeen = true;
            const bool late = std::chrono::steady_clock::now() > firstByteDeadline;
            killTimer(firstByteTimer);
            if (late) {
                input.clear();
                failHandshake(std::string(kNoResponseText));
                return;
            }
            startFrameTimer();
        }
        // 完整帧期限同样以时点为准：数据回调晚于期限时不得再接受完整帧。
        if (current == Phase::Handshaking && std::chrono::steady_clock::now() > frameDeadline) {
            input.clear();
            killTimer(frameTimer);
            failHandshake(std::string(kIncompleteFrameText));
            return;
        }

        const auto* const bytes = static_cast<const std::uint8_t*>(buffer->data());
        input.insert(input.end(), bytes, bytes + buffer->size());
        processInput();
    }

    void processInput()
    {
        for (;;) {
            const DecodeResult decoded = tryDecodeFrame(input);
            if (decoded.status == DecodeStatus::NeedMoreData) {
                return;
            }
            if (decoded.status == DecodeStatus::Failed) {
                input.clear();
                killTimer(frameTimer);
                handleDecodeFailure(decoded.error);
                return;
            }
            input.erase(input.begin(),
                        input.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));
            killTimer(frameTimer);
            if (!handleFrame(decoded.frame)) {
                input.clear();
                return;
            }
        }
    }

    // --------------------------------------------------------
    // 帧分派
    // --------------------------------------------------------

    /// @retval true 继续解析后续帧；false 会话已终结，停止解析。
    bool handleFrame(const Frame& frame)
    {
        Phase current = Phase::Finished;
        {
            const std::lock_guard<std::mutex> guard(mutex);
            current = phase;
        }
        if (current == Phase::Connecting || current == Phase::Handshaking) {
            return handleHandshakeFrame(frame);
        }
        return handleSessionFrame(frame);
    }

    /// 握手帧（契约 §4）：只接受载荷合法的 WELCOME/BUSY/ERROR，其余均失败。
    bool handleHandshakeFrame(const Frame& frame)
    {
        switch (frame.header.type) {
        case MessageType::Welcome:
            if (validatePayload(MessageType::Welcome, frame.payload).has_value()) {
                failHandshake(std::string(kInvalidMessageText));
                return false;
            }
            completeHandshake();
            return true;

        case MessageType::Busy:
            if (validatePayload(MessageType::Busy, frame.payload).has_value()) {
                failHandshake(std::string(kInvalidMessageText));
            } else {
                failHandshake(std::string(kBusyText));
            }
            return false;

        case MessageType::Error: {
            if (validatePayload(MessageType::Error, frame.payload).has_value()) {
                failHandshake(std::string(kInvalidMessageText));
                return false;
            }
            // 合法 ERROR(code 1–6 + UTF-8 reason)：透传 Server 的具体原因。
            std::string reason(frame.payload.begin() + 1, frame.payload.end());
            if (reason.empty()) {
                reason = std::string(kRejectedText);
            }
            failHandshake(std::move(reason));
            return false;
        }

        default:
            // 握手期收到非 WELCOME/BUSY/ERROR 的完整帧（契约 §4）。
            failHandshake(std::string(kUnexpectedMessageText));
            return false;
        }
    }

    /// 活动期帧（契约 §7）：BYE 正常结束；ERROR 或意外帧按失败结束。
    bool handleSessionFrame(const Frame& frame)
    {
        switch (frame.header.type) {
        case MessageType::Bye:
            if (validatePayload(MessageType::Bye, frame.payload).has_value()) {
                failSession(std::string(kInvalidSessionEndText));
                return false;
            }
            {
                const std::lock_guard<std::mutex> guard(mutex);
                byeAcknowledged = true;
            }
            cv.notify_all();
            return true;

        case MessageType::Error: {
            if (validatePayload(MessageType::Error, frame.payload).has_value()) {
                failSession(std::string(kInvalidMessageText));
                return false;
            }
            std::string reason(frame.payload.begin() + 1, frame.payload.end());
            if (reason.empty()) {
                reason = std::string(kRejectedText);
            }
            failSession(std::move(reason));
            return false;
        }

        case MessageType::Response: {
            Result<RemoteResponse, ProtocolError> decoded = decodeResponse(frame.payload);
            if (!decoded.has_value()) {
                // RESPONSE 结构非法（protocol.md §2/§5）。
                failSession(std::string(kInvalidMessageText));
                return false;
            }
            // Phase 01 不使用 status=1（protocol.md §2）：按非法消息处理并退出 1。
            if (decoded.value().status == ResponseStatus::Exit) {
                failSession(std::string(kInvalidMessageText));
                return false;
            }
            bool expected = false;
            {
                const std::lock_guard<std::mutex> guard(mutex);
                expected = awaitingResponse;
                if (expected) {
                    awaitingResponse = false;
                    pendingResponse = std::move(decoded.value());
                }
            }
            if (!expected) {
                // 未发送 COMMAND 却收到 RESPONSE：视为意外消息。
                failSession(std::string(kUnexpectedMessageText));
                return false;
            }
            cv.notify_all();
            return true;
        }

        default:
            // 活动期收到其余服务端消息属意外（契约 §7）。
            failSession(std::string(kUnexpectedMessageText));
            return false;
        }
    }

    /// 帧解码失败（版本/类型/长度非法）。
    void handleDecodeFailure(const ProtocolError& error)
    {
        Phase current = Phase::Finished;
        {
            const std::lock_guard<std::mutex> guard(mutex);
            current = phase;
        }
        const bool handshaking =
            (current == Phase::Connecting || current == Phase::Handshaking);
        if (handshaking) {
            switch (error.code) {
            case ErrorCode::BadMessageType:
                failHandshake(std::string(kUnexpectedMessageText));
                return;
            case ErrorCode::UnsupportedVersion:
                // 版本不兼容属于握手失败：显示具体原因（cli.md §7/§8）。
                failHandshake(error.reason.empty() ? std::string(kUnexpectedMessageText)
                                                   : error.reason);
                return;
            case ErrorCode::FrameTooLarge:
            case ErrorCode::InvalidPayload:
            case ErrorCode::HandshakeTimeout:
            case ErrorCode::Internal:
            default:
                failHandshake(std::string(kInvalidMessageText));
                return;
            }
        }
        failSession(error.reason.empty() ? std::string(kInvalidMessageText) : error.reason);
    }

    // --------------------------------------------------------
    // 握手结果
    // --------------------------------------------------------

    void completeHandshake()
    {
        {
            const std::lock_guard<std::mutex> guard(mutex);
            if (handshakeDone) {
                return;
            }
            handshakeDone = true;
            handshakeOk = true;
            phase = Phase::Active;
        }
        killTimer(firstByteTimer);
        killTimer(frameTimer);
        cv.notify_all();
    }

    void failHandshake(std::string reason)
    {
        {
            const std::lock_guard<std::mutex> guard(mutex);
            if (handshakeDone) {
                return;
            }
            handshakeDone = true;
            handshakeOk = false;
            failureText = std::move(reason);
            phase = Phase::Finished;
        }
        killTimer(firstByteTimer);
        killTimer(frameTimer);
        closeChannel();
        cv.notify_all();
    }

    void onPeerClosed(const hv::SocketChannelPtr& channel)
    {
        std::string connectReason;
        bool connectionFailed = false;
        {
            const std::lock_guard<std::mutex> guard(mutex);
            // 只有从未进入握手阶段才属于“连接未建立”；已建立后关闭保留
            // 既有 closed 文本，避免把对端关闭误报为 connect 失败。
            if (!handshakeDone && phase == Phase::Connecting) {
                connectionFailed = true;
                connectReason = connectionFailureReason(channel);
            }
            peerClosed = true;
            if (!handshakeDone) {
                handshakeDone = true;
                handshakeOk = false;
                phase = Phase::Finished;
                if (connectionFailed) {
                    failureText = "Failed to connect to " + host + ":" +
                                  std::to_string(port);
                    if (!connectReason.empty()) {
                        failureText += ": " + connectReason;
                    }
                } else {
                    failureText = std::string(kClosedByServerText);
                }
            }
        }
        cv.notify_all();
    }

    void failSession(std::string reason)
    {
        {
            const std::lock_guard<std::mutex> guard(mutex);
            if (!sessionFailed) {
                sessionFailed = true;
                sessionFailureText = std::move(reason);
            }
        }
        closeChannel();
        cv.notify_all();
    }

    // --------------------------------------------------------
    // run() 线程：正常结束（BYE + 确认，期限 1 秒；契约 §7）
    // --------------------------------------------------------

    int finishSession()
    {
        enum class EndResult { Acknowledged, Disconnected, Failed, Timeout };

        // 断线/协议错误优先于 exit/EOF（cli.md §7）。
        EndResult outcome = EndResult::Timeout;
        std::string failure;
        {
            const std::lock_guard<std::mutex> guard(mutex);
            if (sessionFailed) {
                outcome = EndResult::Failed;
                failure = sessionFailureText;
            } else if (peerClosed) {
                outcome = EndResult::Disconnected;
                failure = std::string(kClosedByServerText);
            } else {
                phase = Phase::Ending;
            }
        }
        if (outcome != EndResult::Timeout) {
            stopClient();
            std::cerr << failure << std::endl;
            return 1;
        }

        const std::vector<std::uint8_t> bye = encodeFrame(MessageType::Bye, {});
        if (client->send(bye.data(), static_cast<int>(bye.size())) < 0) {
            stopClient();
            std::cerr << kClosedByServerText << std::endl;
            return 1;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, kSessionEndTimeout, [this] {
                return byeAcknowledged || peerClosed || sessionFailed;
            });
            if (sessionFailed) {
                outcome = EndResult::Failed;
                failure = sessionFailureText;
            } else if (byeAcknowledged) {
                outcome = EndResult::Acknowledged;
            } else if (peerClosed) {
                outcome = EndResult::Disconnected;
                failure = std::string(kClosedByServerText);
            } else {
                outcome = EndResult::Timeout;
                failure = std::string(kEndNotAcknowledgedText);
            }
        }
        if (outcome == EndResult::Timeout) {
            // 期限到达且连接仍在：主动关闭（契约 §7）。
            closeChannel();
        }
        stopClient();
        if (outcome == EndResult::Acknowledged) {
            return 0;
        }
        std::cerr << failure << std::endl;
        return 1;
    }

    // --------------------------------------------------------
    // 接收期限（毫秒精度；首字节与完整帧双期限，契约 §4）
    // --------------------------------------------------------

    void startFirstByteTimer()
    {
        const hv::EventLoopPtr loop = client ? client->loop() : hv::EventLoopPtr{};
        if (!loop) {
            failHandshake(std::string(kNoResponseText));
            return;
        }
        // 记录绝对期限：数据回调与定时器回调的先后不改变迟到判定（契约 §4）。
        firstByteDeadline = std::chrono::steady_clock::now() + timeouts.firstByte;
        firstByteTimer = loop->setTimer(
            toTimerMilliseconds(timeouts.firstByte),
            [this](hv::TimerID) { onFirstByteDeadline(); }, 1);
    }

    void startFrameTimer()
    {
        const hv::EventLoopPtr loop = client ? client->loop() : hv::EventLoopPtr{};
        if (!loop) {
            failHandshake(std::string(kIncompleteFrameText));
            return;
        }
        // 记录绝对期限：与首字节期限同一原则（契约 §4）。
        frameDeadline = std::chrono::steady_clock::now() + timeouts.frameComplete;
        frameTimer = loop->setTimer(
            toTimerMilliseconds(timeouts.frameComplete),
            [this](hv::TimerID) { onFrameDeadline(); }, 1);
    }

    void onFirstByteDeadline()
    {
        firstByteTimer = INVALID_TIMER_ID;
        bool expired = false;
        {
            const std::lock_guard<std::mutex> guard(mutex);
            expired = !handshakeDone && !firstByteSeen;
        }
        if (expired) {
            failHandshake(std::string(kNoResponseText));
        }
    }

    void onFrameDeadline()
    {
        frameTimer = INVALID_TIMER_ID;
        bool expired = false;
        {
            const std::lock_guard<std::mutex> guard(mutex);
            expired = !handshakeDone;
        }
        if (expired) {
            failHandshake(std::string(kIncompleteFrameText));
        }
    }

    void killTimer(hv::TimerID& timer)
    {
        if (timer == INVALID_TIMER_ID) {
            return;
        }
        const hv::TimerID id = timer;
        timer = INVALID_TIMER_ID;
        const hv::EventLoopPtr loop = client ? client->loop() : hv::EventLoopPtr{};
        if (loop) {
            loop->killTimer(id);
        }
    }

    // --------------------------------------------------------
    // 连接生命周期
    // --------------------------------------------------------

    void closeChannel()
    {
        if (client && client->channel) {
            client->channel->close();
        }
    }

    void stopClient()
    {
        if (!client) {
            return;
        }
        client->stop(true);
    }
};

// ============================================================
// 公开接口
// ============================================================

RemoteClient::RemoteClient(std::string host, std::uint16_t port)
    : m_impl(std::make_unique<Impl>(std::move(host), port))
{
}

RemoteClient::~RemoteClient() = default;

int RemoteClient::run()
{
    return m_impl->runSession();
}

void RemoteClient::setTimeoutsForTesting(const ClientTimeouts& timeouts)
{
    m_impl->timeouts = timeouts;
}

} // namespace tpe::remote

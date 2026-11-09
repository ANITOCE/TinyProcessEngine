#include "RemoteServer.hpp"

#include "RemoteCommandExecutor.hpp"
#include "RemoteLog.hpp"
#include "RemoteProtocol.hpp"

#include <hv/TcpServer.h>
#include <hv/hlog.h>

#include <atomic>
#include <chrono>
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
// 固定文本与载荷辅助（contracts/protocol.md §2/§3；contracts/cli.md §2）
// ============================================================

/// BUSY 的固定载荷文本：`Server is busy.`。
constexpr std::string_view kBusyText = "Server is busy.";

/// BUSY 写回后的有界关闭延迟（毫秒）。
/// 契约要求“发送 BUSY 并关闭”，但同步 close 会在对端尚未完成连接建立/读取时
/// 以 FIN/RST 结束连接，导致对端丢失 BUSY（T023 集成复现）。延迟关闭使对端
/// 先收到 BUSY；对端读到 BUSY 后通常会主动关闭，此定时器仅作有界兜底。
constexpr int kBusyCloseDelayMs = 1000;

/// Draining 清理延迟（毫秒，contracts/protocol.md §3/§6）。
/// 会话结束（BYE、断线或非法帧关闭）后不在此次事件内回到 `Listening`：
/// 释放占用推迟到下一轮事件循环，让已排队的连接事件先按 `Draining` 判定为
/// BUSY。定时器与事件循环同线程，不引入额外线程。
constexpr int kDrainingCleanupDelayMs = 10;

/// 同一接收缓冲中后续帧的续行延迟（毫秒，contracts/protocol.md §6）。
/// 一条 COMMAND 执行完毕后不在同一回调内继续执行缓冲中的下一条命令：
/// 先把控制权交回事件循环，使已排队的断开事件先被处理，仍留在接收缓冲区、
/// 未进入执行函数的命令据此丢弃（FR-012）。
constexpr int kInputResumeDelayMs = 1;

/// Server 开始监听时写入标准输出的固定文本。
constexpr std::string_view kWaitingText = "Waiting for connection ...";

/// Server 接纳 Client 时写入标准输出的固定文本。
constexpr std::string_view kAcceptedText = "Connection successful.";

/// RESPONSE 的线上提示符；不含尾随空格（尾随空格由 Client 渲染层添加，cli.md §5）。
constexpr std::string_view kPromptText = "TPE>";

/// 结果超过单条 RESPONSE 上限时的固定错误文本（protocol.md §5；cli.md §6）。
constexpr std::string_view kOversizeResponseText = "Response exceeds the maximum message size.";

/// 文本 → 载荷字节序列。
std::vector<std::uint8_t> toPayload(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

/// ERROR 载荷编码：code(uint8) + reason(UTF-8)（contracts/protocol.md §2）。
std::vector<std::uint8_t> encodeErrorPayload(ErrorCode code, std::string_view reason)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(1 + reason.size());
    payload.push_back(static_cast<std::uint8_t>(code));
    payload.insert(payload.end(), reason.begin(), reason.end());
    return payload;
}

/// 写出一帧：length/version/type 由 encodeFrame 编码（T014）。
/// 写失败（对端已关闭）可忽略：契约 §3 允许 BUSY/ERROR 写回失败。
void writeFrame(const hv::SocketChannelPtr& channel, MessageType type,
                std::vector<std::uint8_t> payload)
{
    if (!channel) {
        return;
    }
    const std::vector<std::uint8_t> bytes = encodeFrame(type, std::move(payload));
    channel->write(bytes.data(), static_cast<int>(bytes.size()));
}

/// 静默 libhv 的默认日志器（每进程恰好一次）。
/// libhv 默认 logger 未设 handler 时会回落到文件输出，在进程工作目录生成
/// `libhv.YYYYMMDD.log`；这与 contracts/logging.md §4（只写 Server 的标准控制台
/// 输出、不单独落盘）冲突。`LOG_LEVEL_SILENT` 使 `logger_print` 在写文件之前即返回
/// （libhv base/hlog.c：`if (level < logger->level) return -10;`）。
void silenceLibhvFileLogger()
{
    static std::once_flag once;
    std::call_once(once, [] { hlog_set_level(LOG_LEVEL_SILENT); });
}

} // namespace

// ============================================================
// Impl：libhv 监听、单会话状态机与日志
// ============================================================

struct RemoteServer::Impl {
    /// 会话状态（contracts/protocol.md §3/§4/§6/§7；data-model.md §3）。
    /// 无会话即为 `Listening`；只有 `Listening` 才能进入 `Handshaking`。
    /// `Draining`：会话已结束但清理尚未完成，`session` 仍非空，
    /// 期间被处理的连接事件一律按占用返回 BUSY。
    enum class State {
        Handshaking, // 已接纳连接，等待完整 HELLO
        Active,      // 已发送 WELCOME，进入会话
        Draining,    // 会话已结束，清理完成前保持占用
    };

    /// 当前会话（同一时刻至多一个）。
    struct Session {
        hv::SocketChannelPtr channel;
        State state = State::Handshaking;
        std::vector<std::uint8_t> input;         // 未消费的帧字节
        hv::TimerID timer = INVALID_TIMER_ID;    // 握手期限定时器
        hv::TimerID cleanupTimer = INVALID_TIMER_ID; // Draining 清理定时器
        hv::TimerID resumeTimer = INVALID_TIMER_ID;  // 缓冲后续帧续行定时器
    };

    Impl(std::uint16_t listenPort, tpe::ProcessEngine& processEngine)
        : port(listenPort)
        , engine(processEngine)
    {
    }

    // ---- 配置（启动前设置） ----
    std::uint16_t port;
    /// 活动期 COMMAND 的执行目标；只在事件循环线程访问（contracts/logging.md §4）。
    tpe::ProcessEngine& engine;
    std::chrono::milliseconds handshakeTimeout{5000};

    // ---- 控制面：start()/stop() 调用线程访问，与事件循环线程通过原子量同步 ----
    std::mutex controlMutex; // 串行化 start()/stop()
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::unique_ptr<hv::TcpServer> server; // 事件循环运行期间保持同一对象

    // ---- 数据面：仅事件循环线程访问（stop() 在 join 之后才清理） ----
    std::shared_ptr<Session> session;

    // --------------------------------------------------------
    // 控制面
    // --------------------------------------------------------

    /// 绑定并开始监听；绑定失败或端口占用返回 false（contracts/cli.md §8）。
    bool startListening()
    {
        // 首次启动监听之前静默 libhv 内部文件日志（contracts/logging.md §4）。
        silenceLibhvFileLogger();

        const std::lock_guard<std::mutex> guard(controlMutex);
        if (running.load(std::memory_order_acquire)) {
            return true;
        }

        auto created = std::make_unique<hv::TcpServer>();
        // createsocket() 同步完成 bind+listen，因此这里能立刻发现端口占用/绑定失败；
        // 事件循环线程随后只负责 accept。
        if (created->createsocket(port) < 0) {
            return false;
        }
        // libhv 默认按 CPU 核数创建 worker 线程；本实现要求会话状态、握手定时器与
        // 日志全部在同一事件循环线程内完成（contracts/logging.md §4），因此固定为
        // 0 个 worker：连接与回调都在 accept 所在的事件循环线程处理。
        created->setThreadNum(0);
        created->onConnection = [this](const hv::SocketChannelPtr& channel) {
            handleConnection(channel);
        };
        created->onMessage = [this](const hv::SocketChannelPtr& channel, hv::Buffer* buffer) {
            handleMessage(channel, buffer);
        };

        server = std::move(created);
        stopping.store(false, std::memory_order_release);
        running.store(true, std::memory_order_release);
        server->start(true); // 启动事件循环线程并注册 accept

        std::cout << kWaitingText << std::endl;
        return true;
    }

    /// 停止监听并关闭会话；未启动或已停止时无副作用。
    void stopListening()
    {
        const std::lock_guard<std::mutex> guard(controlMutex);
        running.store(false, std::memory_order_release);
        if (!server) {
            stopping.store(false, std::memory_order_release);
            return;
        }

        // 停止收尾期间不再写日志/固定文本（契约：stop() 之后不写全局流）。
        stopping.store(true, std::memory_order_release);
        // stop(true) 停止事件循环并 join：关闭回调（含事件循环销毁时的收尾）在此期间执行完。
        server->stop(true);
        server.reset();
        // 事件循环线程已退出，残留会话引用可安全清理。
        session.reset();
        stopping.store(false, std::memory_order_release);
    }

    // --------------------------------------------------------
    // 事件循环线程：连接事件
    // --------------------------------------------------------

    void handleConnection(const hv::SocketChannelPtr& channel)
    {
        if (!channel) {
            return;
        }
        if (channel->isConnected()) {
            acceptChannel(channel);
        } else {
            releaseClosedChannel(channel);
        }
    }

    /// 连接事件被处理时的占用判定（contracts/protocol.md §3）：非空闲一律 BUSY。
    void acceptChannel(const hv::SocketChannelPtr& channel)
    {
        if (stopping.load(std::memory_order_acquire)) {
            channel->close();
            return;
        }

        if (session) {
            // 已占用（Handshaking / Active / Draining）：发送 BUSY 并关闭，记 1 条
            // reject/busy；清理完成后 `session` 为空，连接才被接纳。
            writeFrame(channel, MessageType::Busy, toPayload(kBusyText));
            logSessionEvent(channel, SessionEvent::Reject, LogResult::Busy);
            scheduleBusyClose(channel);
            return;
        }

        auto accepted = std::make_shared<Session>();
        accepted->channel = channel;
        session = accepted;
        // 契约 logging.md §3：失败握手属于“接纳前握手失败”，只记 reject/error；
        // 因此 accept/ok 与 `Connection successful.` 推迟到 HELLO 校验通过后记录。
        startHandshakeTimer(accepted);
    }

    /// BUSY 写回后的有界延迟关闭（见 `kBusyCloseDelayMs`）。
    /// 对端读到 BUSY 后会主动关闭，此定时器只处理“对端不关闭”的兜底场景；
    /// 服务器停止时事件循环销毁，未触发的定时器随之丢弃。
    void scheduleBusyClose(const hv::SocketChannelPtr& channel)
    {
        const hv::EventLoopPtr loop = server ? server->loop() : hv::EventLoopPtr{};
        if (!loop) {
            channel->close();
            return;
        }
        loop->setTimeout(kBusyCloseDelayMs, [channel](hv::TimerID) {
            if (channel && channel->isConnected()) {
                channel->close();
            }
        });
    }

    /// 连接关闭（对端关闭或事件循环收尾）：释放占用并按状态记录日志。
    void releaseClosedChannel(const hv::SocketChannelPtr& channel)
    {
        if (!session || session->channel != channel) {
            return; // 非当前会话（已被 BUSY 拒绝的连接或已结束的会话）
        }
        const auto closed = session;
        if (closed->state == State::Draining) {
            // 清理期间自身连接的关闭事件（BYE/ERROR 后主动关闭、或对端先关）：
            // 不计入其它事件，保证每次会话结束恰好 1 条 disconnect 记录（logging.md §4）。
            return;
        }
        if (closed->state == State::Handshaking) {
            // 握手期对端关闭：记 1 条 reject/error 并回到 Listening（契约 §3）。
            session.reset();
            cancelHandshakeTimer(closed);
            logSessionEvent(channel, SessionEvent::Reject, LogResult::Error);
            return;
        }
        // 已接纳会话未收到 BYE 即断开：记 1 条 disconnect/disconnected（契约 §7）。
        logSessionEvent(channel, SessionEvent::Disconnect, LogResult::Disconnected);
        beginDraining(closed);
    }

    // --------------------------------------------------------
    // 事件循环线程：帧处理
    // --------------------------------------------------------

    void handleMessage(const hv::SocketChannelPtr& channel, hv::Buffer* buffer)
    {
        if (!buffer || buffer->size() == 0) {
            return;
        }
        if (!session || session->channel != channel) {
            return;
        }
        if (session->state == State::Draining) {
            return; // 会话已结束：清理完成前不再处理任何输入（§6 未开始的命令丢弃）
        }
        if (stopping.load(std::memory_order_acquire)) {
            return;
        }

        const auto current = session;
        const auto* const bytes = static_cast<const std::uint8_t*>(buffer->data());
        current->input.insert(current->input.end(), bytes, bytes + buffer->size());
        processInput(current);
    }

    /// 从接收缓冲中反复解出完整帧；任一步结束会话即停止。
    /// 一条 COMMAND 返回后先让出事件循环（见 `scheduleInputResume`）：
    /// 已排队的断开事件先被处理，缓冲中尚未开始的命令据此丢弃（§6）。
    void processInput(const std::shared_ptr<Session>& current)
    {
        for (;;) {
            if (session != current || current->state == State::Draining) {
                return; // 会话已结束：未开始的命令一律丢弃
            }
            const DecodeResult decoded = tryDecodeFrame(current->input);
            if (decoded.status == DecodeStatus::NeedMoreData) {
                return;
            }
            if (decoded.status == DecodeStatus::Failed) {
                // 帧级错误（版本/类型/长度）：握手期记 reject/error，活动期记 disconnect/error。
                abortSession(current, decoded.error.code, decoded.error.reason,
                             eventFor(current->state));
                return;
            }
            current->input.erase(
                current->input.begin(),
                current->input.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));
            const bool commandFrame = decoded.frame.header.type == MessageType::Command &&
                                      current->state == State::Active;
            dispatchFrame(current, decoded.frame);
            if (session != current) {
                return; // 帧处理已结束会话
            }
            if (commandFrame) {
                scheduleInputResume(current);
                return;
            }
        }
    }

    /// 失败事件归属：握手期按 reject，活动期按 disconnect（contracts/protocol.md §3/§7）。
    static SessionEvent eventFor(State state)
    {
        return (state == State::Handshaking) ? SessionEvent::Reject : SessionEvent::Disconnect;
    }

    void dispatchFrame(const std::shared_ptr<Session>& current, const Frame& frame)
    {
        if (current->state == State::Handshaking) {
            handleHandshakeFrame(current, frame);
        } else {
            handleActiveFrame(current, frame);
        }
    }

    /// 握手帧（contracts/protocol.md §4）：只接受合法 HELLO，其余按错误码关闭。
    void handleHandshakeFrame(const std::shared_ptr<Session>& current, const Frame& frame)
    {
        if (frame.header.type != MessageType::Hello) {
            abortSession(current, ErrorCode::BadMessageType, "handshake requires HELLO",
                         SessionEvent::Reject);
            return;
        }
        if (const std::optional<ProtocolError> invalid =
                validatePayload(MessageType::Hello, frame.payload);
            invalid.has_value()) {
            abortSession(current, invalid->code, invalid->reason, SessionEvent::Reject);
            return;
        }

        cancelHandshakeTimer(current);
        current->state = State::Active;
        writeFrame(current->channel, MessageType::Welcome, {});
        // 握手成功即“接纳”：此时才记 accept/ok 并输出固定接纳提示（contracts/logging.md §3）。
        logSessionEvent(current->channel, SessionEvent::Accept, LogResult::Ok);
        if (!stopping.load(std::memory_order_acquire)) {
            std::cout << kAcceptedText << std::endl;
        }
    }

    /// 活动期帧（contracts/protocol.md §5/§7）：BYE 正常结束；COMMAND 执行并回 RESPONSE。
    void handleActiveFrame(const std::shared_ptr<Session>& current, const Frame& frame)
    {
        switch (frame.header.type) {
        case MessageType::Bye:
            if (const std::optional<ProtocolError> invalid =
                    validatePayload(MessageType::Bye, frame.payload);
                invalid.has_value()) {
                abortSession(current, invalid->code, invalid->reason, SessionEvent::Disconnect);
                return;
            }
            completeBye(current);
            return;

        case MessageType::Command:
            handleCommandFrame(current, frame);
            return;

        default:
            // 客户端不得发送 WELCOME/BUSY/ERROR/RESPONSE（契约 §2）。
            abortSession(current, ErrorCode::BadMessageType, "unexpected message type",
                         SessionEvent::Disconnect);
            return;
        }
    }

    /// 活动期 COMMAND（contracts/protocol.md §5/§7）：载荷非法 → ERROR(6) 并关闭；
    /// 合法 → 交给 RemoteCommandExecutor 执行并回 RESPONSE，会话保持 Active。
    void handleCommandFrame(const std::shared_ptr<Session>& current, const Frame& frame)
    {
        if (const std::optional<ProtocolError> invalid =
                validatePayload(MessageType::Command, frame.payload);
            invalid.has_value()) {
            abortSession(current, invalid->code, invalid->reason, SessionEvent::Disconnect);
            return;
        }

        const std::string line(frame.payload.begin(), frame.payload.end());
        const RemoteCommandResult result = executeRemoteCommand(line, engine);
        sendResponse(current, result.status, result.output);
    }

    /// 编码并发送 RESPONSE（contracts/protocol.md §5）：
    /// 结果超过单条响应上限时改发固定的 status=2 响应；不截断、不分片，会话保留。
    void sendResponse(const std::shared_ptr<Session>& current, ResponseStatus status,
                      const std::string& output)
    {
        const Result<std::vector<std::uint8_t>, ProtocolError> encoded =
            encodeResponse(status, std::string(kPromptText), output);
        if (encoded.has_value()) {
            writeFrame(current->channel, MessageType::Response, encoded.value());
            return;
        }
        if (encoded.error().code == ErrorCode::FrameTooLarge) {
            sendOversizeFallback(current);
            return;
        }
        failResponseEncoding(current, encoded.error());
    }

    /// 超限回退（protocol.md §5；cli.md §6）：status=2 + 固定文本，结果不截断、不分片。
    void sendOversizeFallback(const std::shared_ptr<Session>& current)
    {
        const Result<std::vector<std::uint8_t>, ProtocolError> encoded =
            encodeResponse(ResponseStatus::CommandError, std::string(kPromptText),
                           std::string(kOversizeResponseText));
        if (encoded.has_value()) {
            writeFrame(current->channel, MessageType::Response, encoded.value());
            return;
        }
        failResponseEncoding(current, encoded.error());
    }

    /// 其余编码错误（如 prompt 超限）理论上不会发生：写一条标准错误诊断并按内部错误
    /// 关闭会话（ERROR(5) + 1 条 disconnect/error），不崩溃、不下发残缺响应。
    void failResponseEncoding(const std::shared_ptr<Session>& current,
                              const ProtocolError& error)
    {
        if (!stopping.load(std::memory_order_acquire)) {
            std::cerr << "RemoteServer: failed to encode response: " << error.reason
                      << std::endl;
        }
        abortSession(current, ErrorCode::Internal, "response encoding failed",
                     SessionEvent::Disconnect);
    }

    // --------------------------------------------------------
    // 会话收尾：Draining（contracts/protocol.md §3/§6/§7）
    // --------------------------------------------------------

    /// 进入 Draining：会话对象保留到清理完成，期间连接事件一律按占用处理。
    /// 握手失败不经过本函数（contracts/protocol.md §7）。
    void beginDraining(const std::shared_ptr<Session>& current)
    {
        if (session != current || current->state == State::Draining) {
            return;
        }
        cancelHandshakeTimer(current);
        cancelTimer(current->resumeTimer);
        // 会话已结束：接收缓冲区中未构成完整帧 / 未进入执行函数的命令全部丢弃（§6）。
        current->input.clear();
        current->state = State::Draining;
        if (stopping.load(std::memory_order_acquire)) {
            session.reset(); // 停止收尾期间不再回到监听态
            return;
        }
        scheduleDrainingCleanup(current);
    }

    /// 推迟清理：已排队的连接事件先按 Draining 判定为 BUSY，清理完成后才可接纳。
    void scheduleDrainingCleanup(const std::shared_ptr<Session>& current)
    {
        const hv::EventLoopPtr loop = server ? server->loop() : hv::EventLoopPtr{};
        if (!loop) {
            finishDraining(current);
            return;
        }
        const std::weak_ptr<Session> weak = current;
        current->cleanupTimer = loop->setTimeout(
            kDrainingCleanupDelayMs, [this, weak](hv::TimerID) { onDrainingCleanup(weak); });
    }

    void onDrainingCleanup(const std::weak_ptr<Session>& weak)
    {
        const auto current = weak.lock();
        if (!current) {
            return;
        }
        current->cleanupTimer = INVALID_TIMER_ID;
        finishDraining(current);
    }

    /// 清理完成：释放占用，状态回到 `Listening`（此后被处理的连接被接纳）。
    void finishDraining(const std::shared_ptr<Session>& current)
    {
        if (session != current || current->state != State::Draining) {
            return;
        }
        session.reset();
    }

    /// 失败路径：记 1 条日志、发送 ERROR 并关闭连接。
    /// 握手期失败立即回到 Listening（reject/error）；活动期失败进入 Draining（§7）。
    void abortSession(const std::shared_ptr<Session>& current, ErrorCode code,
                      std::string_view reason, SessionEvent event)
    {
        const bool wasStopping = stopping.load(std::memory_order_acquire);
        const bool handshake = current->state == State::Handshaking;
        cancelHandshakeTimer(current);
        if (handshake && session == current) {
            session.reset(); // 握手失败不进入 Draining
        }
        if (!wasStopping) {
            logSessionEvent(current->channel, event, LogResult::Error);
            writeFrame(current->channel, MessageType::Error, encodeErrorPayload(code, reason));
        }
        current->channel->close();
        if (!handshake) {
            beginDraining(current);
        }
    }

    /// 正常结束（契约 §7）：回 BYE 确认、记 1 条 disconnect/ok、关闭连接并进入 Draining。
    void completeBye(const std::shared_ptr<Session>& current)
    {
        const bool wasStopping = stopping.load(std::memory_order_acquire);
        cancelHandshakeTimer(current);
        if (!wasStopping) {
            writeFrame(current->channel, MessageType::Bye, {});
            logSessionEvent(current->channel, SessionEvent::Disconnect, LogResult::Ok);
        }
        current->channel->close();
        beginDraining(current);
    }

    // --------------------------------------------------------
    // 接收缓冲续行（contracts/protocol.md §6）
    // --------------------------------------------------------

    /// 缓冲中还有后续帧时推迟到下一轮事件循环：断开事件先被处理，
    /// 尚未进入执行函数的命令随后随会话结束被丢弃。
    void scheduleInputResume(const std::shared_ptr<Session>& current)
    {
        if (current->input.empty() || current->state != State::Active) {
            return;
        }
        const hv::EventLoopPtr loop = server ? server->loop() : hv::EventLoopPtr{};
        if (!loop) {
            return;
        }
        cancelTimer(current->resumeTimer);
        const std::weak_ptr<Session> weak = current;
        current->resumeTimer = loop->setTimeout(
            kInputResumeDelayMs, [this, weak](hv::TimerID) { onInputResume(weak); });
    }

    void onInputResume(const std::weak_ptr<Session>& weak)
    {
        const auto current = weak.lock();
        if (!current) {
            return;
        }
        current->resumeTimer = INVALID_TIMER_ID;
        if (stopping.load(std::memory_order_acquire) || session != current ||
            current->state != State::Active) {
            return; // 停止收尾或会话已结束（断开事件已处理）：缓冲中的命令不得执行
        }
        processInput(current);
    }

    // --------------------------------------------------------
    // 定时器辅助（毫秒精度，可被测试注入覆盖）
    // --------------------------------------------------------

    /// 取消定时器并清除句柄；重复调用无副作用。
    void cancelTimer(hv::TimerID& timer)
    {
        if (timer == INVALID_TIMER_ID) {
            return;
        }
        const hv::EventLoopPtr loop = server ? server->loop() : hv::EventLoopPtr{};
        if (loop) {
            loop->killTimer(timer);
        }
        timer = INVALID_TIMER_ID;
    }

    void startHandshakeTimer(const std::shared_ptr<Session>& current)
    {
        const hv::EventLoopPtr loop = server ? server->loop() : hv::EventLoopPtr{};
        if (!loop) {
            return;
        }
        int timeoutMs = static_cast<int>(handshakeTimeout.count());
        if (timeoutMs < 1) {
            timeoutMs = 1;
        }
        const std::weak_ptr<Session> weak = current;
        current->timer = loop->setTimer(
            timeoutMs, [this, weak](hv::TimerID) { onHandshakeTimeout(weak); }, 1);
    }

    void cancelHandshakeTimer(const std::shared_ptr<Session>& current)
    {
        cancelTimer(current->timer);
    }

    void onHandshakeTimeout(const std::weak_ptr<Session>& weak)
    {
        const auto current = weak.lock();
        if (!current || session != current || current->state != State::Handshaking) {
            return; // HELLO 已到达或会话已结束
        }
        current->timer = INVALID_TIMER_ID; // 一次性定时器已触发，无需再取消
        abortSession(current, ErrorCode::HandshakeTimeout, "handshake timeout",
                     SessionEvent::Reject);
    }

    // --------------------------------------------------------
    // 日志（contracts/logging.md §1–§4）：写入 Server 的标准控制台输出
    // --------------------------------------------------------

    void logSessionEvent(const hv::SocketChannelPtr& channel, SessionEvent event, LogResult result)
    {
        if (stopping.load(std::memory_order_acquire)) {
            return; // stop() 之后不写全局流
        }
        LogRecord record;
        record.utcTime = formatUtcTimestamp(std::chrono::system_clock::now());
        record.peer = channel ? channel->peeraddr() : std::string{};
        record.command = std::string{sessionEventCommand(event)};
        record.pid = std::nullopt;
        record.result = result;
        std::cout << formatLogLine(record) << std::endl;
    }
};

// ============================================================
// 公开接口
// ============================================================

RemoteServer::RemoteServer(std::uint16_t port, tpe::ProcessEngine& engine)
    : m_impl(std::make_unique<Impl>(port, engine))
{
}

RemoteServer::~RemoteServer()
{
    stop();
}

bool RemoteServer::start()
{
    return m_impl->startListening();
}

void RemoteServer::stop()
{
    m_impl->stopListening();
}

bool RemoteServer::isRunning() const
{
    return m_impl->running.load(std::memory_order_acquire);
}

void RemoteServer::setHandshakeTimeoutForTesting(std::chrono::milliseconds timeout)
{
    m_impl->handshakeTimeout = timeout;
}

} // namespace tpe::remote

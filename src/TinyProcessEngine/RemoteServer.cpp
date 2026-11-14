// 平台网络头必须最先包含：Windows 上 winsock2.h 必须先于任何 windows.h
// （项目 HelpFunction/Platform 头会间接包含 windows.h，否则与 winsock.h 冲突）。
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <hv/TcpServer.h>
#include <hv/hlog.h>

#include "RemoteServer.hpp"
#include "RemoteCommandExecutor.hpp"
#include "RemoteLog.hpp"
#include "RemoteProtocol.hpp"

// ProcessEngine.hpp 间接包含 Platform.hpp（Windows.h）；必须放在 libhv 的
// winsock2.h 之后，否则 winsock.h 与 winsock2.h 会重定义（C2375）。
#include "ProcessEngine.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

/// Server 开始监听时写入标准输出的固定文本。
constexpr std::string_view kWaitingText = "Waiting for connection ...";

/// Server 接纳 Client 时写入标准输出的固定文本。
constexpr std::string_view kAcceptedText = "Connection successful.";

/// RESPONSE 的线上提示符；不含尾随空格（尾随空格由 Client 渲染层添加，cli.md §5）。
constexpr std::string_view kPromptText = "TPE>";

/// 结果超过单条 RESPONSE 上限时的固定错误文本（protocol.md §5；cli.md §6）。
constexpr std::string_view kOversizeResponseText = "Response exceeds the maximum message size.";

// ============================================================
// 查询日志元数据（contracts/logging.md §2–§4）
// ============================================================

/// 查询日志的 command 字段（仅这两条命令产生查询日志）。
constexpr std::string_view kAllProcessesCommand = "--all-processes";
constexpr std::string_view kSearchProcessCommand = "--search-process";
constexpr std::string_view kSearchProcessPrefix = "--search-process ";

/// `--search-process` 未命中时的固定失败文本片段（RemoteCommandExecutor.cpp 的稳定
/// 输出）。仅用于判定日志摘要，不复制到日志行、不回显原因。
constexpr std::string_view kSearchNotFoundPrefix = "Process with PID ";
constexpr std::string_view kSearchNotFoundSuffix = " not found.";

/// 未命中固定文本：`Process with PID <pid> not found.`。
std::string searchNotFoundText(std::uint32_t pid)
{
    return std::string{kSearchNotFoundPrefix} + std::to_string(pid) +
           std::string{kSearchNotFoundSuffix};
}

/// 解析 `--search-process` 携带的 PID；规则与 RemoteCommandExecutor 一致：
/// 十进制、无符号、完整匹配、可落入平台 Pid_t 宽度。
/// 参数缺失或非法时返回 std::nullopt（日志 pid 字段渲染为 `-`）。
std::optional<std::uint32_t> parseSearchProcessPid(std::string_view line)
{
    if (line.size() <= kSearchProcessPrefix.size() ||
        line.compare(0, kSearchProcessPrefix.size(), kSearchProcessPrefix) != 0) {
        return std::nullopt; // 无参数（仅命令名）或不是该命令
    }
    const std::string_view argument = line.substr(kSearchProcessPrefix.size());
    if (argument.empty()) {
        return std::nullopt;
    }

    unsigned long long raw = 0;
    const char* const first = argument.data();
    const char* const last = first + argument.size();
    const std::from_chars_result converted = std::from_chars(first, last, raw, 10);
    if (converted.ec != std::errc{} || converted.ptr != last) {
        return std::nullopt; // 前导符号、空白、非十进制或尾随多余字符
    }

    // 与执行层相同的平台上限（Windows Pid_t = DWORD、Linux Pid_t = int）。
    constexpr unsigned long long kMaxPid = static_cast<unsigned long long>(
        (std::numeric_limits<tpe::platform::Pid_t>::max)());
    if (raw > kMaxPid) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(raw);
}

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

// ============================================================
// 绑定失败诊断（contracts/cli.md §8“端口被占用：具体原因” ）
// ============================================================

/// 去掉系统消息尾部的空白与句点：句点与错误码后缀由 describeSocketError 统一追加，
/// 避免末尾出现 "permitted.." 之类的重复标点。
std::string trimSystemMessage(std::string text)
{
    while (!text.empty()) {
        const char last = text.back();
        if (last == '.' || last == ' ' || last == '\t' || last == '\r' || last == '\n') {
            text.pop_back();
            continue;
        }
        break;
    }
    return text;
}

/// 把原生套接字错误码渲染为“系统文本 (error <code>)”。
/// Windows 用 `std::system_category()`：Winsock 错误码（如 10048 WSAEADDRINUSE）
/// 由系统消息表提供；POSIX 用 `std::generic_category()` 渲染 errno（如 EADDRINUSE）。
/// 始终附带十进制错误码，系统文本缺失或本地化时仍能识别原因。
std::string describeSocketError(int code)
{
#if defined(_WIN32)
    std::string text = trimSystemMessage(std::system_category().message(code));
#else
    std::string text = trimSystemMessage(std::generic_category().message(code));
#endif
    if (text.empty()) {
        return "socket error " + std::to_string(code);
    }
    return text + " (error " + std::to_string(code) + ")";
}

/// 对监听端口做一次原始 bind 探测，取回系统错误码；不 listen、不改变端口状态。
/// 仅在 `createsocket()` 已失败后调用，用与 libhv 相同的 0.0.0.0:<port> 绑定复现
/// 失败原因（端口占用、权限等），不设置 SO_REUSEADDR/SO_REUSEPORT，以保证与
/// 监听失败条件一致。返回空字符串表示探测成功（失败原因不是绑定错误），由调用方
/// 使用兜底原因。探测 socket 立即关闭，不引入线程与等待。
std::string probeBindFailure(std::uint16_t port)
{
#if defined(_WIN32)
    const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) {
        return describeSocketError(WSAGetLastError());
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const int code = WSAGetLastError();
        ::closesocket(handle);
        return describeSocketError(code);
    }
    ::closesocket(handle);
    return {};
#else
    const int handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle < 0) {
        return describeSocketError(errno);
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const int code = errno;
        ::close(handle);
        return describeSocketError(code);
    }
    ::close(handle);
    return {};
#endif
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
        std::unique_ptr<RemoteCommandExecutor> executor; // 会话独占命令执行器

        // 会话工作线程：事件循环只投递命令与接收完成事件；执行器与目标由该线程持有。
        std::thread worker;
        std::mutex workerMutex;
        std::mutex workerLifecycleMutex; // 串行化 stop/join
        std::condition_variable workerCv;
        std::deque<std::string> jobs;
        bool workerStop = false;
        bool workerBusy = false; // 仅事件循环线程读写
        std::atomic<bool> workerReleased{true};
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
    /// Draining 清理延迟；仅测试接缝覆盖（默认保持契约行为）。
    std::chrono::milliseconds drainingCleanupDelay{kDrainingCleanupDelayMs};

    // ---- 控制面：start()/stop() 调用线程访问，与事件循环线程通过原子量同步 ----
    mutable std::mutex controlMutex; // 串行化 start()/stop()/lastStartError()
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::unique_ptr<hv::TcpServer> server; // 事件循环运行期间保持同一对象
    /// 最近一次 start() 失败的具体原因；空表示尚未失败或最近一次启动成功。
    std::string lastStartError;

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
            // 失败诊断：对同一端口做一次原始 bind 取回系统错误码（不 listen、不阻塞）。
            // 探测意外成功时保留非空兜底原因，保证调用方总能输出具体原因。
            lastStartError = probeBindFailure(port);
            if (lastStartError.empty()) {
                lastStartError = "the listening socket could not be created";
            }
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
        lastStartError.clear();
        server->start(true); // 启动事件循环线程并注册 accept

        std::cout << kWaitingText << std::endl;
        return true;
    }

    /// lastStartError 的线程安全快照；与 start()/stop() 使用同一把锁。
    std::string startErrorSnapshot() const
    {
        const std::lock_guard<std::mutex> guard(controlMutex);
        return lastStartError;
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
        // 先停止并 join 事件循环，确保没有回调并发访问 session；随后再由
        // 控制线程 join 会话工作线程（worker 的 postEvent 只会入队、不再执行）。
        server->stop(true);
        if (session) {
            stopWorker(session);
        }
        server.reset();
        // 事件循环线程已退出，残留会话引用可安全清理。
        if (session && session->executor) {
            session->executor->release();
        }
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
        accepted->executor = std::make_unique<RemoteCommandExecutor>(engine);
        session = accepted;
        // 契约 logging.md §3：失败握手属于“接纳前握手失败”，只记 reject/error；
        // 因此 accept/ok 与 `Connection successful.` 推迟到 HELLO 校验通过后记录。
        startHandshakeTimer(accepted);
    }

    /// 每会话独占工作线程：执行器与目标只在该线程访问。
    void startWorker(const std::shared_ptr<Session>& current)
    {
        current->workerReleased.store(false, std::memory_order_release);
        const std::weak_ptr<Session> weak = current;
        current->worker = std::thread([this, weak]() { workerLoop(weak); });
    }

    void workerLoop(const std::weak_ptr<Session>& weak)
    {
        const auto current = weak.lock();
        if (!current) {
            return;
        }

        for (;;) {
            std::string line;
            {
                std::unique_lock<std::mutex> lock(current->workerMutex);
                current->workerCv.wait(lock, [&current]() {
                    return current->workerStop || !current->jobs.empty();
                });
                if (current->workerStop) {
                    current->jobs.clear();
                    break;
                }
                if (current->jobs.empty()) {
                    continue;
                }
                line = std::move(current->jobs.front());
                current->jobs.pop_front();
            }

            RemoteCommandResult result = current->executor->execute(line);
            const hv::EventLoopPtr loop = server ? server->loop() : hv::EventLoopPtr{};
            if (loop) {
                loop->postEvent([this, weak, result = std::move(result)](hv::Event*) mutable {
                    onWorkerResult(weak, std::move(result));
                });
            }

            const std::lock_guard<std::mutex> lock(current->workerMutex);
            if (current->workerStop) {
                break;
            }
        }

        if (current->executor) {
            current->executor->release();
        }
        current->workerReleased.store(true, std::memory_order_release);
    }

    void onWorkerResult(const std::weak_ptr<Session>& weak, RemoteCommandResult result)
    {
        const auto current = weak.lock();
        if (!current || session != current) {
            return;
        }
        current->workerBusy = false;
        if (current->state != State::Active || stopping.load(std::memory_order_acquire)) {
            return;
        }
        const ResponseDelivery delivery =
            sendResponse(current, result.status, result.output, result.prompt);
        logCommand(current->channel, result, delivery);
    }

    void stopWorker(const std::shared_ptr<Session>& current)
    {
        if (!current) {
            return;
        }
        const std::lock_guard<std::mutex> lifecycle(current->workerLifecycleMutex);
        {
            const std::lock_guard<std::mutex> lock(current->workerMutex);
            current->workerStop = true;
        }
        current->workerCv.notify_all();
        if (current->worker.joinable()) {
            current->worker.join();
        }
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
            stopWorker(closed);
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
    /// 一条 COMMAND 返回后不再继续解析同一接收事件中的后续帧（§6）：协议规定
    /// Client 每次只发送一条 COMMAND 并等待 RESPONSE，剩余字节一律视为“未开始”，
    /// 只由后续新的读取事件（新数据到达）处理；会话在此前结束则随 `input` 清空丢弃。
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
                // 不在本回调内续行：若在此继续执行缓冲中的下一条命令，任何事件循环
                // 延迟都可能让“续行”先于已排队的 close 事件，导致断开后仍执行未开始
                // 的命令（protocol.md §6 / FR-012）。剩余字节留给后续新的读取事件。
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
        startWorker(current);
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
        if (current->workerBusy) {
            // 客户端必须等待上一 RESPONSE；防御性忽略重复 COMMAND。
            return;
        }
        current->workerBusy = true;
        {
            const std::lock_guard<std::mutex> lock(current->workerMutex);
            if (current->workerStop) {
                current->workerBusy = false;
                return;
            }
            current->jobs.push_back(line);
        }
        current->workerCv.notify_one();
    }

    /// RESPONSE 发送结果；查询日志据此判定 result 字段（contracts/logging.md §3）。
    enum class ResponseDelivery {
        Sent,     // 按执行结果原样发送
        Oversize, // 结果超限：已改发固定 status=2 响应
        Failed,   // 编码内部错误：会话已按 ERROR(5) 关闭
    };

    /// 编码并发送 RESPONSE（contracts/protocol.md §5）：
    /// 结果超过单条响应上限时改发固定的 status=2 响应；不截断、不分片，会话保留。
    ResponseDelivery sendResponse(const std::shared_ptr<Session>& current, ResponseStatus status,
                                  const std::string& output, const std::string& prompt)
    {
        const Result<std::vector<std::uint8_t>, ProtocolError> encoded =
            encodeResponse(status, prompt, output);
        if (encoded.has_value()) {
            writeFrame(current->channel, MessageType::Response, encoded.value());
            return ResponseDelivery::Sent;
        }
        if (encoded.error().code == ErrorCode::FrameTooLarge) {
            return sendOversizeFallback(current, prompt);
        }
        failResponseEncoding(current, encoded.error());
        return ResponseDelivery::Failed;
    }

    /// 超限回退（protocol.md §5；cli.md §6）：status=2 + 固定文本，结果不截断、不分片。
    ResponseDelivery sendOversizeFallback(const std::shared_ptr<Session>& current,
                                          const std::string& prompt)
    {
        const Result<std::vector<std::uint8_t>, ProtocolError> encoded =
            encodeResponse(ResponseStatus::CommandError, prompt,
                           std::string(kOversizeResponseText));
        if (encoded.has_value()) {
            writeFrame(current->channel, MessageType::Response, encoded.value());
            return ResponseDelivery::Oversize;
        }
        failResponseEncoding(current, encoded.error());
        return ResponseDelivery::Failed;
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
        // 会话已结束：接收缓冲区中未构成完整帧 / 未进入执行函数的命令全部丢弃（§6）。
        current->input.clear();
        current->state = State::Draining;
        // 工作线程在完成当前命令后释放目标；清理完成前保持 Draining。
        stopWorker(current);
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
        int timeoutMs = static_cast<int>(drainingCleanupDelay.count());
        if (timeoutMs < 1) {
            timeoutMs = 1;
        }
        const std::weak_ptr<Session> weak = current;
        current->cleanupTimer = loop->setTimeout(
            timeoutMs, [this, weak](hv::TimerID) { onDrainingCleanup(weak); });
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
        if (current->executor) {
            current->executor->release();
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
            stopWorker(current);
            session.reset(); // 握手失败不进入 Draining
        }
        if (!wasStopping) {
            logSessionEvent(current->channel, event, LogResult::Error);
            writeFrame(current->channel, MessageType::Error, encodeErrorPayload(code, reason));
        }
        if (!handshake) {
            // 先进入 Draining 再 close：libhv 可能同步触发 onclose，
            // 若状态仍为 Active 会被误记为 disconnect/disconnected（重复日志）。
            beginDraining(current);
        }
        current->channel->close();
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
        // 先进入 Draining 再 close：同步 onclose 不得再记 disconnect/disconnected。
        beginDraining(current);
        current->channel->close();
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

    /// Phase 02 命令日志：使用执行器提供的元数据；失败响应不再追加第二条终止类记录。
    void logCommand(const hv::SocketChannelPtr& channel, const RemoteCommandResult& result,
                    ResponseDelivery delivery)
    {
        if (!result.loggable || delivery == ResponseDelivery::Failed ||
            stopping.load(std::memory_order_acquire)) {
            return;
        }
        LogRecord record;
        record.utcTime = formatUtcTimestamp(std::chrono::system_clock::now());
        record.peer = channel ? channel->peeraddr() : std::string{};
        record.command = result.logCommand;
        record.pid = result.logPid;
        record.result = delivery == ResponseDelivery::Oversize ? LogResult::Error
                                                              : result.logResult;
        std::cout << formatLogLine(record) << std::endl;
    }

    /// 查询日志（contracts/logging.md §2/§3）：COMMAND 执行并发送响应后记录；
    /// `help`、未知命令与 `exit` 不记录。日志只含元数据，不含原因文本、完整命令行
    /// 或进程名（§4）。
    void logQueryCommand(const hv::SocketChannelPtr& channel, RemoteCommandKind kind,
                         std::string_view line, const RemoteCommandResult& result,
                         ResponseDelivery delivery)
    {
        if (kind != RemoteCommandKind::AllProcesses &&
            kind != RemoteCommandKind::SearchProcess) {
            return; // help / 未知命令 / exit：不记录查询日志
        }
        if (delivery == ResponseDelivery::Failed) {
            // 响应编码内部错误已按 ERROR(5) + disconnect/error 关闭会话（不可达路径），
            // 不再追加查询日志，避免同一关闭产生第二条终止类记录。
            return;
        }
        if (stopping.load(std::memory_order_acquire)) {
            return; // stop() 之后不写全局流
        }

        const bool search = kind == RemoteCommandKind::SearchProcess;
        LogRecord record;
        record.utcTime = formatUtcTimestamp(std::chrono::system_clock::now());
        record.peer = channel ? channel->peeraddr() : std::string{};
        record.command = search ? std::string{kSearchProcessCommand}
                                : std::string{kAllProcessesCommand};
        // 仅 `--search-process` 携带有效 PID 时记录该 PID；参数缺失/非法用 `-`。
        record.pid = search ? parseSearchProcessPid(line) : std::nullopt;
        record.result = queryLogResult(kind, line, result, delivery);
        std::cout << formatLogLine(record) << std::endl;
    }

    /// 查询摘要（contracts/logging.md §3）：
    /// - `--all-processes`：正常响应 → ok；枚举失败/不可编码/超限 → error；
    /// - `--search-process`：参数缺失/非法 → error（pid=-）；命中 → ok；
    ///   固定未命中文本 → not-found；名称不可读/不可编码/超限 → error。
    static LogResult queryLogResult(RemoteCommandKind kind, std::string_view line,
                                    const RemoteCommandResult& result,
                                    ResponseDelivery delivery)
    {
        if (delivery == ResponseDelivery::Oversize) {
            return LogResult::Error; // 结果超限（§3：--all-processes 与 --search-process 均为 error）
        }
        if (kind == RemoteCommandKind::SearchProcess) {
            const std::optional<std::uint32_t> pid = parseSearchProcessPid(line);
            if (!pid.has_value()) {
                return LogResult::Error; // 参数缺失或非法：pid=-、error
            }
            if (result.status == ResponseStatus::Continue) {
                return LogResult::Ok;
            }
            if (result.output == searchNotFoundText(*pid)) {
                return LogResult::NotFound;
            }
            return LogResult::Error; // 进程名不可读/不可编码等
        }
        return result.status == ResponseStatus::Continue ? LogResult::Ok : LogResult::Error;
    }

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

std::string RemoteServer::lastStartError() const
{
    return m_impl->startErrorSnapshot();
}

void RemoteServer::setHandshakeTimeoutForTesting(std::chrono::milliseconds timeout)
{
    m_impl->handshakeTimeout = timeout;
}

void RemoteServer::setDrainingCleanupDelayForTesting(std::chrono::milliseconds delay)
{
    m_impl->drainingCleanupDelay = delay;
}

} // namespace tpe::remote

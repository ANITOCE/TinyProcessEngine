#include "startup_remote.hpp"

#include "ProcessEngine.hpp"
#include "RemoteClient.hpp"
#include "RemoteServer.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace tpe::app {

namespace {

/// Server 省略 -port 时的默认端口（contracts/cli.md §1）。
constexpr std::uint16_t kDefaultServerPort = 8008;

/// 端口合法区间（contracts/cli.md §1）。
constexpr unsigned int kMinimumPort = 1;
constexpr unsigned int kMaximumPort = 65535;

/// 远程用法文本(contracts/cli.md §1;参数错误时写标准输出)。
constexpr std::string_view kRemoteUsageText =
    "Usage:\n"
    "  TinyProcessEngine.exe --server [-port <PORT>]\n"
    "  TinyProcessEngine.exe --connect <IP> -port <PORT>\n";

/// 默认远程入口:解析参数后启动 Server 或 Client(contracts/cli.md §1/§2/§8)。
/// 返回 0 正常结束、1 绑定/连接失败、2 参数用法错误。
int defaultRemoteEntry(const std::vector<std::string>& args)
{
    const auto parsed = parseRemoteStartupArgs(args);
    if (!parsed.has_value()) {
        std::cerr << parsed.error() << std::endl; // 一行原因(标准错误)
        std::cout << kRemoteUsageText;            // 用法(标准输出)
        return 2;
    }

    const RemoteStartupOptions& options = parsed.value();
    if (options.mode == RemoteMode::Server) {
        tpe::ProcessEngine engine;
        tpe::remote::RemoteServer server(options.port, engine);
        if (!server.start()) {
            // 绑定失败:标准错误写 "Failed to listen on port <PORT>: <系统原因>",
            // 退出码 1(contracts/cli.md §8);监听提示只由成功路径输出。
            std::string reason = server.lastStartError();
            if (reason.empty()) {
                reason = "the listening socket could not be created";
            }
            std::cerr << "Failed to listen on port " << options.port << ": " << reason << "."
                      << std::endl;
            return 1;
        }
        // Phase 01 无内部停止入口:主线程持续运行,由外部终止进程。
        while (server.isRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return 0;
    }

    tpe::remote::RemoteClient client(options.host, options.port);
    return client.run();
}

/// 当前生效的远程入口;测试构建可经 setRemoteEntryForTesting 替换。
RemoteEntryFn& remoteEntry()
{
    static RemoteEntryFn entry = &defaultRemoteEntry;
    return entry;
}

/// 文本是否全为十进制数字（空串不算）。
bool isDecimalDigits(std::string_view text)
{
    if (text.empty()) {
        return false;
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

/// 端口：纯十进制数字且落在 1–65535；不依赖异常与 locale。
bool parsePort(std::string_view text, std::uint16_t& port)
{
    if (!isDecimalDigits(text)) {
        return false;
    }
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    unsigned int value = 0;
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc() || parsed.ptr != end) {
        return false; // 溢出（超长数字串）或未消费完整取值
    }
    if (value < kMinimumPort || value > kMaximumPort) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

/// 单个 IPv4 段：1–3 位十进制数字、取值 0–255。
/// 拒绝前导零（与 inet_pton 一致，避免 "010" 这类歧义写法）。
bool isIpv4Octet(std::string_view text)
{
    if (text.empty() || text.size() > 3 || !isDecimalDigits(text)) {
        return false;
    }
    if (text.size() > 1 && text.front() == '0') {
        return false;
    }
    unsigned int value = 0;
    for (const char ch : text) {
        value = value * 10U + static_cast<unsigned int>(ch - '0');
    }
    return value <= 255U;
}

/// IPv4 点分十进制字面量：恰好四段、以 '.' 分隔；
/// 主机名、域名与缩写形式（如 "127.1"）一律拒绝。
bool isIpv4Literal(std::string_view text)
{
    int segments = 0;
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index == text.size() || text[index] == '.') {
            if (!isIpv4Octet(text.substr(begin, index - begin))) {
                return false;
            }
            ++segments;
            begin = index + 1;
        }
    }
    return segments == 4;
}

/// 参数冲突/重复的出错文本（不打印，由调用方写入标准错误）。
constexpr const char* kMutualExclusionError = "--server and --connect cannot be combined.";

} // namespace

Result<RemoteStartupOptions, std::string>
parseRemoteStartupArgs(const std::vector<std::string>& args)
{
    using ParseResult = Result<RemoteStartupOptions, std::string>;

    RemoteStartupOptions options; // mode 默认 Server，由 hasRole 区分"未指定"与"已指定"
    bool hasRole = false;
    bool hasPort = false;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];

        if (arg == "--server") {
            if (hasRole && options.mode == RemoteMode::Server) {
                return ParseResult::error("Duplicate --server.");
            }
            if (hasRole && options.mode == RemoteMode::Client) {
                return ParseResult::error(kMutualExclusionError);
            }
            options.mode = RemoteMode::Server;
            hasRole = true;
            continue;
        }

        if (arg == "--connect") {
            if (hasRole && options.mode == RemoteMode::Client) {
                return ParseResult::error("Duplicate --connect.");
            }
            if (hasRole && options.mode == RemoteMode::Server) {
                return ParseResult::error(kMutualExclusionError);
            }
            // 地址必须是紧随其后的独立取值；以 '-' 开头说明用户漏写了地址。
            if (index + 1 >= args.size() ||
                (!args[index + 1].empty() && args[index + 1].front() == '-')) {
                return ParseResult::error("Missing IPv4 address for --connect.");
            }
            const std::string& address = args[index + 1];
            if (!isIpv4Literal(address)) {
                return ParseResult::error("Invalid IPv4 address: " + address +
                                          " (expected a dotted-decimal literal).");
            }
            options.host = address;
            options.mode = RemoteMode::Client;
            hasRole = true;
            ++index; // 已消费地址取值
            continue;
        }

        if (arg == "-port") {
            if (hasPort) {
                return ParseResult::error("Duplicate -port.");
            }
            if (index + 1 >= args.size()) {
                return ParseResult::error("Missing port value for -port.");
            }
            const std::string& text = args[index + 1];
            std::uint16_t port = 0;
            if (!parsePort(text, port)) {
                return ParseResult::error("Invalid port: " + text +
                                          " (expected a decimal integer in 1-65535).");
            }
            options.port = port;
            hasPort = true;
            ++index; // 已消费端口取值
            continue;
        }

        return ParseResult::error("Unknown argument: " + arg);
    }

    if (!hasRole) {
        return ParseResult::error("Missing remote mode: specify --server or --connect.");
    }
    if (options.mode == RemoteMode::Client && !hasPort) {
        return ParseResult::error("Missing -port for --connect.");
    }
    if (options.mode == RemoteMode::Server && !hasPort) {
        options.port = kDefaultServerPort;
    }
    return ParseResult::success(options);
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

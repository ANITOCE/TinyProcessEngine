#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Result.hpp"

namespace tpe::remote {

// ============================================================
// 协议常量（contracts/protocol.md §1）
// ============================================================

/// 当前协议版本；帧版本字节与此不符时返回 ErrorCode::UnsupportedVersion。
inline constexpr std::uint8_t kProtocolVersion = 1;

/// 帧 length 字段上限（1 MiB，不包含 length 字段自身的 4 字节）。
inline constexpr std::uint32_t kMaxFrameLength = 1u << 20;

/// COMMAND 单行命令 payload 上限（64 KiB）。
inline constexpr std::size_t kMaxCommandBytes = 64u * 1024u;

/// length 字段本身的字节数；帧总字节数 = kFrameLengthBytes + length。
inline constexpr std::size_t kFrameLengthBytes = 4;

/// length 字段下限（version + type 各 1 字节，无 payload）。
inline constexpr std::uint32_t kMinFrameLength = 2;

/// payload 字节数上限（kMaxFrameLength 减去 version 与 type）。
inline constexpr std::uint32_t kMaxPayloadBytes = kMaxFrameLength - 2;

/// HELLO 中 clientId 的字节数上限。
inline constexpr std::size_t kMaxClientIdBytes = 64;

/// RESPONSE 中 promptLen 可表示的上限（uint16）。
inline constexpr std::size_t kMaxPromptBytes = 0xFFFFu;

// ============================================================
// 消息类型与错误码（contracts/protocol.md §2；data-model.md §2）
// ============================================================

/// 帧 type 字节；未列出的值按 ErrorCode::BadMessageType 处理。
enum class MessageType : std::uint8_t {
    Hello = 1,    // C→S：clientId（UTF-8，≤ kMaxClientIdBytes）
    Welcome = 2,  // S→C：空载荷
    Busy = 3,     // S→C：固定文本 "Server is busy."
    Error = 4,    // S→C：code(uint8) + reason(UTF-8)
    Command = 5,  // C→S：单行命令（UTF-8，≤ kMaxCommandBytes，不含 CR/LF）
    Response = 6, // S→C：status(uint8) + promptLen(uint16 BE) + prompt
                  //      + outputLen(uint32 BE) + output
    Bye = 7,      // 双向：空载荷，正常结束会话
};

/// ERROR 载荷首字节的错误码。
enum class ErrorCode : std::uint8_t {
    UnsupportedVersion = 1, // 不支持的协议版本
    BadMessageType = 2,     // 非法消息类型
    FrameTooLarge = 3,      // 帧超限
    HandshakeTimeout = 4,   // 握手期限到期
    Internal = 5,           // 内部错误
    InvalidPayload = 6,     // 已知消息类型的载荷非法
};

/// RESPONSE 载荷首字节的状态码；Phase 01 不使用 Exit。
enum class ResponseStatus : std::uint8_t {
    Continue = 0,     // 继续交互
    Exit = 1,         // 保留：Phase 01 不使用
    CommandError = 2, // 命令层错误，会话保留
};

// ============================================================
// 帧与解码结果（data-model.md §1）
// ============================================================

/// 已解出的帧头；length 不含自身 4 字节，帧总字节数 = 4 + length。
struct FrameHeader {
    std::uint32_t length = 0;
    std::uint8_t version = 0;
    MessageType type = MessageType::Hello;
};

/// 一个完整帧：帧头 + payload（payload 长度 = header.length - kMinFrameLength）。
struct Frame {
    FrameHeader header;
    std::vector<std::uint8_t> payload;
};

/// 协议层错误：错误码 + 人类可读原因。
struct ProtocolError {
    ErrorCode code = ErrorCode::Internal;
    std::string reason;
};

/// tryDecodeFrame 的结果状态。
enum class DecodeStatus {
    Complete,     // 已得到完整帧
    NeedMoreData, // 输入不足以构成完整帧，等待更多字节
    Failed,       // 协议错误；此时 error 有效
};

/// tryDecodeFrame 的结果。
struct DecodeResult {
    DecodeStatus status = DecodeStatus::NeedMoreData;
    Frame frame;              // status == Complete 时有效
    ProtocolError error;      // status == Failed 时有效
    std::size_t consumed = 0; // status == Complete 时有效：本帧占用的输入字节数
};

/// RESPONSE 载荷的解码结果（data-model.md §4）。
struct RemoteResponse {
    ResponseStatus status = ResponseStatus::Continue;
    std::string prompt; // 线上值不含尾随空格（尾随空格由渲染层添加）
    std::string output; // 可为空；多行以 LF（'\n'）分隔，不含提示符
};

// ============================================================
// 编解码接口（骨架：行为由后续任务实现）
// ============================================================

/// 编码一帧：length(4, BE) + version + type + payload；length = kMinFrameLength + payload.size()。
/// 调用方保证 payload.size() ≤ kMaxPayloadBytes（超限属于响应构造错误）。
std::vector<std::uint8_t> encodeFrame(MessageType type, std::vector<std::uint8_t> payload);

/// 从输入前部尝试解码一帧；不消费输入，完整帧时由 consumed 报告帧长。
/// 输入不足以构成完整帧时返回 NeedMoreData。
/// 帧长度非法返回 Failed(FrameTooLarge)，版本非法返回 Failed(UnsupportedVersion)，
/// 类型非法返回 Failed(BadMessageType)。
DecodeResult tryDecodeFrame(const std::uint8_t* data, std::size_t size);

/// tryDecodeFrame 的 vector 便捷重载。
DecodeResult tryDecodeFrame(const std::vector<std::uint8_t>& buffer);

/// 校验已知消息类型的载荷（例如载荷非空的 BYE、不可解码的 HELLO → InvalidPayload）；
/// 载荷合法返回 std::nullopt。
std::optional<ProtocolError> validatePayload(MessageType type,
                                             const std::vector<std::uint8_t>& payload);

/// 编码 RESPONSE 载荷：status(1) + promptLen(2, BE) + prompt
///                            + outputLen(4, BE) + output。
/// prompt 超过 kMaxPromptBytes 或整体超过 kMaxPayloadBytes 时返回 ProtocolError。
Result<std::vector<std::uint8_t>, ProtocolError> encodeResponse(ResponseStatus status,
                                                                const std::string& prompt,
                                                                const std::string& output);

/// 解码 RESPONSE 载荷；结构非法返回 ProtocolError。
Result<RemoteResponse, ProtocolError> decodeResponse(const std::vector<std::uint8_t>& payload);

} // namespace tpe::remote

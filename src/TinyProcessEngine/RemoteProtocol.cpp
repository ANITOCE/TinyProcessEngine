#include "RemoteProtocol.hpp"

#include <string_view>
#include <utility>

namespace tpe::remote {
namespace {

// ------------------------------------------------------------
// 字节序辅助（大端）
// ------------------------------------------------------------

std::uint16_t readBigEndian16(const std::uint8_t* data)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                      static_cast<std::uint16_t>(data[1]));
}

std::uint32_t readBigEndian32(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

void appendBigEndian16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void appendBigEndian32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

// ------------------------------------------------------------
// 载荷辅助
// ------------------------------------------------------------

/// BUSY 的固定文本。
constexpr std::string_view kBusyText = "Server is busy.";

/// ERROR 载荷允许的错误码范围 [1, 6]。
constexpr std::uint8_t kMinErrorCode = static_cast<std::uint8_t>(ErrorCode::UnsupportedVersion);
constexpr std::uint8_t kMaxErrorCode = static_cast<std::uint8_t>(ErrorCode::InvalidPayload);

/// 帧 type 字节的合法范围（MessageType::Hello..MessageType::Bye）。
constexpr std::uint8_t kMinMessageType = static_cast<std::uint8_t>(MessageType::Hello);
constexpr std::uint8_t kMaxMessageType = static_cast<std::uint8_t>(MessageType::Bye);

bool isContinuationByte(std::uint8_t byte)
{
    return (byte & 0xC0u) == 0x80u;
}

/// 严格 UTF-8 校验（不依赖 locale）：拒绝过长编码、代理区码点与超出 U+10FFFF 的序列。
bool isValidUtf8(const std::uint8_t* data, std::size_t size)
{
    std::size_t index = 0;
    while (index < size) {
        const std::uint8_t lead = data[index];
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
        if (data[index + 1] < secondMin || data[index + 1] > secondMax) {
            return false;
        }
        for (std::size_t offset = 2; offset <= trailing; ++offset) {
            if (!isContinuationByte(data[index + offset])) {
                return false;
            }
        }
        index += trailing + 1;
    }
    return true;
}

bool isValidUtf8(const std::vector<std::uint8_t>& payload)
{
    return isValidUtf8(payload.data(), payload.size());
}

bool equalsText(const std::vector<std::uint8_t>& payload, std::string_view text)
{
    if (payload.size() != text.size()) {
        return false;
    }
    for (std::size_t index = 0; index < payload.size(); ++index) {
        if (payload[index] != static_cast<std::uint8_t>(text[index])) {
            return false;
        }
    }
    return true;
}

bool containsLineBreak(const std::vector<std::uint8_t>& payload)
{
    for (const std::uint8_t byte : payload) {
        if (byte == static_cast<std::uint8_t>('\r') || byte == static_cast<std::uint8_t>('\n')) {
            return true;
        }
    }
    return false;
}

ProtocolError makeError(ErrorCode code, std::string reason)
{
    return ProtocolError{code, std::move(reason)};
}

} // namespace

// ============================================================
// 帧编码 / 解码
// ============================================================

std::vector<std::uint8_t> encodeFrame(MessageType type, std::vector<std::uint8_t> payload)
{
    const std::uint32_t length = static_cast<std::uint32_t>(kMinFrameLength + payload.size());

    std::vector<std::uint8_t> frame;
    frame.reserve(kFrameLengthBytes + static_cast<std::size_t>(length));
    appendBigEndian32(frame, length);
    frame.push_back(kProtocolVersion);
    frame.push_back(static_cast<std::uint8_t>(type));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

DecodeResult tryDecodeFrame(const std::uint8_t* data, std::size_t size)
{
    DecodeResult result; // 默认 NeedMoreData，consumed = 0

    if (data == nullptr) {
        if (size == 0) {
            return result;
        }
        result.status = DecodeStatus::Failed;
        result.error = makeError(ErrorCode::Internal, "frame buffer is null");
        return result;
    }

    // length 字段不足 4 字节：等待更多数据。
    if (size < kFrameLengthBytes) {
        return result;
    }

    // length 字段一旦到齐就判定范围，不等待 payload。
    const std::uint32_t length = readBigEndian32(data);
    if (length < kMinFrameLength || length > kMaxFrameLength) {
        result.status = DecodeStatus::Failed;
        result.error = makeError(ErrorCode::FrameTooLarge, "frame length out of range");
        return result;
    }

    // 帧头与 payload 未全部到齐。
    if (size - kFrameLengthBytes < static_cast<std::size_t>(length)) {
        return result;
    }

    const std::uint8_t version = data[kFrameLengthBytes];
    if (version != kProtocolVersion) {
        result.status = DecodeStatus::Failed;
        result.error = makeError(ErrorCode::UnsupportedVersion, "unsupported protocol version");
        return result;
    }

    const std::uint8_t typeByte = data[kFrameLengthBytes + 1];
    if (typeByte < kMinMessageType || typeByte > kMaxMessageType) {
        result.status = DecodeStatus::Failed;
        result.error = makeError(ErrorCode::BadMessageType, "unknown message type");
        return result;
    }

    const std::uint8_t* const payloadBegin = data + kFrameLengthBytes + kMinFrameLength;
    const std::uint8_t* const payloadEnd = data + kFrameLengthBytes + length;

    result.status = DecodeStatus::Complete;
    result.frame.header.length = length;
    result.frame.header.version = version;
    result.frame.header.type = static_cast<MessageType>(typeByte);
    result.frame.payload.assign(payloadBegin, payloadEnd);
    result.consumed = static_cast<std::size_t>(kFrameLengthBytes) + static_cast<std::size_t>(length);
    return result;
}

DecodeResult tryDecodeFrame(const std::vector<std::uint8_t>& buffer)
{
    return tryDecodeFrame(buffer.data(), buffer.size());
}

// ============================================================
// 载荷校验
// ============================================================

std::optional<ProtocolError> validatePayload(MessageType type,
                                             const std::vector<std::uint8_t>& payload)
{
    switch (type) {
    case MessageType::Hello:
        if (payload.size() > kMaxClientIdBytes) {
            return makeError(ErrorCode::InvalidPayload, "hello client id exceeds 64 bytes");
        }
        if (!isValidUtf8(payload)) {
            return makeError(ErrorCode::InvalidPayload, "hello client id is not valid UTF-8");
        }
        return std::nullopt;

    case MessageType::Welcome:
    case MessageType::Bye:
        if (!payload.empty()) {
            return makeError(ErrorCode::InvalidPayload, "payload must be empty");
        }
        return std::nullopt;

    case MessageType::Busy:
        if (!equalsText(payload, kBusyText)) {
            return makeError(ErrorCode::InvalidPayload, "busy payload does not match fixed text");
        }
        return std::nullopt;

    case MessageType::Error: {
        if (payload.empty()) {
            return makeError(ErrorCode::InvalidPayload, "error payload is missing the code byte");
        }
        const std::uint8_t code = payload[0];
        if (code < kMinErrorCode || code > kMaxErrorCode) {
            return makeError(ErrorCode::InvalidPayload, "error code out of range");
        }
        const std::uint8_t* const reasonBegin = payload.data() + 1;
        const std::size_t reasonSize = payload.size() - 1;
        if (!isValidUtf8(reasonBegin, reasonSize)) {
            return makeError(ErrorCode::InvalidPayload, "error reason is not valid UTF-8");
        }
        return std::nullopt;
    }

    case MessageType::Command:
        if (payload.size() > kMaxCommandBytes) {
            return makeError(ErrorCode::InvalidPayload, "command exceeds 64 KiB");
        }
        if (containsLineBreak(payload)) {
            return makeError(ErrorCode::InvalidPayload, "command contains CR or LF");
        }
        if (!isValidUtf8(payload)) {
            return makeError(ErrorCode::InvalidPayload, "command is not valid UTF-8");
        }
        return std::nullopt;

    case MessageType::Response: {
        const auto decoded = decodeResponse(payload);
        if (!decoded) {
            return makeError(ErrorCode::InvalidPayload, "response payload is malformed");
        }
        return std::nullopt;
    }
    }

    return makeError(ErrorCode::BadMessageType, "unknown message type");
}

// ============================================================
// RESPONSE 编解码
// ============================================================

Result<std::vector<std::uint8_t>, ProtocolError> encodeResponse(ResponseStatus status,
                                                                const std::string& prompt,
                                                                const std::string& output)
{
    // 线上 prompt 不含尾随空格（尾随空格由渲染层添加）。
    std::size_t promptSize = prompt.size();
    while (promptSize > 0 && prompt[promptSize - 1] == ' ') {
        --promptSize;
    }

    if (promptSize > kMaxPromptBytes) {
        return Result<std::vector<std::uint8_t>, ProtocolError>::error(
            makeError(ErrorCode::InvalidPayload, "response prompt exceeds uint16 length limit"));
    }

    const std::size_t overhead = 1 + 2 + promptSize + 4; // status + promptLen + prompt + outputLen
    if (output.size() > static_cast<std::size_t>(kMaxPayloadBytes) - overhead) {
        return Result<std::vector<std::uint8_t>, ProtocolError>::error(
            makeError(ErrorCode::FrameTooLarge, "response payload exceeds frame payload limit"));
    }

    std::vector<std::uint8_t> encoded;
    encoded.reserve(overhead + output.size());
    encoded.push_back(static_cast<std::uint8_t>(status));
    appendBigEndian16(encoded, static_cast<std::uint16_t>(promptSize));
    encoded.insert(encoded.end(), prompt.begin(), prompt.begin() + static_cast<std::ptrdiff_t>(promptSize));
    appendBigEndian32(encoded, static_cast<std::uint32_t>(output.size()));
    encoded.insert(encoded.end(), output.begin(), output.end());

    return Result<std::vector<std::uint8_t>, ProtocolError>::success(std::move(encoded));
}

Result<RemoteResponse, ProtocolError> decodeResponse(const std::vector<std::uint8_t>& payload)
{
    const std::uint8_t* const data = payload.data();
    const std::size_t size = payload.size();
    std::size_t offset = 0;

    // status(1)
    if (size - offset < 1) {
        return Result<RemoteResponse, ProtocolError>::error(
            makeError(ErrorCode::InvalidPayload, "response payload is empty"));
    }
    const std::uint8_t statusByte = data[offset];
    ++offset;
    if (statusByte > static_cast<std::uint8_t>(ResponseStatus::CommandError)) {
        return Result<RemoteResponse, ProtocolError>::error(
            makeError(ErrorCode::InvalidPayload, "response status out of range"));
    }

    // promptLen(2, BE) + prompt
    if (size - offset < 2) {
        return Result<RemoteResponse, ProtocolError>::error(
            makeError(ErrorCode::InvalidPayload, "response payload is missing promptLen"));
    }
    const std::size_t promptSize = readBigEndian16(data + offset);
    offset += 2;
    if (size - offset < promptSize) {
        return Result<RemoteResponse, ProtocolError>::error(
            makeError(ErrorCode::InvalidPayload, "response prompt is truncated"));
    }
    RemoteResponse response;
    response.status = static_cast<ResponseStatus>(statusByte);
    response.prompt.assign(reinterpret_cast<const char*>(data + offset), promptSize);
    offset += promptSize;

    // outputLen(4, BE) + output
    if (size - offset < 4) {
        return Result<RemoteResponse, ProtocolError>::error(
            makeError(ErrorCode::InvalidPayload, "response payload is missing outputLen"));
    }
    const std::uint32_t outputSize = readBigEndian32(data + offset);
    offset += 4;
    if (size - offset < static_cast<std::size_t>(outputSize)) {
        return Result<RemoteResponse, ProtocolError>::error(
            makeError(ErrorCode::InvalidPayload, "response output is truncated"));
    }
    response.output.assign(reinterpret_cast<const char*>(data + offset),
                           static_cast<std::size_t>(outputSize));
    offset += static_cast<std::size_t>(outputSize);

    if (offset != size) {
        return Result<RemoteResponse, ProtocolError>::error(
            makeError(ErrorCode::InvalidPayload, "response payload has trailing bytes"));
    }

    return Result<RemoteResponse, ProtocolError>::success(std::move(response));
}

} // namespace tpe::remote

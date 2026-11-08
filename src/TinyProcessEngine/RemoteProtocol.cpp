#include "RemoteProtocol.hpp"

namespace tpe::remote {

// 骨架实现：仅提供可编译的占位，帧编解码行为由后续任务实现。

std::vector<std::uint8_t> encodeFrame(MessageType type, std::vector<std::uint8_t> payload)
{
    (void)type;
    (void)payload;
    return {};
}

DecodeResult tryDecodeFrame(const std::uint8_t* data, std::size_t size)
{
    (void)data;
    (void)size;
    return DecodeResult{};
}

DecodeResult tryDecodeFrame(const std::vector<std::uint8_t>& buffer)
{
    (void)buffer;
    return DecodeResult{};
}

std::optional<ProtocolError> validatePayload(MessageType type,
                                             const std::vector<std::uint8_t>& payload)
{
    (void)type;
    (void)payload;
    return ProtocolError{ErrorCode::Internal, "validatePayload: not implemented"};
}

Result<std::vector<std::uint8_t>, ProtocolError> encodeResponse(ResponseStatus status,
                                                                const std::string& prompt,
                                                                const std::string& output)
{
    (void)status;
    (void)prompt;
    (void)output;
    return Result<std::vector<std::uint8_t>, ProtocolError>::error(
        ProtocolError{ErrorCode::Internal, "encodeResponse: not implemented"});
}

Result<RemoteResponse, ProtocolError> decodeResponse(const std::vector<std::uint8_t>& payload)
{
    (void)payload;
    return Result<RemoteResponse, ProtocolError>::error(
        ProtocolError{ErrorCode::Internal, "decodeResponse: not implemented"});
}

} // namespace tpe::remote

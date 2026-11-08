// T009：远程协议层契约测试（TDD RED，T014 实现后转绿）。
// 依据：specs/001-remote-connection-phase-01/contracts/protocol.md §1/§2/§4、
//       specs/001-remote-connection-phase-01/data-model.md §1/§2/§4、
//       include/TinyProcessEngine/RemoteProtocol.hpp。
// 约束：只用 GoogleTest；不访问网络、不启动进程、不依赖时钟或文件系统。

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "RemoteProtocol.hpp"
#include "Result.hpp"

using tpe::remote::DecodeResult;
using tpe::remote::DecodeStatus;
using tpe::remote::ErrorCode;
using tpe::remote::MessageType;
using tpe::remote::RemoteResponse;
using tpe::remote::ResponseStatus;

namespace {

std::vector<std::uint8_t> bytes(std::initializer_list<unsigned> values)
{
    std::vector<std::uint8_t> out;
    out.reserve(values.size());
    for (const unsigned value : values) {
        out.push_back(static_cast<std::uint8_t>(value));
    }
    return out;
}

std::vector<std::uint8_t> ascii(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

/// 按契约 §1 手工拼帧头：length(4, BE) + version + type（不含 payload）。
std::vector<std::uint8_t> frameHeader(std::uint32_t length, std::uint8_t version, std::uint8_t type)
{
    return bytes({static_cast<unsigned>((length >> 24) & 0xFFu),
                  static_cast<unsigned>((length >> 16) & 0xFFu),
                  static_cast<unsigned>((length >> 8) & 0xFFu),
                  static_cast<unsigned>(length & 0xFFu),
                  static_cast<unsigned>(version),
                  static_cast<unsigned>(type)});
}

std::string toText(const std::vector<std::uint8_t>& data)
{
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

} // namespace

// ---------------------------------------------------------------------------
// 协议常量（contracts/protocol.md §1/§2；data-model.md §1/§2）
// ---------------------------------------------------------------------------

TEST(RemoteProtocol, ConstantsMatchContract)
{
    EXPECT_EQ(static_cast<int>(tpe::remote::kProtocolVersion), 1);
    EXPECT_EQ(tpe::remote::kFrameLengthBytes, std::size_t{4});
    EXPECT_EQ(tpe::remote::kMinFrameLength, std::uint32_t{2});
    EXPECT_EQ(tpe::remote::kMaxFrameLength, std::uint32_t{1} << 20);
    EXPECT_EQ(tpe::remote::kMaxPayloadBytes, tpe::remote::kMaxFrameLength - 2);
    EXPECT_EQ(tpe::remote::kMaxCommandBytes, std::size_t{64} * 1024);
    EXPECT_EQ(tpe::remote::kMaxClientIdBytes, std::size_t{64});
    EXPECT_EQ(tpe::remote::kMaxPromptBytes, std::size_t{0xFFFF});

    EXPECT_EQ(static_cast<int>(MessageType::Hello), 1);
    EXPECT_EQ(static_cast<int>(MessageType::Welcome), 2);
    EXPECT_EQ(static_cast<int>(MessageType::Busy), 3);
    EXPECT_EQ(static_cast<int>(MessageType::Error), 4);
    EXPECT_EQ(static_cast<int>(MessageType::Command), 5);
    EXPECT_EQ(static_cast<int>(MessageType::Response), 6);
    EXPECT_EQ(static_cast<int>(MessageType::Bye), 7);

    EXPECT_EQ(static_cast<int>(ErrorCode::UnsupportedVersion), 1);
    EXPECT_EQ(static_cast<int>(ErrorCode::BadMessageType), 2);
    EXPECT_EQ(static_cast<int>(ErrorCode::FrameTooLarge), 3);
    EXPECT_EQ(static_cast<int>(ErrorCode::HandshakeTimeout), 4);
    EXPECT_EQ(static_cast<int>(ErrorCode::Internal), 5);
    EXPECT_EQ(static_cast<int>(ErrorCode::InvalidPayload), 6);

    EXPECT_EQ(static_cast<int>(ResponseStatus::Continue), 0);
    EXPECT_EQ(static_cast<int>(ResponseStatus::Exit), 1);
    EXPECT_EQ(static_cast<int>(ResponseStatus::CommandError), 2);
}

// ---------------------------------------------------------------------------
// 帧编码（contracts/protocol.md §1）
// ---------------------------------------------------------------------------

TEST(RemoteProtocol, EncodeFrameWritesLengthVersionTypeAndPayload)
{
    const std::vector<std::uint8_t> payload = ascii("TPE");
    const std::vector<std::uint8_t> encoded =
        tpe::remote::encodeFrame(MessageType::Hello, payload);

    // length = version(1) + type(1) + payload(3) = 5；帧总字节数 = 4 + length = 9。
    ASSERT_EQ(encoded.size(), std::size_t{4} + 2 + payload.size());
    EXPECT_EQ(encoded, bytes({0x00, 0x00, 0x00, 0x05, // length = 5，大端
                              0x01,                   // version = 1
                              0x01,                   // type = HELLO
                              'T', 'P', 'E'}));       // payload 原样
}

// ---------------------------------------------------------------------------
// 帧解码：完整帧与消费长度（contracts/protocol.md §1）
// ---------------------------------------------------------------------------

TEST(RemoteProtocol, TryDecodeFrameReturnsCompleteFrame)
{
    std::vector<std::uint8_t> buffer = frameHeader(5, 1, static_cast<std::uint8_t>(MessageType::Hello));
    const std::vector<std::uint8_t> payload = ascii("TPE");
    buffer.insert(buffer.end(), payload.begin(), payload.end());

    const DecodeResult result = tpe::remote::tryDecodeFrame(buffer);

    ASSERT_EQ(result.status, DecodeStatus::Complete);
    EXPECT_EQ(result.consumed, std::size_t{9});
    EXPECT_EQ(result.frame.header.length, std::uint32_t{5});
    EXPECT_EQ(result.frame.header.version, std::uint8_t{1});
    EXPECT_EQ(result.frame.header.type, MessageType::Hello);
    EXPECT_EQ(result.frame.payload, payload);
}

TEST(RemoteProtocol, TryDecodeFrameConsumesOnlyFirstFrameFromTrailingBytes)
{
    std::vector<std::uint8_t> buffer = frameHeader(5, 1, static_cast<std::uint8_t>(MessageType::Hello));
    const std::vector<std::uint8_t> payload = ascii("TPE");
    buffer.insert(buffer.end(), payload.begin(), payload.end());
    // 本帧之后追加多余字节（含下一帧的部分字节），只应消费本帧。
    buffer.push_back(0xAB);
    buffer.push_back(0x00);
    buffer.push_back(0x00);
    buffer.push_back(0x00);

    const DecodeResult result = tpe::remote::tryDecodeFrame(buffer);

    ASSERT_EQ(result.status, DecodeStatus::Complete);
    EXPECT_EQ(result.consumed, std::size_t{9});
    EXPECT_EQ(result.frame.payload, payload);
}

TEST(RemoteProtocol, TryDecodeFramePointerOverloadDecodesSameFrame)
{
    std::vector<std::uint8_t> buffer = frameHeader(5, 1, static_cast<std::uint8_t>(MessageType::Hello));
    const std::vector<std::uint8_t> payload = ascii("TPE");
    buffer.insert(buffer.end(), payload.begin(), payload.end());
    buffer.push_back(0x99); // 尾部多余字节

    const DecodeResult result = tpe::remote::tryDecodeFrame(buffer.data(), buffer.size());

    ASSERT_EQ(result.status, DecodeStatus::Complete);
    EXPECT_EQ(result.consumed, std::size_t{9});
    EXPECT_EQ(result.frame.payload, payload);
}

TEST(RemoteProtocol, TryDecodeFrameAcceptsEmptyPayloadAtMinimumLength)
{
    // length = 2 = version + type，payload 为空：帧总字节数 6。
    const std::vector<std::uint8_t> buffer = frameHeader(2, 1, static_cast<std::uint8_t>(MessageType::Welcome));

    const DecodeResult result = tpe::remote::tryDecodeFrame(buffer);

    ASSERT_EQ(result.status, DecodeStatus::Complete);
    EXPECT_EQ(result.consumed, std::size_t{6});
    EXPECT_EQ(result.frame.header.length, std::uint32_t{2});
    EXPECT_EQ(result.frame.header.type, MessageType::Welcome);
    EXPECT_TRUE(result.frame.payload.empty());
}

// ---------------------------------------------------------------------------
// 帧解码：输入不足 → NeedMoreData（不消费）
// ---------------------------------------------------------------------------

TEST(RemoteProtocol, TryDecodeFrameNeedsMoreDataForEmptyOrShortLengthField)
{
    // 空输入
    const DecodeResult empty = tpe::remote::tryDecodeFrame(std::vector<std::uint8_t>{});
    EXPECT_EQ(empty.status, DecodeStatus::NeedMoreData);
    EXPECT_EQ(empty.consumed, std::size_t{0});

    // length 字段不足 4 字节
    const DecodeResult shortLength = tpe::remote::tryDecodeFrame(bytes({0x00, 0x00, 0x00}));
    EXPECT_EQ(shortLength.status, DecodeStatus::NeedMoreData);
    EXPECT_EQ(shortLength.consumed, std::size_t{0});
}

TEST(RemoteProtocol, TryDecodeFrameNeedsMoreDataForIncompletePayload)
{
    // length 声明 5，但只到 version + type，payload 缺失
    const DecodeResult result =
        tpe::remote::tryDecodeFrame(frameHeader(5, 1, static_cast<std::uint8_t>(MessageType::Hello)));

    EXPECT_EQ(result.status, DecodeStatus::NeedMoreData);
    EXPECT_EQ(result.consumed, std::size_t{0});
}

TEST(RemoteProtocol, TryDecodeFrameWaitsForMaximumLengthFrameInsteadOfFailing)
{
    // length = 1 MiB 为合法上界；数据不足时只能等待，不得判为 Failed。
    const DecodeResult result = tpe::remote::tryDecodeFrame(
        frameHeader(tpe::remote::kMaxFrameLength, 1, static_cast<std::uint8_t>(MessageType::Command)));

    EXPECT_EQ(result.status, DecodeStatus::NeedMoreData);
}

// ---------------------------------------------------------------------------
// 帧解码：length 非法 → Failed(FrameTooLarge)
// ---------------------------------------------------------------------------

TEST(RemoteProtocol, TryDecodeFrameRejectsLengthBelowMinimum)
{
    // length = 0：4 字节 length 字段已到齐，仍非法
    const DecodeResult zero = tpe::remote::tryDecodeFrame(bytes({0x00, 0x00, 0x00, 0x00}));
    ASSERT_EQ(zero.status, DecodeStatus::Failed);
    EXPECT_EQ(zero.error.code, ErrorCode::FrameTooLarge);

    // length = 1：帧声明的 5 字节已到齐，仍非法
    const DecodeResult one = tpe::remote::tryDecodeFrame(bytes({0x00, 0x00, 0x00, 0x01, 0x01}));
    ASSERT_EQ(one.status, DecodeStatus::Failed);
    EXPECT_EQ(one.error.code, ErrorCode::FrameTooLarge);
}

TEST(RemoteProtocol, TryDecodeFrameRejectsLengthAboveMaximumImmediately)
{
    // data-model §1：超过上限立即失败，不等待剩余字节。
    const DecodeResult over = tpe::remote::tryDecodeFrame(
        frameHeader(tpe::remote::kMaxFrameLength + 1, 1, static_cast<std::uint8_t>(MessageType::Command)));
    ASSERT_EQ(over.status, DecodeStatus::Failed);
    EXPECT_EQ(over.error.code, ErrorCode::FrameTooLarge);

    // uint32 上限同样属于超限
    const DecodeResult huge = tpe::remote::tryDecodeFrame(bytes({0xFF, 0xFF, 0xFF, 0xFF}));
    ASSERT_EQ(huge.status, DecodeStatus::Failed);
    EXPECT_EQ(huge.error.code, ErrorCode::FrameTooLarge);
}

// ---------------------------------------------------------------------------
// 帧解码：version / type 非法
// ---------------------------------------------------------------------------

TEST(RemoteProtocol, TryDecodeFrameRejectsUnsupportedVersion)
{
    for (const std::uint8_t version : {std::uint8_t{0}, std::uint8_t{2}}) {
        SCOPED_TRACE(static_cast<int>(version));
        const DecodeResult result = tpe::remote::tryDecodeFrame(
            frameHeader(2, version, static_cast<std::uint8_t>(MessageType::Hello)));
        EXPECT_EQ(result.status, DecodeStatus::Failed);
        EXPECT_EQ(result.error.code, ErrorCode::UnsupportedVersion);
    }
}

TEST(RemoteProtocol, TryDecodeFrameRejectsBadMessageType)
{
    for (const std::uint8_t type : {std::uint8_t{0}, std::uint8_t{8}, std::uint8_t{255}}) {
        SCOPED_TRACE(static_cast<int>(type));
        const DecodeResult result = tpe::remote::tryDecodeFrame(frameHeader(2, 1, type));
        EXPECT_EQ(result.status, DecodeStatus::Failed);
        EXPECT_EQ(result.error.code, ErrorCode::BadMessageType);
    }
}

// ---------------------------------------------------------------------------
// 载荷校验（contracts/protocol.md §2/§4；data-model.md §2）
// ---------------------------------------------------------------------------

TEST(RemoteProtocol, ValidatePayloadAcceptsWellFormedPayloads)
{
    EXPECT_FALSE(tpe::remote::validatePayload(MessageType::Hello, ascii("TPE")).has_value());
    EXPECT_FALSE(tpe::remote::validatePayload(MessageType::Welcome, {}).has_value());
    EXPECT_FALSE(tpe::remote::validatePayload(MessageType::Busy, ascii("Server is busy.")).has_value());
    EXPECT_FALSE(tpe::remote::validatePayload(MessageType::Error, bytes({0x01, 'b', 'a', 'd'})).has_value());
    EXPECT_FALSE(tpe::remote::validatePayload(MessageType::Command, ascii("--all-processes")).has_value());
    EXPECT_FALSE(tpe::remote::validatePayload(MessageType::Bye, {}).has_value());
}

TEST(RemoteProtocol, ValidatePayloadRejectsNonEmptyBye)
{
    // 契约 §2：BYE 载荷为空；Server 收到非空 BYE → ERROR(6)。
    const auto error = tpe::remote::validatePayload(MessageType::Bye, bytes({0x00}));
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, ErrorCode::InvalidPayload);
}

TEST(RemoteProtocol, ValidatePayloadRejectsNonEmptyWelcome)
{
    const auto error = tpe::remote::validatePayload(MessageType::Welcome, ascii("hello"));
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, ErrorCode::InvalidPayload);
}

TEST(RemoteProtocol, ValidatePayloadRejectsBusyTextNotMatchingFixedText)
{
    // 契约 §2：BUSY payload 为固定文本 "Server is busy."；其它文本（含空载荷）非法。
    for (const std::string& payload : {std::string("busy"), std::string("Server is busy"), std::string()}) {
        SCOPED_TRACE(payload);
        const auto error = tpe::remote::validatePayload(MessageType::Busy, ascii(payload));
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->code, ErrorCode::InvalidPayload);
    }
}

TEST(RemoteProtocol, ValidatePayloadEnforcesHelloClientIdLimit)
{
    // clientId ≤ 64 字节：64 合法，65 非法。
    const std::size_t limit = tpe::remote::kMaxClientIdBytes;
    EXPECT_FALSE(
        tpe::remote::validatePayload(MessageType::Hello, std::vector<std::uint8_t>(limit, 'c')).has_value());

    const auto error =
        tpe::remote::validatePayload(MessageType::Hello, std::vector<std::uint8_t>(limit + 1, 'c'));
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, ErrorCode::InvalidPayload);
}

TEST(RemoteProtocol, ValidatePayloadEnforcesCommandSizeLimit)
{
    // 契约 §1/§2：COMMAND payload ≤ 64 KiB；超限属于已知类型载荷非法（ERROR 码 6）。
    const std::size_t limit = tpe::remote::kMaxCommandBytes;
    EXPECT_FALSE(
        tpe::remote::validatePayload(MessageType::Command, std::vector<std::uint8_t>(limit, 'a')).has_value());

    const auto error =
        tpe::remote::validatePayload(MessageType::Command, std::vector<std::uint8_t>(limit + 1, 'a'));
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, ErrorCode::InvalidPayload);
}

// ---------------------------------------------------------------------------
// RESPONSE 编码（contracts/protocol.md §2/§8；data-model.md §4）
// ---------------------------------------------------------------------------

TEST(RemoteProtocol, EncodeResponseWritesContractLayout)
{
    const std::string prompt = "TPE> ";          // 尾随空格不上线（由渲染层添加）
    const std::string output = "line1\nline2\n"; // 12 字节
    const auto encoded = tpe::remote::encodeResponse(ResponseStatus::CommandError, prompt, output);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().reason;

    const std::vector<std::uint8_t>& data = encoded.value();
    ASSERT_EQ(data.size(), std::size_t{1} + 2 + 4 + 4 + output.size());

    EXPECT_EQ(data[0], std::uint8_t{0x02}); // status = CommandError
    EXPECT_EQ(data[1], std::uint8_t{0x00}); // promptLen 高字节
    EXPECT_EQ(data[2], std::uint8_t{0x04}); // promptLen 低字节
    EXPECT_EQ(std::vector<std::uint8_t>(data.begin() + 3, data.begin() + 7), ascii("TPE>"));
    // outputLen（4 字节大端）紧跟在 prompt 之后
    EXPECT_EQ(data[7], std::uint8_t{0x00});
    EXPECT_EQ(data[8], std::uint8_t{0x00});
    EXPECT_EQ(data[9], std::uint8_t{0x00});
    EXPECT_EQ(data[10], std::uint8_t{0x0C});
    EXPECT_EQ(std::vector<std::uint8_t>(data.begin() + 11, data.end()), ascii(output));
}

TEST(RemoteProtocol, EncodeResponseWritesMultiByteBigEndianLengths)
{
    const std::string prompt(300, 'p');   // 300 = 0x012C
    const std::string output(70000, 'o'); // 70000 = 0x00011170
    const auto encoded = tpe::remote::encodeResponse(ResponseStatus::Continue, prompt, output);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().reason;

    const std::vector<std::uint8_t>& data = encoded.value();
    ASSERT_EQ(data.size(), std::size_t{1} + 2 + prompt.size() + 4 + output.size());

    EXPECT_EQ(data[0], std::uint8_t{0x00}); // status = Continue
    EXPECT_EQ(data[1], std::uint8_t{0x01}); // promptLen 大端
    EXPECT_EQ(data[2], std::uint8_t{0x2C});

    const std::size_t outputLenOffset = 3 + prompt.size();
    EXPECT_EQ(data[outputLenOffset + 0], std::uint8_t{0x00}); // outputLen 大端
    EXPECT_EQ(data[outputLenOffset + 1], std::uint8_t{0x00});
    EXPECT_EQ(data[outputLenOffset + 2], std::uint8_t{0x01});
    EXPECT_EQ(data[outputLenOffset + 3], std::uint8_t{0x11});
    EXPECT_EQ(data[outputLenOffset + 4], std::uint8_t{0x70});
}

TEST(RemoteProtocol, EncodeResponseRejectsPromptLongerThanUint16)
{
    // promptLen 为 uint16：65536 字节不可编码。
    const std::string prompt(tpe::remote::kMaxPromptBytes + 1, 'p');
    EXPECT_FALSE(tpe::remote::encodeResponse(ResponseStatus::Continue, prompt, "").has_value());
}

TEST(RemoteProtocol, EncodeResponseEnforcesPayloadSizeLimit)
{
    // 编码结果即 RESPONSE payload：总字节数不得超过 kMaxPayloadBytes。
    const std::string prompt = "TPE>";
    const std::size_t overhead = 1 + 2 + prompt.size() + 4; // status + promptLen + prompt + outputLen
    const std::string maxOutput(tpe::remote::kMaxPayloadBytes - overhead, 'o');

    const auto atLimit = tpe::remote::encodeResponse(ResponseStatus::Continue, prompt, maxOutput);
    ASSERT_TRUE(atLimit.has_value()) << atLimit.error().reason;
    EXPECT_EQ(atLimit.value().size(), std::size_t{tpe::remote::kMaxPayloadBytes});

    const std::string overLimit(tpe::remote::kMaxPayloadBytes - overhead + 1, 'o');
    EXPECT_FALSE(tpe::remote::encodeResponse(ResponseStatus::Continue, prompt, overLimit).has_value());
}

// ---------------------------------------------------------------------------
// RESPONSE 解码（contracts/protocol.md §8；data-model.md §4）
// ---------------------------------------------------------------------------

TEST(RemoteProtocol, DecodeResponseAcceptsContractExamplePayload)
{
    // 契约 §8 示例形状：status=0、promptLen=4 "TPE>"、outputLen=2 "ok"。
    const std::vector<std::uint8_t> payload =
        bytes({0x00, 0x00, 0x04, 'T', 'P', 'E', '>', 0x00, 0x00, 0x00, 0x02, 'o', 'k'});

    const auto decoded = tpe::remote::decodeResponse(payload);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().reason;

    const RemoteResponse& response = decoded.value();
    EXPECT_EQ(response.status, ResponseStatus::Continue);
    EXPECT_EQ(response.prompt, "TPE>");
    EXPECT_EQ(response.output, "ok");
}

TEST(RemoteProtocol, DecodeResponseRestoresEncodedFields)
{
    const auto encoded =
        tpe::remote::encodeResponse(ResponseStatus::CommandError, "TPE> ", "line1\nline2\n");
    ASSERT_TRUE(encoded.has_value()) << encoded.error().reason;

    const auto decoded = tpe::remote::decodeResponse(encoded.value());
    ASSERT_TRUE(decoded.has_value()) << decoded.error().reason;

    const RemoteResponse& response = decoded.value();
    EXPECT_EQ(response.status, ResponseStatus::CommandError);
    EXPECT_EQ(response.prompt, "TPE>");      // 去尾随空格
    EXPECT_EQ(response.output, "line1\nline2\n"); // 保留 LF
}

TEST(RemoteProtocol, DecodeResponseRejectsMalformedPayloads)
{
    // 手工构造的合法 RESPONSE：status=0, promptLen=4 "TPE>", outputLen=2 "ok"
    const std::vector<std::uint8_t> valid =
        bytes({0x00, 0x00, 0x04, 'T', 'P', 'E', '>', 0x00, 0x00, 0x00, 0x02, 'o', 'k'});

    const std::vector<std::vector<std::uint8_t>> malformed = {
        {},                                                                   // 空载荷
        bytes({0x00}),                                                        // 缺 promptLen
        bytes({0x00, 0x00}),                                                  // promptLen 只有 1 字节
        bytes({0x00, 0x00, 0x04, 'T', 'P'}),                                  // prompt 截断
        bytes({0x00, 0x00, 0x04, 'T', 'P', 'E', '>'}),                        // 缺 outputLen
        bytes({0x00, 0x00, 0x04, 'T', 'P', 'E', '>', 0x00, 0x00}),            // outputLen 不足 4 字节
        bytes({0x00, 0x00, 0x04, 'T', 'P', 'E', '>', 0x00, 0x00, 0x00, 0x10, 'o', 'k'}), // output 越界
        bytes({0x00, 0x00, 0xFF, 0xFF, 'T'}),                                 // promptLen 越界
        bytes({0x00, 0x00, 0x04, 'T', 'P', 'E', '>', 0xFF, 0xFF, 0xFF, 0xFF}), // outputLen 越界
    };

    std::size_t index = 0;
    for (const auto& payload : malformed) {
        SCOPED_TRACE(static_cast<int>(index));
        EXPECT_FALSE(tpe::remote::decodeResponse(payload).has_value());
        ++index;
    }

    // 合法载荷后追加多余字节 → 结构非法
    std::vector<std::uint8_t> extra = valid;
    extra.push_back('X');
    SCOPED_TRACE("extra trailing byte");
    EXPECT_FALSE(tpe::remote::decodeResponse(extra).has_value());
}

// ---------------------------------------------------------------------------
// 文本保真：UTF-8 多字节字符与 LF（data-model §1；FR-017）
// ---------------------------------------------------------------------------

TEST(RemoteProtocol, FrameRoundTripPreservesUtf8AndLfBytes)
{
    // 协议层只搬运字节：不得按字符计数、不得转换行分隔符。
    const std::string line = "扫描 进程\nPID 1234";
    const std::vector<std::uint8_t> payload(line.begin(), line.end());

    const std::vector<std::uint8_t> frame =
        tpe::remote::encodeFrame(MessageType::Command, payload);
    ASSERT_EQ(frame.size(), std::size_t{4} + 2 + payload.size()); // length 按字节计
    EXPECT_EQ(frame[3], static_cast<std::uint8_t>(2 + payload.size()));

    const DecodeResult result = tpe::remote::tryDecodeFrame(frame);
    ASSERT_EQ(result.status, DecodeStatus::Complete);
    EXPECT_EQ(result.frame.header.type, MessageType::Command);
    EXPECT_EQ(result.frame.payload, payload);
    EXPECT_EQ(toText(result.frame.payload), line);
}

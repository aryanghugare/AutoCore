#include "vaecu/messages.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace {
using namespace vaecu;
namespace msg = vaecu::messages;

TEST(MessageCodecTest, EngineEncodingMatchesDocumentedLittleEndianBytes)
{
    const auto frame = msg::encode_engine({3000, 88}, Timestamp{});
    const CanFrame::Payload expected{0xB8, 0x0B, 0x58, 0x00, 0, 0, 0, 0};
    EXPECT_EQ(frame.id(), CanId{0x100});
    EXPECT_EQ(frame.payload_size(), 4U);
    EXPECT_EQ(frame.payload(), expected);
}

TEST(MessageCodecTest, EngineDecodingAcceptsIndependentGoldenBytes)
{
    const CanFrame frame{CanId{0x100}, {0x28, 0x23, 0x2C, 0x01}, 4, Timestamp{}};
    const auto value = msg::decode_engine(frame);
    ASSERT_TRUE(value);
    EXPECT_EQ(value->engine_rpm, 9000);
    EXPECT_EQ(value->vehicle_speed_kph, 300);
    const auto zero = msg::decode_engine(msg::encode_engine({}, Timestamp{}));
    ASSERT_TRUE(zero);
    EXPECT_EQ(zero->engine_rpm, 0);
    EXPECT_EQ(zero->vehicle_speed_kph, 0);
}

TEST(MessageCodecTest, RejectsEngineValuesOutsideSimulationRanges)
{
    EXPECT_THROW((void)msg::encode_engine({9001, 0}, Timestamp{}), std::invalid_argument);
    EXPECT_THROW((void)msg::encode_engine({0, 301}, Timestamp{}), std::invalid_argument);
    EXPECT_FALSE(msg::decode_engine(CanFrame{msg::engine_telemetry_id,
        {0x29, 0x23, 0, 0}, 4, Timestamp{}}));
    EXPECT_FALSE(msg::decode_engine(CanFrame{msg::engine_telemetry_id,
        {0, 0, 0x2D, 0x01}, 4, Timestamp{}}));
}

TEST(MessageCodecTest, EveryCodecRequiresItsExactPayloadLength)
{
    for (std::size_t size = 0; size <= CanFrame::max_payload_size; ++size) {
        SCOPED_TRACE(size);
        EXPECT_EQ(msg::decode_engine(CanFrame{msg::engine_telemetry_id, {}, size,
            Timestamp{}}).has_value(), size == 4);
        EXPECT_EQ(msg::decode_brake(CanFrame{msg::brake_status_id, {}, size,
            Timestamp{}}).has_value(), size == 1);
        EXPECT_EQ(msg::decode_brake_event(CanFrame{msg::brake_event_id, {1}, size,
            Timestamp{}}).has_value(), size == 1);
    }
}

TEST(MessageCodecTest, EveryCodecRejectsOtherIdentifiers)
{
    const CanId unsupported{0x7FF};
    EXPECT_FALSE(msg::decode_engine(CanFrame{unsupported, {}, 4, Timestamp{}}));
    EXPECT_FALSE(msg::decode_brake(CanFrame{unsupported, {}, 1, Timestamp{}}));
    EXPECT_FALSE(msg::decode_brake_event(CanFrame{unsupported, {1}, 1, Timestamp{}}));
}

TEST(MessageCodecTest, BrakeStatusUsesOnlyZeroAndOne)
{
    for (const bool applied : {false, true}) {
        const auto frame = msg::encode_brake({applied}, Timestamp{});
        EXPECT_EQ(frame.id(), CanId{0x200});
        EXPECT_EQ(frame.payload_size(), 1U);
        EXPECT_EQ(frame.payload()[0], applied ? 1 : 0);
        const auto decoded = msg::decode_brake(frame);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->applied, applied);
    }
    for (const std::uint8_t invalid : {std::uint8_t{2}, std::uint8_t{255}}) {
        EXPECT_FALSE(msg::decode_brake(CanFrame{msg::brake_status_id, {invalid}, 1,
            Timestamp{}}));
    }
}

TEST(MessageCodecTest, BrakeEventsHaveExplicitApplyAndReleaseCodes)
{
    for (const auto event : {msg::BrakeEvent::Apply, msg::BrakeEvent::Release}) {
        const auto frame = msg::encode_brake_event(event, Timestamp{});
        EXPECT_EQ(frame.id(), CanId{0x300});
        EXPECT_EQ(frame.payload_size(), 1U);
        EXPECT_EQ(frame.payload()[0], static_cast<std::uint8_t>(event));
        ASSERT_TRUE(msg::decode_brake_event(frame));
        EXPECT_EQ(*msg::decode_brake_event(frame), event);
    }
    EXPECT_THROW((void)msg::encode_brake_event(static_cast<msg::BrakeEvent>(0),
        Timestamp{}), std::invalid_argument);
    EXPECT_FALSE(msg::decode_brake_event(CanFrame{msg::brake_event_id, {0}, 1,
        Timestamp{}}));
    EXPECT_FALSE(msg::decode_brake_event(CanFrame{msg::brake_event_id, {255}, 1,
        Timestamp{}}));
}

} // namespace
